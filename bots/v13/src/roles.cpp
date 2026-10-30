// roles.cpp - the ladder (roles.h) and the role steps. [S5]
//
// Each ladder step is one small function returning true when it used the turn's main decision.
// A step may still cast or attack before returning (survival casts while stepping away).
// Worked example (step 3): Marcella recalls home at 40% health. The recall lands at the START of her
//   turn; spawn tiles heal at the END of a turn spent on them, so she stays this one turn
//   (health < SPAWN_STAY_HEALTH = 95%), shops, and leaves at full health next turn.
#include "roles.h"
#include <algorithm>
#include <array>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include "actions.h"
#include "bank.h"
#include "belief.h"
#include "combat.h"
#include "comms.h"
#include "config.h"
#include "disrupt.h"
#include "farm.h"
#include "level2.h"
#include "mapinfo.h"
#include "move.h"
#include "shop.h"
#include "world.h"

namespace {

int g_stuck_until = -1;   // v10 watchdog: bank / endgame steps are skipped until this round
int g_go_home_until = -1; // v10 watchdog: a stand-off with enemies -> go home until this round

// ---- Steps 1-3 ----------------------------------------------------------------------------------

bool channeling_or_stunned() {
    auto& me = bc::self();
    if (me.is_recalling() || me.is_submitting_gems() || me.is_teleporting()) return true;
    const bc::buff_restrictions r = me.restrictions();
    return r.contains(bc::buff_restrictions::move) && r.contains(bc::buff_restrictions::attack) &&
           r.contains(bc::buff_restrictions::cast);
}

bool needs_heal(const Snapshot& s) { return s.health < SPAWN_STAY_HEALTH * s.max_health; }

void hit_what_is_in_range(Snapshot& s) {
    actions::Aim aim;
    actions::cast_best(s, false, aim);
    actions::attack_best(s, aim);
}

// ---- Step 5: survival ---------------------------------------------------------------------------

// Retreat early enough to walk out of reach alive: a monster with range 4 keeps hitting for ~5 more
// rounds while we leave. Example: Marcella at 500 health taking 120 per round from two elks (range 4):
// 500 < 120 x max(3, 4 + 1) = 600 -> retreat now.
bool must_retreat(const Snapshot& s) {
    if (s.on_our_spawn) return false;
    if (s.health < RETREAT_HEALTH * s.max_health) return true;

    int escape_rounds = static_cast<int>(RETREAT_ROUNDS);
    for (const CampNow& c : s.camps)
        if (c.awake && c.strength > 0 && c.pos.chebyshev_distance(s.me) <= c.attack_range)
            escape_rounds = std::max(escape_rounds, c.attack_range + 1);
    return s.health < combat::incoming_damage(s) * escape_rounds;
}

std::vector<vec2> threats_near(const Snapshot& s) {
    std::vector<vec2> out;
    for (const EnemyNow& e : s.enemies)
        if (!e.on_their_spawn && e.pos.chebyshev_distance(s.me) <= ENGAGE_RANGE) out.push_back(e.pos);
    for (const CampNow& c : s.camps)
        if (c.awake && c.strength > 0 && c.pos.chebyshev_distance(s.me) <= c.attack_range + 1) out.push_back(c.pos);
    return out;
}

// Step d (RETREAT): recall now (if safe), step away, or walk home around the danger layer - scored by
// Q = -P(death) L_D - T r (nothing is gained, so the safest quick option wins).
bool retreat_level2(Snapshot& s) {
    struct Cand { double q; int kind; };   // 0 recall, 1 step away, 2 walk home avoiding danger
    std::array<Cand, 3> cands{};
    int n = 0;
    auto score = [&](double T, vec2 dest) {
        level2::Plan p;
        p.T = T;
        p.p_death = level2::p_death(s, T, dest, &p.enemy_share);
        return level2::q(s, p);
    };
    if (bank::recall_is_safe(s)) cands[static_cast<std::size_t>(n++)] = {score(RECALL_TURNS, s.me), 0};
    cands[static_cast<std::size_t>(n++)] = {score(RETREAT_STEP_ROUNDS, s.me), 1};
    const int home = mapinfo::steps_from_us(s.me);
    if (home < FAR_AWAY) cands[static_cast<std::size_t>(n++)] = {score(home, mapinfo::home_centre()), 2};
    std::sort(cands.begin(), cands.begin() + n, [](const Cand& a, const Cand& b) { return a.q > b.q; });
    actions::use_shield_if_hit(s);
    actions::Aim aim;
    for (int i = 0; i < n; ++i) {
        const int kind = cands[static_cast<std::size_t>(i)].kind;
        if (kind == 0) { bc::self().recall(); return true; }
        actions::cast_best(s, true, aim);
        if (kind == 1 && move::step_away(s, threats_near(s))) return true;
        if (kind == 2) {
            const bc::bitboard& avoid = belief::danger();
            if (move::step_toward(s, bc::map::spawn_region(bc::team::ally), &avoid)) return true;
        }
    }
    actions::attack_best(s, aim);
    return true;
}

void retreat_or_recall(Snapshot& s) {
    if (LEVEL2_RETREAT && level2::active() && retreat_level2(s)) return;
    if (bank::recall_is_safe(s)) {
        bc::self().recall();
        return;
    }
    actions::use_shield_if_hit(s);
    actions::Aim aim;
    actions::cast_best(s, true, aim);
    const std::vector<vec2> threats = threats_near(s);
    if (!move::step_away(s, threats)) actions::attack_best(s, aim);
}

// ---- Step 10: role step -------------------------------------------------------------------------

// v11 Willow: heal the squad on cooldown. Her heal is compatible with walking and one cast per turn,
// so healing costs only this turn's offensive cast - farm continues below either way.
// Worked example: Marcella tanks an elk pack at 700/1,700; Willow's 3 x 140 = 420 heal (range 4) puts
// her back to full, removing a recall trip (~17 rounds) from the squad's trip time.
void support_heal(Snapshot& s) {
    if (s.role != Role::Support) return;
    actions::heal_best(s);
}

// v11 denial (OPTIMAL_STRATEGY.md §14): our lock blocks only THEIR tributes, so placing one is a free
// action (consumables do not use the move / attack / cast allowances). Lock an enemy-half altar we
// walk past, any altar under bank pressure, or when their carrier is in our vision heading to bank.
// Worked example: they have 860 banked and their Vesper (bag 40) walks past us to the mid altar: the
// 10-gold lock forces 5 basic attacks or a detour - a full tribute cycle - before she can bank.
bool maybe_lock_altar(Snapshot& s) {
    auto& me = bc::self();
    const bc::consumable& lock = me.consumables()[bc::consumable_type::altar_lock];
    if (lock.count() == 0) return false;
    const bool pressure = s.banked_them >= LOCK_BANK_PRESSURE;
    bool carrier_visible = false;
    for (const EnemyNow& e : s.enemies)
        if (!e.on_their_spawn && e.bag >= LOCK_ENEMY_BAG) carrier_visible = true;
    if (!pressure && !carrier_visible && s.enemies.empty()) return false;   // cheap exit
    // An allied lock we cannot see (bushes block vision of the altar) makes the placement throw; each
    // throw costs 10k fuel and aborts the turn, so a failed altar is off-limits for a while.
    static std::vector<std::pair<vec2, int>> failed;   // altar -> retry not before this round
    for (std::size_t i = 0; i < failed.size();)
        if (s.round >= failed[i].second) failed.erase(failed.begin() + static_cast<long>(i));
        else ++i;
    for (vec2 altar : mapinfo::altars()) {
        if (altar.chebyshev_distance(s.me) > 2 || bc::vision::has_altar_lock(altar)) continue;
        bool known_failed = false;
        for (const auto& f : failed)
            if (f.first == altar) known_failed = true;
        if (known_failed) continue;
        const bool worth_it = pressure || carrier_visible || mapinfo::in_enemy_half(altar);
        if (!worth_it) continue;
        try {
            if (lock.use(altar)) return true;
        } catch (const bc::error&) {
            failed.push_back({altar, s.round + 40});
        }
    }
    return false;
}

// v11: an enemy is capturing a cleared region (world.cpp reads capture_state): standing on the region
// resets their progress, and they are stationary for the timer - often a free catchable fight. Walk
// over when the reward pays for the detour and we arrive inside their capture timer.
const GroupInfo* contest_target(const Snapshot& s) {
    if (s.bag > CONTEST_MAX_BAG || s.health < CONTEST_MIN_HEALTH * s.max_health) return nullptr;
    const GroupInfo* best = nullptr;
    long best_distance = 1L << 30;
    for (const GroupInfo& g : mapinfo::groups()) {
        if (g.enemy_capture_timer < 0) continue;
        const double reward = g.gems + LAMBDA_GEMS_PER_GOLD * g.gold;
        if (reward < CONTEST_MIN_REWARD) continue;
        const long d = g.centre.chebyshev_distance(s.me);
        if (d > g.enemy_capture_timer + 2 || d >= best_distance) continue;   // cannot arrive in time
        best = &g;
        best_distance = d;
    }
    return best;
}

bool contest_capture(Snapshot& s) {
    const GroupInfo* best = contest_target(s);
    if (!best) return false;
    s.detail = "contest";
    actions::Aim aim;
    aim.hit_camps = false;
    actions::cast_best(s, true, aim);
    if (move::step_toward(s, move::square(best->centre, 1))) return true;
    return move::square(best->centre, 1).is_set(s.me);   // already on it: hold, the enemy must react
}

// Interceptor / FarPair idle point: near an ENEMY-SIDE or MIDDLE altar but outside the enemy zone, in
// a bush if possible. v12: middle altars count too - with the altars in the middle of the map, parking
// the roaming pair next to them keeps vision, contests captures and covers our bankers; drifting home
// instead is how the middle got conceded (match 8d0f4189).
bc::bitboard idle_area(const Snapshot& s) {
    static bool built = false;
    static bc::bitboard area;
    if (built) return area;
    built = true;
    area.clear();
    for (vec2 a : mapinfo::altars()) {
        if (!mapinfo::in_enemy_half(a) && !mapinfo::in_middle_band(a)) continue;
        for (int dx = -6; dx <= 6; ++dx)
            for (int dy = -6; dy <= 6; ++dy) {
                const vec2 p{a.x + dx, a.y + dy};
                if (!bc::map::is_in_bounds(p) || p.chebyshev_distance(a) < 3 || mapinfo::in_enemy_zone(p)) continue;
                if (mapinfo::steps_from_us(p) < FAR_AWAY) area.set(p, true);
            }
    }
    area &= ~mapinfo::terrain_blocked();
    const bc::bitboard in_bushes = area & bc::map::bushes();
    if (!in_bushes.is_empty()) area = in_bushes;
    (void)s;
    return area;
}

// Nothing to farm right now: bank a partial bag if the altar is close, else take the idle point.
void idle_step(Snapshot& s) {
    if (s.bag >= IDLE_BANK_BAG && bank::reach_steps(s) <= IDLE_BANK_STEPS && bank::go_tribute(s)) return;
    // Waiting anyway: go home to heal (and shop) instead of farming hurt later - if home is not far.
    const bool hurt = s.health < 0.5 * s.max_health || (s.health < IDLE_HEAL_HEALTH * s.max_health && mapinfo::steps_from_us(s.me) <= IDLE_HEAL_HOME_STEPS);
    if (hurt && bank::recall_is_safe(s)) { bc::self().recall(); return; }
    if (s.role == Role::Interceptor || s.role == Role::FarPair) {
        const bc::bitboard area = idle_area(s);
        if (!area.is_empty() && !area.is_set(s.me)) move::travel_toward(s, 2000, area);
    }
}

// ---- Step c, Level 1: one mode per turn (Phase 1 design doc, "Level 1: choosing the mode") --------

enum class Mode : std::uint8_t { None, Carry, Retreat, Fight, Support, Explore, Farm };

const char* mode_name(Mode m) {
    switch (m) {
        case Mode::Carry: return "CARRY";
        case Mode::Retreat: return "RETREAT";
        case Mode::Fight: return "FIGHT";
        case Mode::Support: return "SUPPORT";
        case Mode::Explore: return "EXPLORE";
        case Mode::Farm: return "FARM";
        default: return "";
    }
}

struct ModeMemory { Mode mode = Mode::None; int since = -1000; };
ModeMemory g_mode;

void enter(Snapshot& s, Mode m) {
    if (m != g_mode.mode) g_mode = ModeMemory{m, s.round};
    s.mode = mode_name(m);
}

// Rule 5: Willow and an ally in range missing SUPPORT_HEAL_MISSING+ health; or (step b fields) the
// scorer is in sight, carries SUPPORT_SCORER_BAG+ gems, and a danger record lies within
// SUPPORT_DANGER_RANGE tiles of it.
bool support_wanted(const Snapshot& s) {
    if (s.role == Role::Support)
        for (const AllyNow& a : s.allies) {
            if (a.pos.chebyshev_distance(s.me) > SUPPORT_HEAL_RANGE) continue;
            a.read_stats();
            if (a.max_health - a.health >= SUPPORT_HEAL_MISSING) return true;
        }
    if (!USE_STEP_B_FIELDS) return false;
    return false;   // scorer escort: reads the step b scorer field (switched off until step b passes)
}

// Rule 6: I am the scout and a stale camp target exists (step b field: off until step b passes).
bool scout_wanted(const Snapshot& s) {
    (void)s;
    return USE_STEP_B_FIELDS && false;
}

// FARM (rule 7): the capture I stand on, then v13's role work (disruptors, contest, squad / split /
// side-job target). True if it acted.
bool run_farm_v13(Snapshot& s) {
    if (farm::finish_capture(s)) { s.step = "capture"; return true; }
    if (disrupt::step(s)) { s.step = "disrupt"; return true; }
    if (contest_capture(s)) { s.step = "contest"; return true; }
    if (farm::farm_step(s)) { s.step = "farm"; return true; }
    return false;
}

// Step d (FARM): the candidates available this turn - finish the capture I stand on, contest an enemy
// capture, work the squad's (or split / side-job) target - scored with Q and tried best first; one that
// cannot act falls through to the next. Disruptors keep their own step first (not a FARM candidate).
bool run_farm(Snapshot& s) {
    if (!LEVEL2_FARM_TURN || !level2::active()) return run_farm_v13(s);
    if (disrupt::step(s)) { s.step = "disrupt"; return true; }
    struct Cand { double q; int kind; };   // 0 capture here, 1 contest, 2 target
    std::array<Cand, 3> cands{};
    int n = 0;
    auto score = [&](const GroupInfo& g, double T, vec2 dest) {
        level2::Plan p;
        p.dE = g.gems + LAMBDA_GEMS_PER_GOLD * g.gold;
        p.T = std::max(1.0, T);
        p.p_death = level2::p_death(s, p.T, dest, &p.enemy_share);
        return level2::q(s, p);
    };
    const int here = mapinfo::group_at(s.me);
    if (here != NO_GROUP && mapinfo::groups()[static_cast<std::size_t>(here)].state == GroupState::RewardAvailable)
        cands[static_cast<std::size_t>(n++)] = {score(mapinfo::groups()[static_cast<std::size_t>(here)], CAPTURE_ROUNDS, s.me), 0};
    if (const GroupInfo* c = contest_target(s))
        cands[static_cast<std::size_t>(n++)] = {score(*c, static_cast<double>(c->centre.chebyshev_distance(s.me)) + CAPTURE_ROUNDS, c->centre), 1};
    const int target = farm::current_group() != NO_GROUP ? farm::current_group() : comms::squad_target(s.squad);
    if (target >= 0 && target < static_cast<int>(mapinfo::groups().size())) {
        const GroupInfo& g = mapinfo::groups()[static_cast<std::size_t>(target)];
        cands[static_cast<std::size_t>(n++)] = {score(g, farm::plan_rounds(s, target), g.centre), 2};
    } else {
        cands[static_cast<std::size_t>(n++)] = {-1e17, 2};   // no known target yet: farm_step chooses one
    }
    std::sort(cands.begin(), cands.begin() + n, [](const Cand& a, const Cand& b) { return a.q > b.q; });
    for (int i = 0; i < n; ++i) {
        const int kind = cands[static_cast<std::size_t>(i)].kind;
        if (kind == 0 && farm::finish_capture(s)) { s.step = "capture"; return true; }
        if (kind == 1 && contest_capture(s)) { s.step = "contest"; return true; }
        if (kind == 2 && farm::farm_step(s)) { s.step = "farm"; return true; }
    }
    return false;
}

void run_support(Snapshot& s) {
    s.step = "support";
    actions::heal_best(s);   // the escort candidate (walk to the scorer) arrives with step b
}

// Step d (EXPLORE): up to LEVEL2_CANDIDATES stale valuable groups, Q = 0.9 P(available) reward -
// P(death) L_D - T r, with P(available) = 1 when the belief says alive or back by arrival, else 0.5.
// The best runs if its Q is positive; otherwise v13's idle step. (A danger record as a scouting target
// gains nothing in Q - dE = 0 - so it never wins and is not listed.)
bool explore_level2(Snapshot& s) {
    // Pass 1 (cheap): the stale groups with the best expected reward per walked round.
    std::array<std::pair<double, int>, LEVEL2_CANDIDATES> top{};
    int n = 0;
    for (const GroupInfo& g : mapinfo::groups()) {
        if (g.gems + g.gold <= 0 || g.steps_from_us >= FAR_AWAY || !belief::group_stale(g, s.round)) continue;
        const double walk = std::max(1.0, 1.2 * static_cast<double>(g.centre.chebyshev_distance(s.me)));
        const double rate = (g.gems + LAMBDA_GEMS_PER_GOLD * g.gold) / walk;
        int k = std::min(n, LEVEL2_CANDIDATES - 1);
        if (n == LEVEL2_CANDIDATES && rate <= top[static_cast<std::size_t>(k)].first) continue;
        while (k > 0 && top[static_cast<std::size_t>(k - 1)].first < rate) { top[static_cast<std::size_t>(k)] = top[static_cast<std::size_t>(k - 1)]; --k; }
        top[static_cast<std::size_t>(k)] = {rate, g.index};
        n = std::min(n + 1, LEVEL2_CANDIDATES);
    }
    // Pass 2: Q for those.
    int best = NO_GROUP;
    double best_q = 0;
    for (int i = 0; i < n; ++i) {
        const GroupInfo& g = mapinfo::groups()[static_cast<std::size_t>(top[static_cast<std::size_t>(i)].second)];
        const double walk = std::max(1.0, 1.2 * static_cast<double>(g.centre.chebyshev_distance(s.me)));
        const bool available = g.state == GroupState::Alive ||
                               (g.state == GroupState::Respawning && g.respawn_ready_round <= s.round + walk);
        level2::Plan p;
        p.dE = (available ? 1.0 : P_AVAILABLE_UNKNOWN) * (g.gems + LAMBDA_GEMS_PER_GOLD * g.gold);
        p.T = walk;
        p.p_death = level2::p_death(s, p.T, g.centre, &p.enemy_share);
        const double q = level2::q(s, p);
        if (q > best_q) best_q = q, best = g.index;
    }
    if (best == NO_GROUP) return false;
    s.step = "explore";
    return move::travel_toward(s, 4000 + best, move::square(mapinfo::groups()[static_cast<std::size_t>(best)].centre, 2));
}

void run_explore(Snapshot& s) {
    s.step = "idle";
    if (LEVEL2_EXPLORE && level2::active()) {
        if (s.bag >= IDLE_BANK_BAG && bank::reach_steps(s) <= IDLE_BANK_STEPS && bank::go_tribute(s)) return;
        const bool hurt = s.health < 0.5 * s.max_health || (s.health < IDLE_HEAL_HEALTH * s.max_health && mapinfo::steps_from_us(s.me) <= IDLE_HEAL_HOME_STEPS);
        if (hurt && bank::recall_is_safe(s)) { bc::self().recall(); return; }
        if (explore_level2(s)) return;
    }
    idle_step(s);
}

// Rules 5-8 with hysteresis: FARM, EXPLORE and SUPPORT are kept for MODE_HOLD_TURNS turns once chosen
// (rules 1-4 have already had their chance to interrupt before this runs).
void modes_5_to_8(Snapshot& s) {
    support_heal(s);          // v11: free cast for the Support role (outside the modes)
    maybe_lock_altar(s);      // v11: free consumable action (outside the modes)
    const bool holding = (g_mode.mode == Mode::Farm || g_mode.mode == Mode::Explore || g_mode.mode == Mode::Support) &&
                         s.round - g_mode.since < MODE_HOLD_TURNS;
    if (holding) {
        enter(s, g_mode.mode);
        if (g_mode.mode == Mode::Support) run_support(s);
        else if (g_mode.mode == Mode::Explore) run_explore(s);
        else if (!run_farm(s)) run_explore(s);   // a held FARM with nothing to farm this turn waits
        return;
    }
    if (support_wanted(s)) { enter(s, Mode::Support); run_support(s); return; }
    if (scout_wanted(s)) { enter(s, Mode::Explore); run_explore(s); return; }
    if (run_farm(s)) { enter(s, Mode::Farm); return; }
    enter(s, Mode::Explore);
    run_explore(s);
}

// ---- Steps 11-12 --------------------------------------------------------------------------------

void place_ward(const Snapshot& s) {
    auto& me = bc::self();
    if (me.ward_cooldown() > 0 || me.restrictions().contains(bc::buff_restrictions::ward)) return;
    const int group = farm::current_group();
    const vec2 look_at = group != NO_GROUP ? mapinfo::groups()[static_cast<std::size_t>(group)].centre : mapinfo::enemy_centre();
    const bc::bitboard tiles = me.ward_placement_tiles() & ~(mapinfo::terrain_blocked() | bc::vision::ward_tiles() |
                                                             bc::map::spawn_region(bc::team::ally));
    vec2 best = s.me;
    long best_distance = s.me.chebyshev_distance(look_at);
    for (int dx = -2; dx <= 2; ++dx)
        for (int dy = -2; dy <= 2; ++dy) {
            const vec2 q{s.me.x + dx, s.me.y + dy};
            if (!bc::map::is_in_bounds(q) || !tiles.is_set(q)) continue;
            const long d = q.chebyshev_distance(look_at);
            if (d < best_distance) best_distance = d, best = q;
        }
    if (best != s.me) me.place_ward(best);
}

void write_flags(const Snapshot& s, bool dead) {
    const bool home = dead || s.on_our_spawn || mapinfo::in_home(s.me);
    if (s.role == Role::TankFarmer) comms::set_flag(comms::FLAG_MARCELLA_HOME, home);
    if (s.my_type == bc::champion_type::marcella && s.on_our_spawn) {
        const auto items = bc::self().item_count();
        comms::set_flag(comms::FLAG_MARCELLA_JUNGLE, items[bc::item_type::jungle_emblem_of_health] > 0);
    }
    if (s.role == Role::Farmer) comms::set_flag(comms::FLAG_PARTNER_HOME, home);
}

void log_turn(const Snapshot& s) {
    if (!LOG_TURNS) return;
    const int g = farm::current_group();
    const int state = g == NO_GROUP ? -1 : static_cast<int>(mapinfo::groups()[static_cast<std::size_t>(g)].state);
    std::cout << "M " << s.round << " " << bc::to_string(s.my_type) << " " << s.mode << ":" << s.step << "/" << s.detail << " hit " << s.hit << " target " << g << " state "
              << state << " bag " << s.bag << " hp " << static_cast<int>(s.health) << " at " << s.me.x << "," << s.me.y
              << " banked " << s.banked_us << " fuel_left " << bc::fuel_remaining() << "\n";
}

// Level 1 rules 1-8 in the doc's order; the first that applies is this turn's mode. Every rule reuses
// the v13 decision at the same place in the old ladder, so Level 1 alone plays like v13 plus the
// hysteresis. The stand-off (watchdog) and the shop trip stay outside the modes, where they were.
void ladder(Snapshot& s) {
    g_stage = "endgame";
    const bool unstuck = s.round >= g_stuck_until;
    if (unstuck && bank::endgame_or_win(s) && bank::go_tribute(s)) { s.step = "endgame-bank"; enter(s, Mode::Carry); return; }   // 1 CARRY
    g_stage = "retreat";
    if (must_retreat(s)) { s.step = "retreat"; enter(s, Mode::Retreat); retreat_or_recall(s); return; }                    // 2 RETREAT
    g_stage = "enemies";
    if (s.round < g_go_home_until && !s.on_our_spawn) {                                      // watchdog stand-off (outside modes)
        s.step = "standoff";
        if (bank::recall_is_safe(s)) { bc::self().recall(); return; }
        hit_what_is_in_range(s);
        move::travel_toward(s, 3001, bc::map::spawn_region(bc::team::ally));
        return;
    }
    if (combat::handle_enemies(s)) {                                                          // 2 RETREAT (walk away) / 3 FIGHT
        s.step = "enemies";
        enter(s, std::string_view(s.detail) == "walk-away" ? Mode::Retreat : Mode::Fight);
        return;
    }
    g_stage = "bank";
    if (unstuck && bank::handle(s)) { s.step = "bank"; enter(s, Mode::Carry); return; }      // 4 CARRY
    g_stage = "shop-trip";
    if (shop::maybe_trip(s)) { s.step = "shop-trip"; return; }                               // shop trip (outside modes)
    g_stage = "role";
    modes_5_to_8(s);                                                                          // 5 SUPPORT, 6/8 EXPLORE, 7 FARM
}

// v10 watchdog: a champion that stays on one tile without hitting anything for STUCK_ROUNDS, outside the
// legitimate waits (spawn, capture, respawn wait, idle point, channel), drops its current group and skips
// the bank / endgame steps for a while. Example (boss-battle, v9): Vesper held "ranged-alone" outside an
// elk's reach for 1,200 rounds because her tank partner was frozen; this trips after 12 rounds.
bool legit_wait(const Snapshot& s) {
    const std::string_view step = s.step, detail = s.detail;
    return s.on_our_spawn || step == "idle" || step == "heal" || step == "capture" || step == "hold" ||
           detail == "capture" || detail == "respawn-wait" || detail == "hold";
}

void watchdog(const Snapshot& s, bool channel_started) {
    static vec2 last_pos{-1, -1};
    static int still_since = 0;
    if (s.me != last_pos || s.hit || channel_started || legit_wait(s)) still_since = s.round;
    last_pos = s.me;
    if (s.round - still_since < STUCK_ROUNDS) return;
    farm::avoid_group(farm::current_group(), s.round + STUCK_AVOID_ROUNDS);
    g_stuck_until = s.round + STUCK_SKIP_BANK_ROUNDS;
    // Stand-off (islands, v10 vs v9, Oct 25: all ten champions stood within 4 tiles of each other from round
    // 600 to 2000, each side "walking away" with no free tile): break it by going home.
    if (std::string_view(s.step) == "enemies") g_go_home_until = s.round + STANDOFF_HOME_ROUNDS;
    still_since = s.round;
    if (LOG_TURNS) std::cout << "r" << s.round << " watchdog: stuck at " << s.me.x << "," << s.me.y << " step " << s.step << "/" << s.detail << "\n";
}

Snapshot dead_snapshot() {
    Snapshot s;
    auto& me = bc::self();
    s.round = static_cast<int>(bc::round_number());
    s.my_type = me.champion_name();
    s.my_slot = body_slot_of(s.my_type);          // v12: shop::is_capture_buyer reads these - they were
    s.squad = world::squad_of(s.my_type);         // default 0 / NO_SLOT before, misassigning dead buyers
    s.gold = static_cast<int>(me.gold_count());
    s.max_health = 1;                              // p_die-free: the dead only shop
    s.role = world::role_of(s.my_type);
    return s;
}

}  // namespace

