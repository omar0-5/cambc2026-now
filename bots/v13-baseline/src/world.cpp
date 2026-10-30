// world.cpp - per-turn snapshot, enemy memory, group timers and sightings. [S5]
//
// Worked example (group timer): round 100, Marcella sees a wolf group's spawn tiles, no wolf on them,
//   and the engine says camp_group_respawning{timer = 31} -> state Respawning, ready in round 131.
//   Later, out of sight, every champion still knows "back in round 131" from its own memory.
// FUEL: sense() ~5-15k (vision lists, one visible-tile export, group checks only within 12 tiles).
#include "world.h"
#include <algorithm>
#include <iostream>
#include <span>
#include <variant>
#include "config.h"
#include "mapinfo.h"

namespace world {
namespace {

struct WorldData {
    bool setup = false;
    bool plan_read = false;
    comms::Plan plan;
    bc::champion_type my_type = bc::champion_type::ryth;
    int my_slot = NO_SLOT;
    Role role = Role::Farmer;
    int squad = 0;
    std::array<EnemyMemory, 13> enemies{};
    std::vector<std::uint8_t> visible_bits;
    int width = 0;
};

WorldData& data() {
    static WorldData d;
    return d;
}

// Fallback roles when there is no plan (draft crashed) or this champion is not in our lineup.
void fallback_role(WorldData& d) {
    const std::size_t n = bc::champions(bc::team::ally).size();
    switch (d.my_type) {   // v10/v11 formations (draft.cpp draft_set())
        case bc::champion_type::marcella: d.role = Role::TankFarmer; d.squad = 0; break;
        case bc::champion_type::ryth: d.role = Role::Farmer; d.squad = 0; break;
        case bc::champion_type::vendetta: d.role = Role::Farmer; d.squad = 0; break;
        case bc::champion_type::heart_of_ruin: d.role = n >= 5 ? Role::FarPair : Role::Farmer; d.squad = n >= 5 ? 1 : 0; break;
        case bc::champion_type::vesper_vale: d.role = n >= 5 ? Role::FarPair : Role::Farmer; d.squad = n >= 5 ? 1 : 0; break;
        case bc::champion_type::cassi: d.role = Role::DisruptorCaster; d.squad = 0; break;   // v11
        case bc::champion_type::issac: d.role = Role::DisruptorMelee; d.squad = 0; break;    // v11
        case bc::champion_type::willow: d.role = Role::Support; d.squad = 0; break;          // v11
        default: d.role = Role::Farmer; d.squad = 0; break;
    }
}

// Copy the draft's per-group distances into mapinfo (saves recomputing nothing but keeps one truth).
void apply_plan_to_groups(const comms::Plan& plan) {
    auto& groups = mapinfo::groups();
    if (plan.groups.size() != groups.size()) { mapinfo::classify_groups_now(); return; }
    for (std::size_t i = 0; i < groups.size(); ++i) {
        const comms::GroupPlan& gp = plan.groups[i];
        groups[i].steps_from_us = gp.steps_from_us == NO_DISTANCE ? FAR_AWAY : gp.steps_from_us;
        groups[i].steps_from_enemy = gp.steps_from_enemy == NO_DISTANCE ? FAR_AWAY : gp.steps_from_enemy;
        groups[i].steps_to_altar = gp.steps_to_altar == NO_DISTANCE ? FAR_AWAY : gp.steps_to_altar;
        groups[i].nearest_altar = gp.nearest_altar == 0xFF ? -1 : gp.nearest_altar;
        groups[i].in_enemy_zone = gp.flags & 1;
        groups[i].in_home = gp.flags & 2;
        groups[i].in_middle_band = gp.flags & 4;
    }
}

int priority_of(bc::champion_type t) {
    // Marcella acts first so monsters pick her as the nearest champion [S5]. Willow heals early in
    // the round (before allies act and take damage-free positioning); Cassi after the melee bodies.
    switch (t) {
        case bc::champion_type::marcella: return 30;
        case bc::champion_type::ryth: return 28;
        case bc::champion_type::willow: return 25;   // v11
        case bc::champion_type::heart_of_ruin: return 20;
        case bc::champion_type::vendetta: return 15;
        case bc::champion_type::issac: return 14;    // v11
        case bc::champion_type::cassi: return 12;    // v11
        default: return 10;
    }
}

EnemyNow read_enemy(const bc::champion& c) {
    const bc::buff_collection buffs = c.buffs();
    const float shield = buffs.has_shield() ? buffs.get<bc::buff_shield>().value : 0.0f;
    const vec2 p = c.position();
    const bool channel = c.is_submitting_gems() || c.is_recalling();
    return EnemyNow{c, p, c.champion_name(), c.health(), std::max(1.0f, c.max_health()), shield, c.strength(),
                    c.focus(), static_cast<int>(c.gem_count()), c.attack_range(), channel,
                    bc::map::spawn_region(bc::team::enemy).is_set(p), buffs.has_stun()};
}

// Strength and attack range of the monster on spawn tile p (from the group's design stats).
void design_of(int group, vec2 p, float& strength, int& range) {
    if (group == NO_GROUP) return;
    const GroupInfo& g = mapinfo::groups()[static_cast<std::size_t>(group)];
    for (std::size_t i = 0; i < g.tiles.size(); ++i)
        if (g.tiles[i] == p) { strength = g.strengths[i]; range = g.ranges[i]; return; }
}

void read_units(Snapshot& s) {
    // FUEL: visible enemies are read once (read_enemy) and memory is filled from that copy; for the rest
    // we only ask type and alive (~600 fuel each instead of ~3k).
    const int round = s.round;
    for (const bc::champion& c : bc::vision::champions(bc::team::enemy)) {
        const EnemyNow e = read_enemy(c);
        s.enemies.push_back(e);
        EnemyMemory& m = data().enemies[static_cast<std::size_t>(e.type)];
        m.type = e.type; m.exists = true; m.alive = true; m.last_pos = e.pos; m.last_seen_round = round;
        m.health = e.health; m.max_health = e.max_health; m.bag = e.bag; m.channeling = e.channeling;
    }
    if (round % 4 == 0 || round < 3)   // alive flags of unseen enemies: every 4 rounds is enough (~5k fuel)
    for (const bc::champion& c : bc::champions(bc::team::enemy)) {
        const bc::champion_type t = c.champion_name();
        EnemyMemory& m = data().enemies[static_cast<std::size_t>(t)];
        if (m.last_seen_round == round) continue;
        m.type = t;
        m.exists = true;
        m.alive = c.is_alive();
    }
    for (const bc::champion& c : bc::vision::champions(bc::team::ally)) {
        const vec2 p = c.position();
        if (p == s.me) continue;
        s.allies.push_back(AllyNow{c, p, c.champion_name()});
    }
    // FUEL: each engine query costs ~300; monsters never move and their strength / range are design
    // facts, so per camp we only ask for its health (awake state comes from one bitboard).
    const std::vector<bc::camp> camps = bc::vision::camps();
    if (camps.empty()) return;
    const bc::bitboard awake = bc::vision::awake_camp_tiles();
    for (const bc::camp& c : camps) {
        const float hp = c.health();
        if (hp <= 0) continue;
        const vec2 p = c.spawn_position();
        const int group = mapinfo::group_at(p);
        float strength = 0;
        int range = 0;
        design_of(group, p, strength, range);
        s.camps.push_back(CampNow{c, p, group, c.kind(), hp, strength, range, awake.is_set(p)});
    }
}

bool group_tile_visible(const GroupInfo& g) {
    for (vec2 t : g.tiles)
        if (tile_visible(t)) return true;
    return false;
}

// Update what we know about groups we can see (only those within 12 tiles: cheap test first).
void update_groups(const Snapshot& s) {
    auto& groups = mapinfo::groups();
    for (GroupInfo& g : groups) {
        if (g.centre.chebyshev_distance(s.me) > GROUP_SEEN_RANGE || !group_tile_visible(g)) continue;
        g.last_seen_round = s.round;
        const bool monster_seen = std::any_of(s.camps.begin(), s.camps.end(), [&](const CampNow& c) { return c.group == g.index; });
        if (monster_seen) { g.state = GroupState::Alive; g.enemy_capture_timer = -1; continue; }
        const bc::map::camp_group_state st = mapinfo::engine_groups()[static_cast<std::size_t>(g.index)].state();
        if (const auto* r = std::get_if<bc::map::camp_group_respawning>(&st)) {
            if (LOG_TURNS && g.state != GroupState::Respawning)
                std::cout << "r" << s.round << " group " << g.index << " respawning timer " << r->timer << "\n";
            g.state = GroupState::Respawning;
            g.respawn_ready_round = s.round + r->timer;
        } else if (std::get_if<bc::map::camp_group_reward_available>(&st)) {
            g.state = GroupState::RewardAvailable;
            // v11 (engine 2.0.0): exact capture progress, queried once per capture cycle; afterwards the
            // timer counts down locally (one engine call per visible group per turn ran lanes out of
            // fuel). An enemy standing on the region resets on our entry and is stationary for the
            // timer: a cheap contest target (OPTIMAL_STRATEGY §14).
            if (g.enemy_capture_timer < 0 && bc::fuel_remaining() > 30000) {
                const bc::map::camp_group_capture_state cap =
                    mapinfo::engine_groups()[static_cast<std::size_t>(g.index)].capture_state();
                if (cap.state == bc::map::camp_group_capture_type::enemy_capturing) g.enemy_capture_timer = cap.timer;
            }
        } else {
            g.state = GroupState::Alive;
            g.enemy_capture_timer = -1;
        }
    }
    for (GroupInfo& g : groups) {
        if (g.state == GroupState::Respawning && s.round >= g.respawn_ready_round) g.state = GroupState::Unknown;
        if (g.enemy_capture_timer >= 0) --g.enemy_capture_timer;   // counts down with or without us watching
    }
}

// Tell the team where enemies are (bytes 4-6), nearest enemy first [S10].
void write_sighting(const Snapshot& s) {
    const EnemyNow* nearest = nullptr;
    int count = 0;
    bool channeling = false;
    for (const EnemyNow& e : s.enemies) {
        if (e.on_their_spawn || e.pos.chebyshev_distance(s.me) > 8) continue;
        ++count;
        channeling = channeling || e.channeling;
        if (!nearest || e.pos.chebyshev_distance(s.me) < nearest->pos.chebyshev_distance(s.me)) nearest = &e;
    }
    if (nearest) comms::write_sighting(nearest->pos, count, channeling);
}

void log_groups(const WorldData& d) {
    std::cout << "setup " << bc::to_string(d.my_type) << " role " << static_cast<int>(d.role) << " squad " << d.squad
              << " plan " << d.plan.valid << " d15 " << mapinfo::d15() << " d60 " << mapinfo::d60() << "\n";
    for (const GroupInfo& g : mapinfo::groups())
        std::cout << " group " << g.index << " at " << g.centre.x << "," << g.centre.y << " gems " << g.gems << " gold " << g.gold
                  << " hp " << g.total_health << " str " << g.total_strength << " us " << g.steps_from_us << " them "
                  << g.steps_from_enemy << " altar " << g.steps_to_altar << " zone " << g.in_enemy_zone << " home " << g.in_home
                  << " mid " << g.in_middle_band << "\n";
}

}  // namespace

bool setup_step() {
    WorldData& d = data();
    auto& me = bc::self();
    if (!d.plan_read) {
        d.plan = comms::read_plan();
        d.plan_read = true;
        d.my_type = me.champion_name();
        d.my_slot = body_slot_of(d.my_type);
        me.set_priority(priority_of(d.my_type));
    }
    const bool map_planned = d.plan.valid && !d.plan.groups.empty();   // v10: the draft may write slots only
    if (!mapinfo::build_step(map_planned, d.plan.d15, d.plan.d60)) return false;
    if (map_planned) apply_plan_to_groups(d.plan);
    const bool slot_planned = d.plan.valid && d.my_slot != NO_SLOT &&
                              d.plan.type_of_slot[static_cast<std::size_t>(d.my_slot)] == static_cast<std::uint8_t>(d.my_type);
    if (slot_planned) {
        d.squad = d.plan.squad_of_slot[static_cast<std::size_t>(d.my_slot)];
        d.role = static_cast<Role>(d.plan.role_of_slot[static_cast<std::size_t>(d.my_slot)]);
    } else {
        fallback_role(d);
    }
    d.width = bc::map::width();
    d.setup = true;
    if (LOG_TURNS) log_groups(d);
    return true;
}

bool is_setup() { return data().setup; }
const comms::Plan& plan() { return data().plan; }
const std::array<EnemyMemory, 13>& enemy_memory() { return data().enemies; }

bool tile_visible(vec2 p) {
    const WorldData& d = data();
    const int i = p.y * d.width + p.x;
    if (i < 0 || static_cast<std::size_t>(i >> 3) >= d.visible_bits.size()) return false;
    return (d.visible_bits[static_cast<std::size_t>(i >> 3)] >> (i & 7)) & 1;
}

Snapshot sense() {
    WorldData& d = data();
    auto& me = bc::self();
    Snapshot s;
    s.round = static_cast<int>(bc::round_number());
    g_round = s.round;
    s.me = me.position();
    s.my_type = d.my_type;
    s.my_slot = d.my_slot;
    s.role = d.role;
    s.squad = d.squad;
    s.health = me.health();
    s.max_health = std::max(1.0f, me.max_health());
    s.strength = me.strength();
    s.focus = me.focus();
    s.bag = static_cast<int>(me.gem_count());
    s.gold = static_cast<int>(me.gold_count());
    s.attack_range = me.attack_range();
    s.melee = s.attack_range <= 1;
    s.on_our_spawn = bc::map::spawn_region(bc::team::ally).is_set(s.me);
    s.banked_us = static_cast<int>(bc::global_submitted_gems(bc::team::ally));
    s.banked_them = static_cast<int>(bc::global_submitted_gems(bc::team::enemy));
    d.visible_bits = bc::vision::visible_tiles().export_linear_buffer();
    read_units(s);
    update_groups(s);
    write_sighting(s);
    return s;
}

int squad_of(bc::champion_type type) {
    const WorldData& d = data();
    const int slot = body_slot_of(type);
    if (!d.plan.valid || slot == NO_SLOT || d.plan.squad_of_slot[static_cast<std::size_t>(slot)] == comms::UNUSED_SLOT)
        return type == d.my_type ? d.squad : -1;
    return d.plan.squad_of_slot[static_cast<std::size_t>(slot)];
}

Role role_of(bc::champion_type type) {
    const WorldData& d = data();
    const int slot = body_slot_of(type);
    if (type == d.my_type) return d.role;
    if (!d.plan.valid || slot == NO_SLOT) return Role::Farmer;
    return static_cast<Role>(d.plan.role_of_slot[static_cast<std::size_t>(slot)]);
}

namespace {

struct TeamRoll { bc::champion_type type; bool alive; };

// Round each of our champions last died (alive -> dead transition), -10000 = never.
std::array<int, 13>& death_rounds() {
    static std::array<int, 13> rounds{};
    static bool init = false;
    if (!init) {
        rounds.fill(-10000);
        init = true;
    }
    return rounds;
}

// Our champions and whether each is alive, read from the engine once per round (~5k fuel per read).
// v12: also records when each champion died, so farm.cpp can merge squads while the enemy hunts us
// (match 8d0f4189: 21 deaths with the pair split apart for the whole game).
const std::vector<TeamRoll>& team_roll() {
    static int cached_round = -1;
    static std::vector<TeamRoll> roll;
    const int round = static_cast<int>(bc::round_number());
    if (cached_round == round) return roll;
    std::array<bool, 13> was_alive{};
    for (std::size_t i = 0; i < roll.size(); ++i) was_alive[static_cast<std::size_t>(roll[i].type)] = roll[i].alive;
    roll.clear();
    for (const bc::champion& c : bc::champions(bc::team::ally)) {
        const TeamRoll r{c.champion_name(), c.is_alive()};
        if (!r.alive && was_alive[static_cast<std::size_t>(r.type)] && cached_round >= 0)
            death_rounds()[static_cast<std::size_t>(r.type)] = round;
        roll.push_back(r);
    }
    cached_round = round;
    return roll;
}

}  // namespace

int team_deaths_since(int since) {
    team_roll();   // refresh for this round first
    int n = 0;
    for (int r : death_rounds()) n += r >= since ? 1 : 0;
    return n;
}

bool ally_alive(bc::champion_type type) {
    for (const TeamRoll& r : team_roll())
        if (r.type == type) return r.alive;
    return false;
}

bool team_has(bc::champion_type type) {
    for (const TeamRoll& r : team_roll())
        if (r.type == type) return true;
    return false;
}

std::vector<bc::champion_type> squad_members(int squad, bool alive_only) {
    std::vector<bc::champion_type> out;
    for (const TeamRoll& r : team_roll()) {
        if (squad_of(r.type) != squad) continue;
        if (alive_only && !r.alive) continue;
        out.push_back(r.type);
    }
    return out;
}

int alive_enemy_count() {
    int n = 0;
    for (const EnemyMemory& m : data().enemies) n += (m.exists && m.alive) ? 1 : 0;
    return n;
}

}  // namespace world
