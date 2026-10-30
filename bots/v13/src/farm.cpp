// farm.cpp - group scoring, targets, standing around monsters, and capture. [S4, S5]
//
// Standing rules (monsters hit the NEAREST champion in range and never chase):
//   melee champions (Marcella first - she acts first) stand next to a monster on the tile the fewest
//   monsters reach, so two melee split a pack's hits; ranged champions shoot from distance 3 only from
//   a "covered" tile, where every monster that reaches them has one of our champions nearer to it
//   ("tank rule"). With nobody covering, a ranged champion whose spell range (4) beats the monsters'
//   range (wolves 3) casts from distance 4 and takes no damage; otherwise it holds just out of reach.
// Worked example (feasibility): Marcella (1,200 health) + Stellar vs one elk (2,400 health, 60 strength).
//   Squad damage per round ~ 60 + 60 = 120 -> kill time 20 rounds -> tank damage 60 x 20 = 1,200
//   > 0.8 x 1,200 = 960 -> not feasible in one trip. Multi-trip: 60 x (range 4 + 1) = 300 <= 0.3 x 1,200
//   = 360 -> she can always walk out alive, so it is feasible, paying one trip home per health bar.
#include "farm.h"
#include <algorithm>
#include <iostream>
#include "actions.h"
#include "bank.h"
#include "combat.h"
#include "comms.h"
#include "config.h"
#include "mapinfo.h"
#include "move.h"
#include "periodic.h"
#include "shop.h"
#include "world.h"