void champion_turn() {
    auto& me = bc::self();
    g_stage = "setup";
    if (!world::is_setup()) {
        // Setup continues next turn instead of running out of fuel; a turn that just finished setup only
        // goes on if enough fuel is left for a normal turn.
        if (!world::setup_step() || bc::fuel_remaining() < FUEL_FOR_TURN) return;
    }
    // v13 death autopsy (debug builds only): the last living turn of a champion that is now dead.
    static struct { bool alive = true; std::string step, detail; int hp = 0, bag = 0, allies = 0, enemies = 0, near_enemies = 0, round = 0, home = 0; float hpf = 1; } last;
    debug_block({
        if (!me.is_alive() && last.alive)
            std::cout << "DIED " << bc::to_string(me.champion_name()) << " r" << last.round << " step " << last.step << "/" << last.detail
                      << " hp " << last.hp << " hpf " << last.hpf << " bag " << last.bag << " allies " << last.allies << " enemies " << last.enemies
                      << " enemies<=4 " << last.near_enemies << " home " << last.home << std::endl;
    });
    last.alive = me.is_alive();
    if (!me.is_alive()) {                                            // 1
        const Snapshot s = dead_snapshot();
        shop::buy_all(s);
        write_flags(s, true);
        return;
    }
    if (channeling_or_stunned()) return;                             // 2
    g_stage = "sense";
    Snapshot s = world::sense();
    farm::note_gold(s.gold);
    shop::note_gold(s.gold);
    g_stage = "spawn";
    if (s.on_our_spawn) {                                            // 3
        shop::buy_all(s);
        s.gold = static_cast<int>(me.gold_count());
        if (needs_heal(s)) { s.step = "heal"; hit_what_is_in_range(s); write_flags(s, false); log_turn(s); return; }
    }
    ladder(s);
    const bool channel_started = me.is_recalling() || me.is_submitting_gems();
    watchdog(s, channel_started);
    g_stage = "ward";
    if (!channel_started && bc::fuel_remaining() > FUEL_FOR_WARD) place_ward(s);   // 11
    write_flags(s, false);                                           // 12
    debug_block({
        last.step = s.step ? s.step : ""; last.detail = s.detail ? s.detail : ""; last.hp = static_cast<int>(s.health);
        last.hpf = static_cast<float>(s.health / std::max(1.0, static_cast<double>(s.max_health))); last.bag = s.bag; last.round = s.round;
        last.allies = static_cast<int>(s.allies.size()); last.enemies = static_cast<int>(s.enemies.size());
        last.near_enemies = 0; for (const EnemyNow& e : s.enemies) last.near_enemies += e.pos.chebyshev_distance(s.me) <= 4;
        last.home = mapinfo::steps_from_us(s.me);
    });
    // v13: a turn that ends on our spawn shops last (gold can arrive mid-turn); shop-gate counter.
    if (me.is_alive() && bc::map::spawn_region(bc::team::ally).is_set(me.position()) && me.gold_count() >= 30u &&
        bc::fuel_remaining() > 15000) {
        Snapshot t = s;
        t.gold = static_cast<int>(me.gold_count());
        shop::buy_all(t);
    }
    debug_block({
        if (me.is_alive() && bc::map::spawn_region(bc::team::ally).is_set(me.position()) && me.gold_count() >= 30u &&
            me.champion_name() != bc::champion_type::tavi)
            std::cout << "SHOPLEFT " << bc::to_string(me.champion_name()) << " r" << s.round << " gold " << me.gold_count() << "\n";
    });
    log_turn(s);
}
