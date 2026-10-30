// roles.cpp - the ladder (roles.h) and the role steps. [S5]
//
// Each ladder step is one small function returning true when it used the turn's main decision.
// A step may still cast or attack before returning (survival casts while stepping away).
// Worked example (step 3): Marcella recalls home at 40% health. The recall lands at the START of her
//   turn; spawn tiles heal at the END of a turn spent on them, so she stays this one turn
//   (health < SPAWN_STAY_HEALTH = 95%), shops, and leaves at full health next turn.
#include "roles.h"
#include <algorithm>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include "actions.h"
#include "bank.h"
#include "combat.h"
#include "comms.h"
#include "config.h"
#include "disrupt.h"
#include "farm.h"
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

void retreat_or_recall(Snapshot& s) {
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
bool contest_capture(Snapshot& s) {
    if (s.bag > CONTEST_MAX_BAG || s.health < CONTEST_MIN_HEALTH * s.max_health) return false;
    const GroupInfo* best = nullptr;
    long best_distance = 1L << 30;
    for (const GroupInfo& g : mapinfo::groups()) {
        if (g.enemy_capture_timer < 0) continue;
        const double reward = g.gems + LAMBDA_GEMS_PER_GOLD * g.gold;
        if (reward < CONTEST_MIN_REWARD) continue;
        const int walk = mapinfo::steps_to_group(g.index, s.me);   // step 0c: real walk when known
        const long d = walk >= 0 ? walk : g.centre.chebyshev_distance(s.me);
        if (d > g.enemy_capture_timer + 2 || d >= best_distance) continue;   // cannot arrive in time
        best = &g;
        best_distance = d;
    }
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

void role_step(Snapshot& s) {
    s.step = "role";
    support_heal(s);          // v11: free cast for the Support role
    maybe_lock_altar(s);      // v11: free consumable action
    if (disrupt::step(s)) { s.step = "disrupt"; return; }
    if (contest_capture(s)) { s.step = "contest"; return; }
    if (farm::farm_step(s)) { s.step = "farm"; return; }
    s.step = "idle";
    idle_step(s);
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
    std::cout << "M " << s.round << " " << bc::to_string(s.my_type) << " " << s.step << "/" << s.detail << " hit " << s.hit << " target " << g << " state "
              << state << " bag " << s.bag << " hp " << static_cast<int>(s.health) << " at " << s.me.x << "," << s.me.y
              << " banked " << s.banked_us << " fuel_left " << bc::fuel_remaining() << "\n";
}

// Steps 4-10. Returns after the first step that used the turn.
void ladder(Snapshot& s) {
    g_stage = "endgame";
    const bool unstuck = s.round >= g_stuck_until;
    if (unstuck && bank::endgame_or_win(s) && bank::go_tribute(s)) { s.step = "endgame-bank"; return; }   // 4 (v10: only if it acted)
    g_stage = "retreat";
    if (must_retreat(s)) { s.step = "retreat"; retreat_or_recall(s); return; }                // 5                // 5
    g_stage = "enemies";
    if (s.round < g_go_home_until && !s.on_our_spawn) {                                      // 6a stand-off
        s.step = "standoff";
        if (bank::recall_is_safe(s)) { bc::self().recall(); return; }
        hit_what_is_in_range(s);
        move::travel_toward(s, 3001, bc::map::spawn_region(bc::team::ally));
        return;
    }
    if (combat::handle_enemies(s)) { s.step = "enemies"; return; }                            // 6
    g_stage = "bank";
    if (unstuck && bank::handle(s)) { s.step = "bank"; return; }                                         // 7
    g_stage = "shop-trip";
    if (shop::maybe_trip(s)) { s.step = "shop-trip"; return; }                               // 8
    g_stage = "capture";
    if (farm::finish_capture(s)) { s.step = "capture"; return; }                              // 9
    g_stage = "role";
    role_step(s);                                                                             // 10
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
        mapinfo::build_group_field_step();   // step 0c: a dead turn has fuel to spare
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
    mapinfo::build_group_field_step();   // step 0c: leftover fuel only (it stops below FUEL_FOR_GROUP_FIELD)
}