namespace farm {

namespace {
int g_last_gold = -1, g_earned = 0;   // v13: lifetime gold earned (zero-gold capture rule)
}

void note_gold(int gold_now) {
    if (g_last_gold >= 0 && gold_now > g_last_gold) g_earned += gold_now - g_last_gold;
    g_last_gold = gold_now;
}
namespace {

int g_current_group = NO_GROUP;

bool is_ranged_type(bc::champion_type t) {
    return t == bc::champion_type::stellar || t == bc::champion_type::cassi || t == bc::champion_type::prism ||
           t == bc::champion_type::tavi || t == bc::champion_type::vesper_vale || t == bc::champion_type::willow;
}

// Tank preference [S5]: Marcella, else Heart of Ruin, else Issac, else any melee, else the first member.
bc::champion_type pick_tank(const std::vector<bc::champion_type>& members) {
    const bc::champion_type order[] = {bc::champion_type::marcella, bc::champion_type::ryth, bc::champion_type::heart_of_ruin,
                                       bc::champion_type::vendetta, bc::champion_type::issac};
    for (bc::champion_type t : order)
        if (std::find(members.begin(), members.end(), t) != members.end()) return t;
    for (bc::champion_type t : members)
        if (!is_ranged_type(t)) return t;
    return members.empty() ? bc::champion_type::marcella : members.front();
}

bool has_jungle_emblem() {
    const auto counts = bc::self().item_count();
    return counts[bc::item_type::jungle_emblem_of_health] + counts[bc::item_type::jungle_emblem_of_strength] +
           counts[bc::item_type::jungle_emblem_of_focus] > 0;
}

// v11: capture time for planning. The engine cuts it to 3 rounds while a Capture-emblem holder stands
// on the region; our buyer is the squad's lowest body slot, which capture_step leaves on the region,
// so "I hold it" is the common case for the champion actually planning the trip.
// FUEL: cached per round - group_value asks for this once per group, and capture_emblem_ready reads
// the engine's item table (~1k fuel; 40 groups x that per rescore is what ran lanes out of budget).
int capture_rounds(const Snapshot& s) {
    static int cached_round = -1;
    static int cached = CAPTURE_ROUNDS;
    if (cached_round != s.round) {
        cached = shop::capture_emblem_ready(s) ? CAPTURE_EMBLEM_ROUNDS : CAPTURE_ROUNDS;
        cached_round = s.round;
    }
    return cached;
}

// Groups we walked away from because an enemy we would lose to stood in reach: skipped until the
// stored round, so we do not step back onto the capture tile every other turn (gold-rush, Oct 24:
// Heart of Ruin alternated capture / walk-away at one group for 500 turns).
std::vector<int>& avoid_until() {
    static std::vector<int> until;
    if (until.size() != mapinfo::groups().size()) until.assign(mapinfo::groups().size(), -1);
    return until;
}

bool avoided(int g) {
    if (g < 0 || g >= static_cast<int>(avoid_until().size())) return false;
    return avoid_until()[static_cast<std::size_t>(g)] >= g_round;   // g_round: no engine call (~300 fuel)
}

std::vector<int> claimed_by_others(int squad) {
    std::vector<int> out;
    for (std::size_t g = 0; g < avoid_until().size(); ++g)
        if (avoided(static_cast<int>(g))) out.push_back(static_cast<int>(g));
    for (int other = 0; other < 3; ++other)
        if (other != squad && comms::squad_target(other) != NO_GROUP) out.push_back(comms::squad_target(other));
    if (squad != 0 && comms::split_target() != NO_GROUP) out.push_back(comms::split_target());
    return out;
}

bool contains(const std::vector<int>& list, int v) { return std::find(list.begin(), list.end(), v) != list.end(); }

// Damage per round on ONE melee champion: monsters hit the nearest champion, so with two melee next to
// two different monsters each takes one monster's hits. Example: 2 elks, Marcella + Ren -> 60 each;
// stampede's pack of 6 elks: 3 reach even the best edge tile -> 180, shared by two melee -> 90 each.
// The tank stands on the group's best edge tile (mapinfo edge_strength = fewest monsters reach it).
double hitting_one_tank(const GroupInfo& g, const SquadStats& squad) {
    const int sharing = std::clamp(squad.melee_count, 1, MAX_MONSTERS_HITTING);
    return std::max<double>(g.max_strength, g.edge_strength / sharing);
}

// Kill time and tank damage for one group and squad.
struct Cost { double kill_time = 0, tank_damage = 0; bool kite = false; };

Cost cost_of(const GroupInfo& g, const SquadStats& squad) {
    Cost c;
    // Ranged-only squad vs monsters whose range is shorter than our spells (4): no damage taken.
    c.kite = !squad.has_melee && g.max_attack_range < 4 && squad.spell_dps > 0;
    const double dps = c.kite ? squad.spell_dps : squad.dps;
    c.kill_time = g.total_health / std::max(1.0, dps);
    c.tank_damage = c.kite ? 0.0 : hitting_one_tank(g, squad) * c.kill_time;
    return c;
}

std::vector<const CampNow*> live_camps_of_group_visible(const Snapshot& s, int group) {
    std::vector<const CampNow*> out;
    for (const CampNow& c : s.camps)
        if (c.group == group) out.push_back(&c);
    return out;
}

bool target_finished(const Snapshot& s, int g) {
    if (g < 0 || g >= static_cast<int>(mapinfo::groups().size())) return true;
    if (avoided(g)) return true;
    const GroupInfo& gr = mapinfo::groups()[static_cast<std::size_t>(g)];
    return gr.state == GroupState::Respawning && gr.respawn_ready_round - s.round > RESPAWN_HOLD;
}

}  // namespace

namespace {

SquadStats stats_of_members(const Snapshot& s, std::vector<bc::champion_type> members) {
    SquadStats st;
    st.members = std::move(members);
    if (st.members.empty()) st.members.push_back(s.my_type);
    for (bc::champion_type t : st.members) {
        const bc::champion_stats base = combat::base_stats_of(t);
        const float strength = t == s.my_type ? s.strength : base.base_strength;
        const float focus = t == s.my_type ? s.focus : base.base_focus;
        st.dps += combat::damage_per_round(t, strength, focus);
        st.spell_dps += combat::damage_per_round(t, 0.0f, focus);
        st.has_melee = st.has_melee || !is_ranged_type(t);
        st.melee_count += is_ranged_type(t) ? 0 : 1;
    }
    st.tank = pick_tank(st.members);
    const bc::champion_stats tank_base = combat::base_stats_of(st.tank);
    st.tank_max_health = st.tank == s.my_type ? s.max_health : tank_base.base_health;
    st.tank_health = st.tank == s.my_type ? s.health : st.tank_max_health;
    st.tank_has_jungle = st.tank == s.my_type ? has_jungle_emblem()
                                              : (st.tank == bc::champion_type::marcella && comms::flag(comms::FLAG_MARCELLA_JUNGLE));
    return st;
}

}  // namespace

SquadStats squad_stats(const Snapshot& s, int squad) {
    // Cached per (round, squad): target scoring asks for it once per group.
    static int cached_round = -1, cached_squad = -1;
    static SquadStats cached;
    if (cached_round == s.round && cached_squad == squad) return cached;
    cached = stats_of_members(s, world::squad_members(squad, true));
    cached_round = s.round;
    cached_squad = squad;
    return cached;
}

bool group_feasible(const GroupInfo& g, const SquadStats& squad) {
    if (g.tiles.empty() || g.steps_from_us >= FAR_AWAY) return false;
    Cost c = cost_of(g, squad);
    const double jungle = squad.tank_has_jungle ? JUNGLE_FACTOR : 1.0;
    if (c.tank_damage * jungle <= TANK_FEASIBLE_SHARE * squad.tank_health) return true;   // one trip
    if (!ALLOW_MULTI_TRIP) return false;
    // Multi-trip: when the tank retreats (at RETREAT_HEALTH) it must survive walking out of range.
    // Example: 2 elks (60 each, range 4) on Marcella alone: 120 x 5 rounds = 600 > 0.3 x 1,200 = 360 ->
    // not safe; with Ren taking the second elk: 60 x 5 = 300 <= 360 -> safe.
    const double hitting = hitting_one_tank(g, squad) * jungle;
    return hitting * (g.max_attack_range + 1) <= RETREAT_HEALTH * squad.tank_max_health;
}

// Step 0c: rounds to walk to the group - the real walk once its field exists (tangle: group 28 was 9
// tiles away in a straight line and ~60 steps by path), else the old 1.2 x straight-line estimate.
double walk_rounds(const GroupInfo& g, const Snapshot& s) {
    const int steps = mapinfo::steps_to_group(g.index, s.me);
    return steps >= 0 ? static_cast<double>(steps) : 1.2 * static_cast<double>(g.centre.chebyshev_distance(s.me));
}

double group_value(const GroupInfo& g, const SquadStats& squad, const Snapshot& s) {
    double reward = g.gems + LAMBDA_GEMS_PER_GOLD * g.gold;
    if (g.gems == 0) reward *= GOLD_ONLY_FACTOR;
    // v12 middle control: while we are not behind on the scoreboard, middle-band groups are worth a
    // little extra - they are the ground the altars sit on, and conceding them is how a lead around
    // the middle becomes "the enemy owns the middle" (match 8d0f4189: two mid altars, we drifted home).
    if (g.in_middle_band && s.banked_us >= s.banked_them - MIDDLE_BEHIND_GEMS) reward *= MIDDLE_CONTEST_BONUS;
    const Cost c = cost_of(g, squad);
    const double walk = walk_rounds(g, s);
    double wait = 0;
    if (g.state == GroupState::Respawning) wait = std::max(0.0, g.respawn_ready_round - s.round - walk);
    // Each tank health bar spent = one recall home and the walk back to this group.
    const double jungle = squad.tank_has_jungle ? JUNGLE_FACTOR : 1.0;
    const double bars = c.tank_damage * jungle / std::max(1.0, squad.tank_max_health * (1.0 - RETREAT_HEALTH));
    const double healing = bars * (RECALL_TURNS + g.steps_from_us);
    return reward / std::max(1.0, walk + wait + c.kill_time + capture_rounds(s) + healing);
}

namespace {

bool interceptor_like(Role r) {
    return r == Role::Interceptor || r == Role::FarPair || r == Role::DisruptorCaster || r == Role::DisruptorMelee;
}

// [S4] Interceptor / far pair / disruptors prefer the middle band and the enemy half (outside the zone);
// the interceptor takes only clusters and wolves.
bool preferred_area(const GroupInfo& g, Role role) {
    if (!interceptor_like(role)) return true;
    if (!(g.in_middle_band || mapinfo::in_enemy_half(g.centre))) return false;
    if (role == Role::Interceptor) return g.clusters_only || g.max_strength <= 20.0f;   // clusters, wolves
    return true;
}

// 5.8.1 Omar's enemy-zone rule: enter only close, healthy and light; then farm only if P_die is low.
bool zone_rule_allows(const GroupInfo& g, const Snapshot& s) {
    if (!g.in_enemy_zone) return true;
    if (g.centre.chebyshev_distance(s.me) > ZONE_ENTRY_RANGE) return false;
    if (s.health < ZONE_ENTRY_HEALTH * s.max_health || s.bag > ZONE_ENTRY_BAG) return false;
    return bank::p_die(s, g.steps_to_altar + static_cast<int>(walk_rounds(g, s))) <= ZONE_FARM_MAX_PDIE;
}

// Upper bound of group_value (no wait, kill or healing time): lets the scans below skip the costly
// feasibility test for groups that cannot beat the best so far. FUEL: a full rescore cost ~65k on
// road-of-riches (Oct 24) and ran the turn out of fuel every 10 rounds.
double value_upper_bound(const GroupInfo& g, const Snapshot& s) {
    double reward = g.gems + LAMBDA_GEMS_PER_GOLD * g.gold;
    if (g.gems == 0) reward *= GOLD_ONLY_FACTOR;
    return reward / std::max(1.0, walk_rounds(g, s) + CAPTURE_ROUNDS);
}

// Group indices by upper bound, best first. Step 0: cached per round and tile - the sort cost ~17k on
// 01islands and choose_target ran it twice a turn (plus once for the side job).
const std::vector<std::pair<double, int>>& by_bound(const Snapshot& s) {
    static std::vector<std::pair<double, int>> order;
    static int cached_round = -1;
    static vec2 cached_at{-1, -1};
    if (cached_round == s.round && cached_at == s.me) return order;
    cached_round = s.round;
    cached_at = s.me;
    order.clear();
    for (const GroupInfo& g : mapinfo::groups())
        if ((g.gems + g.gold) > 0) order.push_back({value_upper_bound(g, s), g.index});
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    return order;
}

int best_group(const Snapshot& s, const SquadStats& squad, const std::vector<int>& claimed, bool preferred_only,
               bool clusters_only) {
    int best = NO_GROUP;
    double best_value = 0;
    for (const auto& [bound, index] : by_bound(s)) {
        if (bound <= best_value) break;
        if (bc::fuel_remaining() < FUEL_SCAN_FLOOR) break;   // step 0: every scan stops here, keeping the best so far
        const GroupInfo& g = mapinfo::groups()[static_cast<std::size_t>(index)];
        if (contains(claimed, g.index)) continue;
        if (clusters_only && !g.clusters_only) continue;
        if (preferred_only && !preferred_area(g, s.role)) continue;
        if (!group_feasible(g, squad) || !zone_rule_allows(g, s)) continue;
        const double v = group_value(g, squad, s);
        if (v > best_value) best_value = v, best = g.index;
    }
    return best;
}

}  // namespace

// Should my squad join squad 0's monster group instead of farming alone? Yes when the combined force
// earns more there than both squads apart. Example (ambush): Marcella + Ren need 46 rounds and three
// trips home for two elks; with Heart of Ruin + Stellar too it is 22 rounds and one trip.
bool join_is_better(const Snapshot& s, int squad, int join, int alone) {
    const GroupInfo& g = mapinfo::groups()[static_cast<std::size_t>(join)];
    if (g.clusters_only) return false;
    std::vector<bc::champion_type> both = world::squad_members(0, true);
    for (bc::champion_type t : world::squad_members(squad, true)) both.push_back(t);
    const SquadStats together = stats_of_members(s, both);
    if (!group_feasible(g, together)) return false;
    const SquadStats theirs = stats_of_members(s, world::squad_members(0, true));
    const SquadStats mine = squad_stats(s, squad);
    double apart = group_feasible(g, theirs) ? group_value(g, theirs, s) : 0.0;
    if (alone != NO_GROUP) apart += group_value(mapinfo::groups()[static_cast<std::size_t>(alone)], mine, s);
    return group_value(g, together, s) > apart;
}

int choose_target(const Snapshot& s, int squad) {
    // v12 harassment merge: while an enemy kill squad is taking us apart (match 8d0f4189: 21 deaths
    // with the pair farming alone across the map), squad 1 farms squad 0's group - five bodies are
    // not free kills. Deaths only count with a recent enemy sighting: on big monster maps (expanse,
    // the-farm) camp deaths alone must not merge the squads - the split IS the throughput there
    // (bench Oct 26: without this gate v12 lost both sides on 17 of 70 maps).
    // Hysteresis: once merged, stay merged for two windows after the last death.
    static int merged_until = -1;
    if (squad != 0) {
        const comms::Sighting seen = comms::read_sighting();
        if (world::team_deaths_since(g_round - HARASS_WINDOW) >= HARASS_DEATHS && seen.age <= 15)
            merged_until = g_round + 2 * HARASS_WINDOW;
        if (g_round < merged_until) {
            const int join = comms::squad_target(0);
            if (join != NO_GROUP) return join;
        }
    }
    const SquadStats st = squad_stats(s, squad);
    const std::vector<int> claimed = claimed_by_others(squad);
    int best = best_group(s, st, claimed, true, false);
    if (best == NO_GROUP) best = best_group(s, st, claimed, false, false);   // anything we can take
    const int join = comms::squad_target(0);
    // Step 0c: the join test (~9k) is optional - skipped once the scan reached its fuel floor (01islands).
    if (squad != 0 && join != NO_GROUP && join != best && bc::fuel_remaining() >= FUEL_SCAN_FLOOR && join_is_better(s, squad, join, best)) best = join;
    return best;
}

namespace {

// The lowest body slot alive in the squad chooses the target; the others follow it.
bool i_choose_for_squad(const Snapshot& s) {
    for (bc::champion_type t : world::squad_members(s.squad, true))
        if (body_slot_of(t) != NO_SLOT && body_slot_of(t) < s.my_slot) return false;
    return true;
}

// The squad's shared target. Re-chosen when missing or finished (respawning for long); the chooser
// also re-checks every 10 rounds and switches only to a clearly better group (1.5x the value), so the
// squad does not flip between two targets as it walks.
int choose_or_keep(const Snapshot& s, int target, bool must, bool chooser);

int squad_target(const Snapshot& s) {
    const int target = comms::squad_target(s.squad);
    const bool chooser = i_choose_for_squad(s);
    const bool must = target == NO_GROUP || target_finished(s, target);
    // Step 0c: the periodic re-check (~20k-52k) runs only with its measured cost + margin left, and
    // staggered by body slot (periodic.h). It ran turns dry on 01islands under the old flat 70k rule.
    static PeriodicJob rescore{RESCORE_ROUNDS, RESCORE_COST_GUESS};
    const bool periodic = chooser && rescore.due(s.round, s.my_slot);
    if (!must && !periodic) return target;
    const std::uint64_t fuel_before = bc::fuel_remaining();
    const int result = choose_or_keep(s, target, must, chooser);
    if (periodic) rescore.ran(s.round, fuel_before - std::min(fuel_before, bc::fuel_remaining()));
    return result;
}

int choose_or_keep(const Snapshot& s, int target, bool must, bool chooser) {
    // v13 task 3: with NO target at all, choosing one is the most important thing this turn - v12 waited
    // for 70k fuel, which a turn on rushbait-v2 never had left after sensing (the team idled all game and
    // never tributed). The scan below stops early when fuel runs low, keeping the best group found.
    const std::uint64_t need = target == NO_GROUP ? FUEL_FOR_FIRST_TARGET : FUEL_FOR_RESCORE;
    if (must && bc::fuel_remaining() < need) return target;   // choose next turn
    if (!must && !live_camps_of_group_visible(s, target).empty()) return target;   // mid-fight: keep it
    const int fresh = choose_target(s, s.squad);
    if (fresh == target || fresh == NO_GROUP) {
        if (must && fresh == NO_GROUP && target != NO_GROUP && chooser) comms::set_squad_target(s.squad, NO_GROUP);
        return must ? fresh : target;
    }
    if (!must) {
        const SquadStats st = squad_stats(s, s.squad);
        const auto& groups = mapinfo::groups();
        const double now = group_value(groups[static_cast<std::size_t>(target)], st, s);
        // v12: a mid-walk switch must beat the current target clearly (1.8x, was 1.5x): the weaker
        // margin let the chooser flip between two similar groups and the squad roamed between them
        // (match 8d0f4189 rounds 50-100: Marcella walking back and forth with no goal).
        if (group_value(groups[static_cast<std::size_t>(fresh)], st, s) < GROUP_SWITCH_FACTOR * now) return target;
    }
    comms::set_squad_target(s.squad, fresh);
    return fresh;
}

// [S4] Farming pair on clusters: the second farmer takes its own cluster group (byte 1).
int split_target(const Snapshot& s, int squad_goal) {
    const bool squad_on_clusters = squad_goal == NO_GROUP || mapinfo::groups()[static_cast<std::size_t>(squad_goal)].clusters_only;
    if (!squad_on_clusters) { comms::set_split_target(NO_GROUP); return squad_goal; }
    int mine = comms::split_target();
    if (mine == NO_GROUP || target_finished(s, mine) || mine == squad_goal) {
        SquadStats solo;
        solo.members = {s.my_type};
        solo.dps = combat::damage_per_round(s.my_type, s.strength, s.focus);
        solo.spell_dps = combat::damage_per_round(s.my_type, 0.0f, s.focus);
        solo.tank = s.my_type;
        solo.tank_health = s.health;
        solo.tank_max_health = s.max_health;
        solo.has_melee = s.melee;
        std::vector<int> claimed = claimed_by_others(s.squad);
        claimed.push_back(squad_goal);
        mine = best_group(s, solo, claimed, false, true);
        comms::set_split_target(mine);
    }
    return mine == NO_GROUP ? squad_goal : mine;
}

}  // namespace

int current_group() { return g_current_group; }

void avoid_group(int group, int until_round) {
    if (group < 0 || group >= static_cast<int>(avoid_until().size())) return;
    avoid_until()[static_cast<std::size_t>(group)] = until_round;
}

void avoid_current(int until_round) {
    // Only a repeated walk-away (3 within a few rounds each) marks the group: one walk-away is normal
    // fight footwork, and avoiding at once cost 10 of 80 games against v8 (bench Oct 24).
    static int last_group = NO_GROUP, last_round = -100, streak = 0;
    const int round = g_round;
    if (g_current_group < 0 || g_current_group >= static_cast<int>(avoid_until().size())) return;
    streak = (g_current_group == last_group && round - last_round <= 4) ? streak + 1 : 1;
    last_group = g_current_group;
    last_round = round;
    if (streak >= AVOID_STREAK) avoid_until()[static_cast<std::size_t>(g_current_group)] = until_round;
}

namespace {

// ---- Standing around monsters -----------------------------------------------------------------

std::vector<const CampNow*> live_camps_of(const Snapshot& s, int group) { return live_camps_of_group_visible(s, group); }

const CampNow* nearest_camp(const Snapshot& s, const std::vector<const CampNow*>& live) {
    const CampNow* best = live.front();
    for (const CampNow* c : live)
        if (c->pos.chebyshev_distance(s.me) < best->pos.chebyshev_distance(s.me)) best = c;
    return best;
}

// Tiles at exactly `distance` from the nearest monster, minus terrain.
bc::bitboard stand_ring(const CampNow& target, int distance) {
    return move::ring(target.pos, distance) & ~mapinfo::terrain_blocked();
}

// Hit from where we stand if we can; otherwise walk (melee may walk and still attack).
void hit_or_walk(Snapshot& s, const bc::bitboard& goal, const actions::Aim& aim) {
    if (s.melee) {
        const bool moved = !goal.is_set(s.me) && move::step_toward(s, goal);
        actions::attack_best(s, aim);
        actions::cast_best(s, moved, aim);
        return;
    }
    if (goal.is_set(s.me)) {
        actions::cast_best(s, false, aim);
        actions::attack_best(s, aim);
        return;
    }
    actions::cast_best(s, true, aim);
    if (!move::step_toward(s, goal)) actions::attack_best(s, aim);
}

bool fight_clusters(Snapshot& s, const GroupInfo& g, const std::vector<const CampNow*>& live) {
    s.detail = "clusters";
    actions::Aim aim;
    aim.focus_group = g.index;
    const CampNow& target = *nearest_camp(s, live);
    hit_or_walk(s, stand_ring(target, s.melee ? 1 : std::min(s.attack_range, RANGED_CAMP_DISTANCE)), aim);
    return true;
}

// Ranged with nobody tanking: stay outside the monsters' reach and use range-4 spells (or wait).
bool ranged_without_tank(Snapshot& s, const GroupInfo& g, const CampNow& target) {
    s.detail = "ranged-alone";
    actions::Aim aim;
    aim.focus_group = g.index;
    const int safe = g.max_attack_range + 1;
    if (target.pos.chebyshev_distance(s.me) < safe) {
        std::vector<vec2> threats;
        for (const CampNow& c : s.camps) if (c.group == g.index) threats.push_back(c.pos);
        actions::cast_best(s, true, aim);
        move::step_away(s, threats);
        return true;
    }
    if (safe <= 4) {   // our spells reach 4: cast from the safe ring
        hit_or_walk(s, stand_ring(target, std::max(safe, 4)), aim);
        return true;
    }
    move::step_toward(s, stand_ring(target, safe));   // hold just outside their reach
    return true;
}

// Damage on `tile` from the live monsters of this fight (each hits within its own range).
float live_exposure(vec2 tile, const std::vector<const CampNow*>& live) {
    float total = 0;
    for (const CampNow* c : live)
        if (c->pos.chebyshev_distance(tile) <= c->attack_range) total += c->strength;
    return total;
}

// Tank tiles: next to a live monster, where the fewest monsters reach (fight packs from their edge).
// Example (stampede, 6 elks): a corner tile reached by 2 elks beats a side tile reached by 4.
bc::bitboard tank_tiles(const Snapshot& s, const std::vector<const CampNow*>& live) {
    const bc::bitboard blocked = move::blocked_now(s, false);
    std::vector<std::pair<float, vec2>> candidates;
    // FUEL (v10): stand next to one of the 4 monsters nearest to us. Scanning the neighbours of all 15
    // monsters of stampede's pack cost ~45k fuel per turn (two engine calls per tile) and ran turns dry.
    std::vector<const CampNow*> near = live;
    std::sort(near.begin(), near.end(), [&](const CampNow* a, const CampNow* b) {
        return a->pos.chebyshev_distance(s.me) < b->pos.chebyshev_distance(s.me); });
    if (near.size() > 4) near.resize(4);
    for (const CampNow* c : near)
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy) {
                const vec2 q{c->pos.x + dx, c->pos.y + dy};
                if ((!dx && !dy) || !bc::map::is_in_bounds(q) || (q != s.me && blocked.is_set(q))) continue;
                candidates.push_back({live_exposure(q, live), q});
            }
    bc::bitboard out;
    out.clear();
    float best = 1e9f;
    for (const auto& c : candidates) best = std::min(best, c.first);
    for (const auto& c : candidates)
        if (c.first <= best) out.set(c.second, true);
    return out;
}

// A tile is covered when every live monster that could reach it has one of our champions strictly
// nearer to it (monsters hit their NEAREST champion). Example: an elk at (10, 10) with Marcella at (11, 10):
// Stellar at (13, 10) is 3 away > Marcella's 1 -> covered; at (10, 9) she would be 1 away -> not covered.
bool covered(const Snapshot& s, vec2 tile, const std::vector<const CampNow*>& live) {
    for (const CampNow* m : live) {
        const long mine = m->pos.chebyshev_distance(tile);
        if (m->strength <= 0 || mine > m->attack_range) continue;
        bool shielded = false;
        for (const AllyNow& a : s.allies) shielded = shielded || a.pos.chebyshev_distance(m->pos) < mine;
        if (!shielded) return false;
    }
    return true;
}

// Ranged stand tiles: at attack range from the target monster, and covered.
bc::bitboard covered_ranged_tiles(const Snapshot& s, const CampNow& target, const std::vector<const CampNow*>& live) {
    const int distance = std::min(s.attack_range, RANGED_CAMP_DISTANCE);
    const bc::bitboard blocked = move::blocked_now(s, false);
    bc::bitboard out;
    out.clear();
    for (int dx = -distance; dx <= distance; ++dx)
        for (int dy = -distance; dy <= distance; ++dy) {
            const vec2 q{target.pos.x + dx, target.pos.y + dy};
            if (q.chebyshev_distance(target.pos) != distance || !bc::map::is_in_bounds(q)) continue;
            if ((q == s.me || !blocked.is_set(q)) && covered(s, q, live)) out.set(q, true);
        }
    return out;
}

// Every melee champion stands on a tank tile (monsters split their hits between melee champions);
// ranged champions shoot from covered tiles, or fall back to range-4 spells / holding outside reach.
bool fight_monsters(Snapshot& s, const GroupInfo& g, const std::vector<const CampNow*>& live) {
    const CampNow& target = *nearest_camp(s, live);
    actions::Aim aim;
    aim.focus_group = g.index;
    if (s.my_type == bc::champion_type::heart_of_ruin) actions::use_shield_if_hit(s);
    if (s.melee) { s.detail = "tank"; hit_or_walk(s, tank_tiles(s, live), aim); return true; }
    const bc::bitboard safe = covered_ranged_tiles(s, target, live);
    if (safe.is_empty()) return ranged_without_tank(s, g, target);
    s.detail = "ranged-covered";
    hit_or_walk(s, safe, aim);
    return true;
}

// ---- Capture ------------------------------------------------------------------------------------

bool capture_step(Snapshot& s, GroupInfo& g) {
    s.detail = "capture";
    // v13: Vesper (+33 % gold) is the capturer of gold-paying groups when she is alive and near. v12's
    // "first on the region keeps it" rule meant the melee standing next to the last monster always took
    // it and ranged Vesper (3 tiles back) never captured: ladder games with Vesper at 0 gold, 0 gems.
    // A champion with no gold earned yet always joins a gold capture (rewards split) - nobody stays at 0.
    const bool me_vesper = s.my_type == bc::champion_type::vesper_vale;
    const bool broke = ZERO_GOLD_JOIN && g_earned == 0 && s.round >= ZERO_GOLD_JOIN_ROUND && g.gold > 0;
    // Yield only to a Vesper who is right there (<= 3 tiles, not channeling) and only for 3 rounds per
    // group: if she does not take it by then, the normal rule applies (a reward is never left lying).
    static std::vector<int> yield_since;
    if (yield_since.size() < mapinfo::groups().size()) yield_since.assign(mapinfo::groups().size(), -1000);
    int& ys = yield_since[static_cast<std::size_t>(g.index)];
    bool vesper_near = false;
    for (const AllyNow& a : s.allies)
        if (a.type == bc::champion_type::vesper_vale && a.pos.chebyshev_distance(g.centre) <= VESPER_CAPTURE_RANGE &&
            !a.handle.is_recalling() && !a.handle.is_submitting_gems())
            vesper_near = true;
    if (vesper_near && s.round - ys > 30) ys = s.round;   // a new yield episode
    const bool still_yielding = s.round - ys <= VESPER_YIELD_ROUNDS;
    const bool vesper_takes = g.gold > VESPER_GOLD_RATIO * g.gems && (me_vesper || (vesper_near && still_yielding));
    if (vesper_takes && !me_vesper && !broke) {
        if (g.spawn.is_set(s.me)) {
            const bc::bitboard off = move::square(s.me, 1) & ~g.spawn & ~mapinfo::terrain_blocked();
            if (move::step_toward(s, off)) return true;
        }
        actions::Aim aim;
        aim.focus_group = g.index;
        actions::cast_best(s, false, aim);
        actions::attack_best(s, aim);
        return false;
    }
    // One champion captures (more do not speed it up); the lowest body slot standing there keeps it.
    for (const AllyNow& a : s.allies) {
        if (vesper_takes && me_vesper) break;   // Vesper does not yield on gold groups
        if (broke) break;                       // a champion at 0 gold shares the capture
        const bool they_hold = g.spawn.is_set(a.pos);
        if (they_hold && (!g.spawn.is_set(s.me) || body_slot_of(a.type) < s.my_slot)) {
            if (LOG_TURNS) std::cout << "r" << s.round << " group " << g.index << " left to " << bc::to_string(a.type) << "\n";
            g.state = GroupState::Respawning;   // for us: taken, back after capture + respawn
            g.respawn_ready_round = s.round + capture_rounds(s) + g.respawn_full;
            return false;
        }
    }
    actions::Aim aim;
    aim.focus_group = g.index;
    if (g.spawn.is_set(s.me)) {
        actions::cast_best(s, false, aim);
        actions::attack_best(s, aim);
        return true;
    }
    bc::bitboard free_tiles = g.spawn & ~bc::vision::champion_tiles();
    return move::step_toward(s, free_tiles.is_empty() ? move::square(g.centre, 1) : free_tiles);
}

// Walk next to an unseen group (never onto its spawn tiles); return false when only waiting.
bool approach_group(Snapshot& s, const GroupInfo& g) {
    s.detail = "walk";
    actions::Aim aim;
    aim.focus_group = g.index;
    // v10: walk toward the monster tiles we cannot see yet, not the group's first tile. A long group (a row
    // of 30 gold clusters on yellow-brick-road) can be half cleared: the rest is out of sight, and walking
    // to tile 0 - where we already stood - froze Ryth on that spot.
    bc::bitboard unseen;
    unseen.clear();
    for (vec2 t : g.tiles)
        if (!world::tile_visible(t)) unseen |= move::square(t, 1);
    bc::bitboard near = unseen & ~(mapinfo::terrain_blocked() | g.spawn);
    if (near.is_empty()) near = move::square(g.centre, 2) & ~(mapinfo::terrain_blocked() | g.spawn);
    if (g.state == GroupState::Respawning && g.centre.chebyshev_distance(s.me) <= 3 && !g.spawn.is_set(s.me)) {
        s.detail = "respawn-wait";
        return false;
    }
    if (!s.camps.empty() || !s.enemies.empty()) actions::cast_best(s, true, aim);
    move::travel_toward(s, 1000 + g.index, near);
    return true;
}

bool work_on_group(Snapshot& s, int index) {
    GroupInfo& g = mapinfo::groups()[static_cast<std::size_t>(index)];
    const std::vector<const CampNow*> live = live_camps_of(s, index);
    if (live.empty() && g.state == GroupState::RewardAvailable) return capture_step(s, g);
    if (live.empty()) return approach_group(s, g);
    if (g.clusters_only) return fight_clusters(s, g, live);
    return fight_monsters(s, g, live);
}

}  // namespace

