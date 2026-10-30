// combat.cpp - damage model, kill race, fight decision and fight turns. [S6]
//
// Worked example (Interrupt): an enemy Prism stands next to an altar, tributing 34 gems (4 turns).
//   Damage does NOT stop a tribute; a stun or a new altar lock does. Cassi is 4 tiles away with her
//   stun ready -> Interrupt: stun (slot 1, range 4) cancels the channel; she does not need to win a fight.
//   A recall, on the other hand, is cancelled by any damage, so any hit interrupts it.
#include "combat.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <variant>
#include "actions.h"
#include "comms.h"
#include "farm.h"
#include "config.h"
#include "mapinfo.h"
#include "move.h"
#include "world.h"

namespace combat {
namespace {

// What a champion's two slots add up to, per point of Focus (read once from the engine's metadata).
struct KitFacts {
    bool loaded = false;
    double spell_sum = 0;     // sum of damage modifiers over both slots (x Focus = damage per cycle)
    double spell_best = 0;    // biggest single damage modifier (x Focus = burst spell)
    bool stuns = false;       // a slot that stuns champions (stun, damage+stun, Heart of Ruin's dash)
};

double modifier_of(const bc::ability& a) {
    double m = 0;
    if (!a.info()) return 0;
    std::visit([&](const auto& p) { if constexpr (requires { p.modifier; }) m = p.modifier; }, *a.info());
    return m;
}

const KitFacts& kit_of(bc::champion_type type) {
    static std::array<KitFacts, 13> cache{};
    KitFacts& k = cache[static_cast<std::size_t>(type)];
    if (k.loaded) return k;
    k.loaded = true;
    const bc::champion design(type);
    for (const bc::ability& a : design.abilities()) {
        const bool damaging = a.is_damage() || a.is_aoe_damage() || a.is_cc() || a.is_dash();
        const double m = damaging ? modifier_of(a) : 0.0;
        k.spell_sum += m;
        k.spell_best = std::max(k.spell_best, m);
        k.stuns = k.stuns || a.is_cc() || (a.is_dash() && m > 0);
    }
    return k;
}

// Heart of Ruin hits up to 50% harder as his health falls (0% at full, 25% at half).
double ruin_bonus(bc::champion_type type, double health, double max_health) {
    if (type != bc::champion_type::heart_of_ruin || max_health <= 0) return 1.0;
    return 1.0 + 0.5 * std::clamp(1.0 - health / max_health, 0.0, 1.0);
}

struct Side { double dps = 0, burst = 0; int stuns = 0, count = 0; };

void add_fighter(Side& side, bc::champion_type type, float strength, float focus, double health, double max_health) {
    const double bonus = ruin_bonus(type, health, max_health);
    side.dps += damage_per_round(type, strength, focus) * bonus;
    side.burst += burst(type, strength, focus) * bonus;
    side.stuns += kit_of(type).stuns ? 1 : 0;
    side.count += 1;
}

// Me + visible allies within ENGAGE_RANGE of the target.
Side our_side(const Snapshot& s, vec2 target) {
    Side side;
    const double bonus = ruin_bonus(s.my_type, s.health, s.max_health);
    side.dps += damage_per_round(s.my_type, s.strength, s.focus) * bonus;
    side.burst += my_burst_now(s) * bonus;
    side.stuns += kit_of(s.my_type).stuns ? 1 : 0;
    side.count = 1;
    for (const AllyNow& a : s.allies)
        if (a.pos.chebyshev_distance(target) <= ENGAGE_RANGE) {
            a.read_stats();
            add_fighter(side, a.type, a.strength, a.focus, a.health, a.max_health);
        }
    return side;
}

// Visible enemies within ENGAGE_RANGE of us (those standing on their own spawn do not come out).
Side their_side(const Snapshot& s) {
    Side side;
    for (const EnemyNow& e : s.enemies)
        if (!e.on_their_spawn && e.pos.chebyshev_distance(s.me) <= ENGAGE_RANGE)
            add_fighter(side, e.type, e.strength, e.focus, e.health, e.max_health);
    return side;
}

double my_effective_health(const Snapshot& s) {
    static int cached_round = -1;   // v10: once per turn (asked once per enemy by the race)
    static double cached = 0;
    if (cached_round == s.round) return cached;
    const bc::buff_collection buffs = bc::self().buffs();
    cached = s.health + (buffs.has_shield() ? buffs.get<bc::buff_shield>().value : 0.0f);
    cached_round = s.round;
    return cached;
}

// Rounds until a teammate of `target` can join: visible ones by distance; unseen-but-alive ones from
// memory (last place minus rounds since). v12: never-seen enemies are placed by the walk from THEIR
// SPAWN to the target (mapinfo::steps_from_enemy), not a flat ETA_UNSEEN - the flat 7 made every
// 5-v-5 fight look "helped" and we walked away from winnable 2-v-1s all game (match 8d0f4189:
// HoR + Marcella kept distance from a lone Tavi for dozens of rounds). At most UNSEEN_HELPER_CAP
// unseen helpers count.
double eta_help(const Snapshot& s, const EnemyNow& target) {
    double eta = 1e9;
    int unseen = 0;
    for (const EnemyNow& e : s.enemies) {
        if (e.handle == target.handle) continue;
        eta = std::min(eta, static_cast<double>(std::max<long>(0, e.pos.chebyshev_distance(target.pos) - e.attack_range)));
    }
    for (const EnemyMemory& m : world::enemy_memory()) {
        if (!m.exists || !m.alive || m.last_seen_round == s.round || m.type == target.type) continue;
        if (unseen >= UNSEEN_HELPER_CAP) break;
        ++unseen;
        if (m.last_seen_round < -100) {   // never seen: assume coming from their spawn
            eta = std::min(eta, static_cast<double>(std::max(1, mapinfo::steps_from_enemy(target.pos))));
            continue;
        }
        const double since = s.round - m.last_seen_round;
        const double walk = static_cast<double>(m.last_pos.chebyshev_distance(target.pos)) - since;
        eta = std::min(eta, since > 30 ? static_cast<double>(mapinfo::steps_from_enemy(target.pos)) : std::max(1.0, walk));
    }
    return eta;
}

// How far away a target can be for us to damage it THIS turn. Everyone walks 1 tile per turn, so an
// enemy outside this reach simply walks away from us: chasing it wastes turns.
// melee: walk 1 + attack 1 = 2 (a dash landing next to it: 3); ranged: attack 3, or a ready range-4 spell.
int my_reach(const Snapshot& s) {
    int reach = s.melee ? s.attack_range + 1 : s.attack_range;
    for (const bc::ability& a : bc::self().abilities()) {
        if (a.cooldown() > 0) continue;
        if (a.is_dash() && s.melee) reach = std::max(reach, s.attack_range + 2);
        if (a.is_damage() || a.is_cc() || a.is_aoe_damage()) reach = std::max(reach, 4);
    }
    return reach;
}

bool catchable(const Snapshot& s, const EnemyNow& t, double our_burst) {
    const long distance = t.pos.chebyshev_distance(s.me);
    if (t.channeling && distance <= 6) return true;   // a channeler does not walk away
    if (distance <= my_reach(s)) return true;
    return t.stunned && distance <= my_reach(s) + 1 && t.health + t.shield <= our_burst * 2;
}

// Omar's zone rule applied to chasing: no hunting deep inside the enemy zone.
bool chase_allowed(const Snapshot& s, const EnemyNow& t) {
    if (!mapinfo::in_enemy_zone(t.pos)) return true;
    return t.pos.chebyshev_distance(s.me) <= ZONE_ENTRY_RANGE && s.health >= ZONE_ENTRY_HEALTH * s.max_health &&
           s.bag <= ZONE_ENTRY_BAG;
}

struct Race { double ours = 0, theirs = 0, chance = 0, eta = 0; bool catchable = false; int us = 1, them = 1; };

Race race_against(const Snapshot& s, const EnemyNow& t) {
    const Side us = our_side(s, t.pos);
    Side them = their_side(s);
    if (t.type == bc::champion_type::ren) them.dps += 0.2 * t.focus * us.count;   // Ren retaliates per hit
    // v12: an enemy Willow near the fight undoes ~420 / 8 = 52.5 of our damage every round; without
    // this the race loves fights we then lose by attrition (match 8d0f4189: 8k damage dealt, 31k taken).
    double heal_eaten = 0;
    for (const EnemyNow& e : s.enemies)
        if (e.type == bc::champion_type::willow && !e.on_their_spawn && e.pos.chebyshev_distance(t.pos) <= ENGAGE_RANGE)
            heal_eaten += HEAL_DPS;
    const double our_dps = std::max(us.dps - heal_eaten, 0.4 * us.dps);
    const int stuns_on_us = s.my_type == bc::champion_type::marcella ? 0 : std::min(them.stuns, 2);
    Race r;
    r.ours = rounds_to_kill(t.health + t.shield, us.burst, our_dps, stuns_on_us);
    r.theirs = rounds_to_kill(my_effective_health(s), them.burst, them.dps, std::min(us.stuns, 2));
    r.chance = p_win(r.theirs, r.ours, true);
    r.eta = eta_help(s, t);
    r.catchable = catchable(s, t, us.burst);
    r.us = us.count;
    r.them = them.count;
    return r;
}

// Value of a kill per round spent: stolen bag x2, kill gold, and 60 rounds of their absence.
double kill_value(const EnemyNow& t, double rounds) {
    return (2.0 * t.bag + 100.0 * LAMBDA_GEMS_PER_GOLD + 60.0 * 0.4) / std::max(1.0, rounds);
}

bool in_reach_of(const Snapshot& s, const EnemyNow& e) {
    return e.pos.chebyshev_distance(s.me) <= std::max(e.attack_range, 4) + 1;
}

bool have_lock() { return bc::self().consumables()[bc::consumable_type::altar_lock].count() > 0; }

}  // namespace

double damage_per_round(bc::champion_type type, float strength, float focus) {
    return strength + focus * kit_of(type).spell_sum / CAST_PERIOD;
}

double burst(bc::champion_type type, float strength, float focus) {
    return strength + focus * kit_of(type).spell_best;
}

double my_burst_now(const Snapshot& s) {
    static int cached_round = -1;
    static double cached = 0;
    if (cached_round == s.round) return cached;
    double best_spell = 0;
    for (const bc::ability& a : bc::self().abilities())
        if (a.cooldown() == 0 && (a.is_damage() || a.is_aoe_damage() || a.is_cc() || a.is_dash()))
            best_spell = std::max(best_spell, static_cast<double>(a.damage_amount()));
    cached_round = s.round;
    cached = s.strength + best_spell;
    return cached;
}

const bc::champion_stats& base_stats_of(bc::champion_type type) {
    static std::array<bool, 13> loaded{};
    static std::array<bc::champion_stats, 13> stats{};
    const std::size_t i = static_cast<std::size_t>(type);
    if (!loaded[i]) { stats[i] = bc::champion(type).base_stats(); loaded[i] = true; }
    return stats[i];
}

double rounds_to_kill(double ehp, double burst_damage, double dps, int defender_stuns) {
    return 1.0 + std::max(0.0, ehp - burst_damage) / std::max(1.0, dps) + defender_stuns;
}

double p_win(double their_rounds, double our_rounds, bool we_act_now) {
    const double edge = their_rounds - our_rounds + (we_act_now ? 0.5 : -0.5);
    return 1.0 / (1.0 + std::exp(-edge / RACE_SOFTNESS));
}

// An enemy that can hit us next turn is already in the fight: we answer it if the race is even or
// better (walking away from a ranged enemy only gets us shot in the back).
bool engaged_with(const Snapshot& s, const EnemyNow& e) {
    return e.pos.chebyshev_distance(s.me) <= e.attack_range + 1;
}

namespace {

// v10: the enemy to hit when we are caught: a melee (or dashing) enemy within 2 tiles follows and hits us
// every turn, and home is farther than we can walk before dying. Returns the index of the weakest enemy
// we can damage this turn, or -1 when we can still get away.
// Example: Vesper at 600 health, an enemy Marcella adjacent (66 damage per round) and a Vesper at range 3
// (62): 128 per round -> 4.7 rounds to live; home is 12 steps -> cornered -> she shoots the weaker one.
int cornered_target_of(const Snapshot& s) {
    bool chased = false;
    double incoming = 0;
    for (const EnemyNow& e : s.enemies) {
        if (e.on_their_spawn || e.pos.chebyshev_distance(s.me) > std::max(e.attack_range, 4) + 1) continue;
        incoming += damage_per_round(e.type, e.strength, e.focus);
        const bool melee_or_dash = e.attack_range <= 1 || e.type == bc::champion_type::vesper_vale || e.type == bc::champion_type::vendetta ||
                                   e.type == bc::champion_type::heart_of_ruin || e.type == bc::champion_type::ren;
        chased = chased || (melee_or_dash && e.pos.chebyshev_distance(s.me) <= 2);
    }
    if (!chased || incoming <= 0) return -1;
    const double rounds_alive = my_effective_health(s) / incoming;
    if (mapinfo::steps_from_us(s.me) < rounds_alive) return -1;   // home in time: run
    int best = -1;
    double best_ehp = 1e18;
    const int reach = my_reach(s);
    for (std::size_t i = 0; i < s.enemies.size(); ++i) {
        const EnemyNow& e = s.enemies[i];
        if (e.on_their_spawn || e.pos.chebyshev_distance(s.me) > reach) continue;
        const double ehp = e.health + e.shield;
        if (ehp < best_ehp) best_ehp = ehp, best = static_cast<int>(i);
    }
    return best;
}

}  // namespace

FightPlan decide_fight(const Snapshot& s) {
    FightPlan plan;
    double best_value = 0;
    int channeler = -1;
    for (std::size_t i = 0; i < s.enemies.size(); ++i) {
        const EnemyNow& e = s.enemies[i];
        if (e.on_their_spawn || e.pos.chebyshev_distance(s.me) > HUNT_RANGE || !chase_allowed(s, e)) continue;
        const Race r = race_against(s, e);
        if (e.channeling && in_reach_of(s, e) && r.theirs > 1.5) channeler = static_cast<int>(i);
        // v10: a long tribute is worth a walk. It lasts ceil(bag / 10) turns and an altar lock placed
        // within 2 tiles cancels it (a stun-immune Marcella included). Online, boss-battle: an enemy
        // Marcella tributed 763 gems over 77 rounds while four of ours carried unused locks.
        if (channeler < 0 && e.handle.is_submitting_gems() && have_lock() && r.theirs > 1.5 &&
            e.pos.chebyshev_distance(s.me) - 3 <= std::max(1, (e.bag + 9) / 10))
            channeler = static_cast<int>(i);
        const bool engaged = engaged_with(s, e) && r.catchable;
        bool fight = engaged ? r.chance >= 0.5 : (r.catchable && r.chance >= PWIN_FIGHT && r.ours <= r.eta);
        // v10 commit rule: an enemy already in range of us, at least as many of us as of them, and a race
        // that is close -> fight. Walking away from it hands out free hits: hooks, v10 vs v9, Oct 25: our
        // four kept stepping away from a 4-v-4 that looked 45/55 and died 34 times with 0 kills.
        if (!fight && engaged && r.catchable && r.us >= r.them && r.chance >= COMMIT_PWIN) fight = true;
        // v12 outnumber commit: two or more of us on one catchable enemy and their help honestly far
        // (eta_help now measures from their spawn / last sighting, not a flat 7) -> take the fight at a
        // discount. Match 8d0f4189 round ~120: HoR + Marcella danced at range around a lone Tavi next to
        // an altar for dozens of rounds instead of killing him.
        if (!fight && r.catchable && r.us >= r.them + 1 && r.chance >= OUTNUMBER_PWIN && r.ours <= r.eta + 2) fight = true;
        // v12 altar guard: an enemy standing on our banking ground (near an altar we tribute at) is worth
        // a below-gate fight - letting them camp the altars is how the middle of the map was handed over.
        if (!fight && r.catchable && r.us >= r.them && r.chance >= ALTAR_GUARD_PWIN &&
            mapinfo::steps_to_altar(e.pos) <= ALTAR_GUARD_RANGE)
            fight = true;
        const double value = kill_value(e, r.ours) + (engaged ? 1000.0 : 0.0);   // answer attackers first
        if (fight && value > best_value) {
            best_value = value;
            plan.choice = FightChoice::Fight;
            plan.target = static_cast<int>(i);
            plan.chance = r.chance;
            plan.our_rounds = r.ours;
        }
        if (!fight && 1.0 - r.chance > 0.5 && in_reach_of(s, e)) plan.threats.push_back(e.pos);
    }
    if (plan.choice == FightChoice::Fight) return plan;
    if (channeler >= 0) { plan.choice = FightChoice::Interrupt; plan.target = channeler; return plan; }
    if (!plan.threats.empty()) {
        plan.choice = FightChoice::WalkAway;
        // v10: walking away only helps when it gets us out. A melee enemy next to us walks and hits every
        // turn, so fleeing from it deals no damage and still dies (online v9 losses: 0 kills and ~250
        // champion damage per champion over 10 deaths). If home is too far to reach alive, hit back.
        const int cornered_target = CORNERED_FIGHT ? cornered_target_of(s) : -1;
        if (cornered_target >= 0) { plan.choice = FightChoice::Fight; plan.target = cornered_target; }
    }
    return plan;
}

namespace {

// Step b: starting a fight is a help emergency (type 3) at the target (the old focus type is dropped).
void announce_fight(const Snapshot& s, const EnemyNow& t) {
    (void)s;
    comms::raise_emergency(comms::EmergencyType::Help, t.pos);
}

// Cast -> attack -> move, in that order (a ranged champion that walks has no attack left).
void fight_turn(Snapshot& s, int target) {
    const EnemyNow t = s.enemies[static_cast<std::size_t>(target)];
    actions::Aim aim;
    aim.focus_enemy = target;
    aim.hit_camps = false;
    const long distance = t.pos.chebyshev_distance(s.me);
    if (s.my_type == bc::champion_type::heart_of_ruin && distance >= 2 && distance <= 3) actions::dash_onto(s, t.pos);
    if (t.pos.chebyshev_distance(s.me) <= s.attack_range) {
        actions::cast_best(s, false, aim);
        actions::attack_best(s, aim);
        return;
    }
    const bool cast = actions::cast_best(s, s.melee, aim);   // melee still walks: no exclusive spells
    if (!s.melee && cast) return;                     // ranged: the spell was this turn's action
    move::step_toward(s, move::square(t.pos, s.melee ? 1 : s.attack_range));
    actions::attack_best(s, aim);
}

bool use_lock_on(Snapshot& s, vec2 enemy_pos) {
    auto& me = bc::self();
    const bc::consumable& lock = me.consumables()[bc::consumable_type::altar_lock];
    if (lock.count() == 0) return false;
    for (vec2 altar : mapinfo::altars()) {
        if (altar.chebyshev_distance(enemy_pos) > 1 || altar.chebyshev_distance(s.me) > 2) continue;
        if (bc::vision::has_altar_lock(altar)) continue;
        return lock.use(altar);
    }
    return false;
}

void interrupt_turn(Snapshot& s, int target) {
    const EnemyNow t = s.enemies[static_cast<std::size_t>(target)];
    if (bc::self().is_submitting_gems() || bc::self().is_recalling()) return;
    if (t.handle.is_submitting_gems() && use_lock_on(s, t.pos)) return;   // a new lock cancels the tribute
    // v10: out of reach but a lock in the bag: walk to 2 tiles from the altar it tributes at.
    if (t.handle.is_submitting_gems() && have_lock() && t.pos.chebyshev_distance(s.me) > std::max(2, s.attack_range)) {
        for (vec2 altar : mapinfo::altars()) {
            if (altar.chebyshev_distance(t.pos) > 1) continue;
            actions::Aim aim;
            aim.hit_camps = false;
            actions::cast_best(s, true, aim);
            if (move::step_toward(s, move::square(altar, 2))) return;
        }
    }
    fight_turn(s, target);
}

void walk_away_turn(Snapshot& s, const FightPlan& plan) {
    actions::Aim aim;
    aim.hit_camps = false;
    actions::use_shield_if_hit(s);
    actions::cast_best(s, true, aim);
    // v10: close to home, walk home (the spawn heals every turn end and enemies cannot enter it, see
    // spawn_hold); otherwise just away from the threats.
    if (mapinfo::steps_from_us(s.me) <= WALK_HOME_STEPS &&
        move::travel_toward(s, 3000, bc::map::spawn_region(bc::team::ally))) return;
    if (!move::step_away(s, plan.threats)) actions::attack_best(s, aim);
}

// No enemy in sight: answer a fresh call for help nearby (farmers only if very close).
bool answer_help(Snapshot& s) {
    const comms::Emergency seen = comms::read_emergency();
    if (!seen.fresh || seen.type != comms::EmergencyType::Help) return false;
    const long distance = seen.pos.chebyshev_distance(s.me);
    const bool farmer = s.role == Role::TankFarmer || s.role == Role::Farmer;
    if (distance > (farmer ? FARMER_HELP_RANGE : HELP_RANGE) || distance <= 2) return false;
    if (s.bag >= HELP_MAX_BAG || s.health < 0.5 * s.max_health || mapinfo::in_enemy_zone(seen.pos)) return false;
    return move::step_toward(s, move::square(seen.pos, 2));
}

}  // namespace

namespace {

// v10 spawn hold. Our spawn heals us to full at every turn end and enemies cannot step on it, so a fight
// we would lose outside is safe on it: stay and hit whatever comes in range. Also, a champion on the spawn
// does not walk out alone into a fresh sighting of 2+ enemies near home (hooks, online: 39 deaths in
// 1,000 rounds, each champion walking out alone into the same 4 enemies), for at most SPAWN_HOLD_MAX rounds.
bool spawn_hold(Snapshot& s, bool losing) {
    if (!SPAWN_HOLD || !s.on_our_spawn) return false;
    static int hold_since = -1000, last_hold = -1000;
    if (s.round - last_hold > 1) hold_since = s.round;
    bool hold = losing;
    if (!hold && s.enemies.empty()) {
        // Step b: the emergency keeps only "1 vs 2+" enemies (type 1 / types 2-3), so 2+ counts as 2.
        const comms::Emergency seen = comms::read_emergency();
        const int count = seen.type == comms::EmergencyType::DangerOne ? 1 : 2;
        int allies_home = 1;
        for (const AllyNow& a : s.allies) allies_home += a.pos.chebyshev_distance(s.me) <= 3 ? 1 : 0;
        hold = seen.fresh && count >= 2 && count > allies_home &&
               seen.pos.chebyshev_distance(mapinfo::home_centre()) <= SPAWN_HOLD_RANGE;
    }
    if (!hold || s.round - hold_since >= SPAWN_HOLD_MAX) return false;
    last_hold = s.round;
    s.detail = "hold";
    actions::Aim aim;
    aim.hit_camps = false;
    actions::cast_best(s, false, aim);
    actions::attack_best(s, aim);
    return true;
}

}  // namespace

bool handle_enemies(Snapshot& s) {
    if (s.enemies.empty() && spawn_hold(s, false)) return true;
    if (s.enemies.empty()) {
        s.detail = "help";
        return answer_help(s);
    }
    const FightPlan plan = decide_fight(s);
    if (plan.choice == FightChoice::WalkAway && spawn_hold(s, true)) return true;
    switch (plan.choice) {
        case FightChoice::Fight: s.detail = "fight"; announce_fight(s, s.enemies[static_cast<std::size_t>(plan.target)]); fight_turn(s, plan.target); return true;
        case FightChoice::Interrupt: s.detail = "interrupt"; interrupt_turn(s, plan.target); return true;
        case FightChoice::WalkAway: s.detail = "walk-away"; farm::avoid_current(s.round + AVOID_ROUNDS); walk_away_turn(s, plan); return true;
        case FightChoice::Ignore: return false;
    }
    return false;
}

double incoming_damage(const Snapshot& s) {
    double total = 0;
    for (const CampNow& c : s.camps) {
        const long d = c.pos.chebyshev_distance(s.me);
        if (!c.awake || c.strength <= 0 || d > c.attack_range) continue;
        bool someone_closer = false;
        for (const AllyNow& a : s.allies) someone_closer = someone_closer || a.pos.chebyshev_distance(c.pos) < d;
        if (!someone_closer) total += c.strength;
    }
    for (const EnemyNow& e : s.enemies)
        if (!e.on_their_spawn && e.pos.chebyshev_distance(s.me) <= std::max(e.attack_range, 4) + 1)
            total += damage_per_round(e.type, e.strength, e.focus);
    return total;
}

}  // namespace combat