bool finish_capture(Snapshot& s) {
    const int here = mapinfo::group_at(s.me);
    if (here == NO_GROUP) return false;
    GroupInfo& g = mapinfo::groups()[static_cast<std::size_t>(here)];
    if (g.state != GroupState::RewardAvailable) return false;
    g_current_group = here;
    return capture_step(s, g);
}

namespace {

// v9 side job: a champion that would only wait (a teammate holds the capture, or the target is still
// respawning) takes the best group it can kill alone within SIDE_JOB_RANGE tiles, then rejoins.
// Example (collision, 3 picks): Vesper waited 230 turns per game beside Heart of Ruin's captures; a gem
// cluster 6 tiles away (120 health, 7 gems) dies to two of her basic attacks in that time.
int side_job(const Snapshot& s, int squad_goal) {
    const SquadStats solo = stats_of_members(s, {s.my_type});
    std::vector<int> claimed = claimed_by_others(s.squad);
    claimed.push_back(squad_goal);
    int best = NO_GROUP;
    double best_value = 0;
    for (const auto& [bound, index] : by_bound(s)) {
        if (bound <= best_value) break;
        if (bc::fuel_remaining() < FUEL_SCAN_FLOOR) break;   // step 0: every scan stops here, keeping the best so far
        const GroupInfo& g = mapinfo::groups()[static_cast<std::size_t>(index)];
        if (contains(claimed, g.index)) continue;
        if (walk_rounds(g, s) > SIDE_JOB_RANGE) continue;   // step 0c: by walk, not straight line
        if (g.state == GroupState::Respawning) continue;
        if (!group_feasible(g, solo) || !zone_rule_allows(g, s)) continue;
        const double v = group_value(g, solo, s);
        if (v > best_value) best_value = v, best = g.index;
    }
    return best;
}

}  // namespace

bool farm_step(Snapshot& s) {
    int target = squad_target(s);
    if (s.role == Role::Farmer) target = split_target(s, target);
    g_current_group = target;
    if (target == NO_GROUP) return false;
    if (!avoided(target) && work_on_group(s, target)) return true;
    // Step 0: the side-job scan costs ~30k; without fuel left to act on it the turn ran out (00lanes).
    if (!SIDE_JOBS || s.health < 0.6 * s.max_health || bc::fuel_remaining() < FUEL_FOR_SIDE_JOB) return false;
    const int side = side_job(s, target);
    if (side == NO_GROUP) return false;
    g_current_group = side;
    return work_on_group(s, side);
}

}  // namespace farm
