// actions.cpp - spell and basic-attack target choice. Every action is checked first, because a
// rejected action throws bc::error (10k+ fuel). [S6]
//
// Worked example (Heart of Ruin vs an enemy Stellar 2 tiles away): dash_onto() looks for a free tile
//   within 2 of him and next to her; the landing deals 1.5 x 100 = 150 damage and stuns her for a turn,
//   then attack_best() adds a 30-damage basic attack (melee may walk/dash and attack in one turn).
#include "actions.h"
#include <algorithm>
#include <iostream>
#include <optional>
#include <variant>
#include "config.h"
#include "mapinfo.h"

namespace actions {
namespace {

struct SlotFacts { float range = 0, modifier = 0; bool exclusive = false; };

SlotFacts facts_of(const bc::ability& a) {
    SlotFacts f;
    const auto& info = a.info();
    if (!info) return f;
    std::visit([&](const auto& p) {
        f.exclusive = p.exclusive_with_movement;
        if constexpr (requires { p.range; }) f.range = p.range;
        if constexpr (requires { p.modifier; }) f.modifier = p.modifier;
    }, *info);
    return f;
}

bool still_alive(const EnemyNow& e) { return e.handle.is_visible() && e.handle.health() > 0; }
bool still_alive(const CampNow& c) { return c.handle.is_visible() && c.handle.health() > 0; }

// Which camps we may hit: our farming target, or anything already awake (it is hitting someone).
bool camp_allowed(const CampNow& c, const Aim& aim) {
    if (!aim.hit_camps) return false;
    if (c.kind == bc::camp_type::monolith && c.group != aim.focus_group) return false;
    return c.group == aim.focus_group || c.awake;
}

// Best enemy in `reach` for this slot: focus target, then channelers (for stuns), then low health.
int enemy_in_reach(const Snapshot& s, const bc::bitboard& reach, const Aim& aim, bool stun) {
    int best = -1;
    double best_score = -1e18;
    for (std::size_t i = 0; i < s.enemies.size(); ++i) {
        const EnemyNow& e = s.enemies[i];
        if (e.on_their_spawn || !reach.is_set(e.pos)) continue;
        double score = -e.health + 4.0 * e.bag;
        if (static_cast<int>(i) == aim.focus_enemy) score += 1000;
        if (stun && e.channeling) score += 2000;   // a stun cancels a tribute or a recall
        if (stun && e.stunned) score -= 1500;      // already stunned: wasted
        if (score > best_score) best_score = score, best = static_cast<int>(i);
    }
    return best;
}

// An area spell or a dash landing also hits camps around the target tile: never splash a sleeping
// monster of another group (a sleeping monolith next to a gold cluster would wake up and kill us).
bool splash_wakes_monster(const Snapshot& s, vec2 centre, int focus_group) {
    for (const CampNow& c : s.camps)
        if (!c.awake && c.strength > 0 && c.group != focus_group && c.pos.chebyshev_distance(centre) <= 1) return true;
    return false;
}

// Best camp in `reach`: the one a hit kills (clusters: 120 health), else the lowest health.
const CampNow* camp_in_reach(const Snapshot& s, const bc::bitboard& reach, const Aim& aim, bool splash) {
    const CampNow* best = nullptr;
    for (const CampNow& c : s.camps) {
        if (!camp_allowed(c, aim) || !reach.is_set(c.pos)) continue;
        if (splash && splash_wakes_monster(s, c.pos, c.group)) continue;
        if (!best || c.health < best->health) best = &c;
    }
    return best;
}

bool cast_on_enemy(Snapshot& s, const bc::ability& a, int index) {
    const EnemyNow& e = s.enemies[static_cast<std::size_t>(index)];
    if (!still_alive(e)) return false;
    return a.can_target_ground() ? a.use(e.pos) : (a.can_target_enemies() && a.use(e.handle));
}

bool cast_on_camp(const bc::ability& a, const CampNow& c) {
    if (!still_alive(c)) return false;
    return a.can_target_ground() ? a.use(c.pos) : (a.can_target_camps() && a.use(c.handle));
}

// Damage, area damage or stun slot: enemy champions first, then camps (pure stuns: champions only).
bool cast_offensive(Snapshot& s, const bc::ability& a, const SlotFacts& f, const Aim& aim) {
    const bc::bitboard reach = a.valid_tiles();
    const bool stun = a.is_cc();
    const bool splash = a.is_aoe_damage();
    const int enemy = enemy_in_reach(s, reach, aim, stun);
    if (enemy >= 0 && !(splash && splash_wakes_monster(s, s.enemies[static_cast<std::size_t>(enemy)].pos, aim.focus_group)) &&
        cast_on_enemy(s, a, enemy))
        return true;
    if (stun && f.modifier <= 0.0f) return false;   // a damage-less stun is kept for champions
    const CampNow* camp = camp_in_reach(s, reach, aim, splash);
    // Do not waste a big spell on a cluster that a basic attack would kill anyway... unless it dies now.
    return camp && cast_on_camp(a, *camp);
}

bool threatened(const Snapshot& s) {
    for (const EnemyNow& e : s.enemies)
        if (!e.on_their_spawn && e.pos.chebyshev_distance(s.me) <= e.attack_range + 1) return true;
    for (const CampNow& c : s.camps)
        if (c.awake && c.strength > 0 && c.pos.chebyshev_distance(s.me) <= c.attack_range) return true;
    return false;
}

void mark_cast(Snapshot& s, const SlotFacts& f) {
    if (f.exclusive) s.cast_exclusive = true;
}

}  // namespace

bool use_shield_if_hit(Snapshot& s) {
    auto& me = bc::self();
    if (!me.can_cast() || !threatened(s) || s.health > SHIELD_BELOW_HEALTH * s.max_health) return false;
    for (const bc::ability& a : me.abilities()) {
        if (!a.is_shield() || a.cooldown() > 0 || !a.can_target_self()) continue;
        if (a.use()) { mark_cast(s, facts_of(a)); return true; }
    }
    return false;
}

// A dash may cross holes but not walls, altars or units: the landing needs a free middle tile when it
// is 2 tiles away (a 1-tile dash needs none).
bool dash_path_clear(vec2 from, vec2 land) {
    if (from.chebyshev_distance(land) <= 1) return true;
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy) {
            const vec2 mid{from.x + dx, from.y + dy};
            if ((!dx && !dy) || mid.chebyshev_distance(land) != 1 || !bc::map::is_in_bounds(mid)) continue;
            if (bc::map::walls().is_set(mid) || bc::map::altars().is_set(mid)) continue;
            if (bc::vision::has_champion(mid) || bc::vision::has_camp(mid)) continue;
            return true;
        }
    return false;
}

bool landing_ok(const Snapshot& s, vec2 land, const bc::bitboard& reach) {
    if (land == s.me || !bc::map::is_in_bounds(land) || !reach.is_set(land) || land.chebyshev_distance(s.me) > 2) return false;
    if (mapinfo::terrain_blocked().is_set(land) || bc::map::spawn_region(bc::team::ally).is_set(land)) return false;
    if (!bc::vision::in_vision(land) || bc::vision::has_champion(land) || bc::vision::has_camp(land) || bc::vision::has_teleport(land))
        return false;
    if (splash_wakes_monster(s, land, NO_GROUP)) return false;
    return dash_path_clear(s.me, land);
}

bool dash_onto(Snapshot& s, vec2 target) {
    auto& me = bc::self();
    if (!me.can_cast()) return false;
    for (const bc::ability& a : me.abilities()) {
        if (!a.is_dash() || a.cooldown() > 0 || facts_of(a).modifier <= 0.0f) continue;
        const bc::bitboard reach = a.valid_tiles();
        // v13 correctness: valid_tiles() is the casting range only; the landing also needs a legal dash
        // path of <= 2 steps (walls, altars, units and ENEMY SPAWN tiles block it, holes don't) or the
        // engine throws error 32 (elk-arena: diagonal dash across a corner of their spawn).
        bc::bitboard path_block = bc::map::walls() | bc::map::altars() | bc::vision::champion_tiles() | bc::vision::camp_tiles() |
                                  bc::map::spawn_region(bc::team::enemy);
        path_block.set(me.position(), false);
        const bc::bitboard path_ok = path_block.reachable_tiles(me.position(), 2);
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy) {
                const vec2 land{target.x + dx, target.y + dy};
                if ((!dx && !dy) || !landing_ok(s, land, reach)) continue;
                if (land.x < 0 || land.y < 0 || land.x >= bc::map::width() || land.y >= bc::map::height() || !path_ok.is_set(land)) continue;
                if (a.use(land)) { s.me = land; s.moved = true; return true; }
            }
    }
    return false;
}

bool has_ready_stun(const Snapshot& s) {
    (void)s;
    for (const bc::ability& a : bc::self().abilities())
        if ((a.is_cc() || (a.is_dash() && facts_of(a).modifier > 0)) && a.cooldown() == 0) return true;
    return false;
}

bool cast_best(Snapshot& s, bool will_move, const Aim& aim) {
    auto& me = bc::self();
    if (!me.can_cast()) return false;
    for (const bc::ability& a : me.abilities()) {
        if (!a.info() || a.cooldown() > 0) continue;
        const SlotFacts f = facts_of(a);
        if (f.exclusive && (will_move || s.moved)) continue;
        if (a.is_attack_reset() || a.is_dash() || a.is_heal()) continue;   // used elsewhere
        if (a.is_shield()) {
            if (use_shield_if_hit(s)) return true;
            continue;
        }
        if (cast_offensive(s, a, f, aim)) { mark_cast(s, f); s.hit = true; return true; }
    }
    return false;
}

namespace {

// One basic attack on the best target in range; true if it landed.
bool basic_attack_once(Snapshot& s, const Aim& aim) {
    auto& me = bc::self();
    if (!me.is_alive() || !me.can_attack()) return false;
    s.me = me.position();   // v13 correctness: measure range from where we really are (error 43)
    const int range = me.attack_range();
    int best_enemy = -1;
    for (std::size_t i = 0; i < s.enemies.size(); ++i) {
        const EnemyNow& e = s.enemies[i];
        if (e.on_their_spawn || e.pos.chebyshev_distance(s.me) > range || !still_alive(e)) continue;
        if (best_enemy < 0 || static_cast<int>(i) == aim.focus_enemy ||
            e.health < s.enemies[static_cast<std::size_t>(best_enemy)].health)
            best_enemy = static_cast<int>(i);
    }
    if (best_enemy >= 0) { me.attack(s.enemies[static_cast<std::size_t>(best_enemy)].handle); return true; }
    const CampNow* best_camp = nullptr;
    for (const CampNow& c : s.camps)
        if (camp_allowed(c, aim) && c.pos.chebyshev_distance(s.me) <= range && still_alive(c) &&
            (!best_camp || c.health < best_camp->health))
            best_camp = &c;
    if (!best_camp) return false;
    // v13 correctness: the snapshot is from turn start; the camp may have died this turn (our first hit,
    // an ally's) - attacking it throws attack_bad_target.
    if (!best_camp->handle.is_visible() || best_camp->handle.health() <= 0) return false;
    me.attack(best_camp->handle);
    return true;
}

}  // namespace

bool attack_best(Snapshot& s, const Aim& aim) {
    auto& me = bc::self();
    if (!basic_attack_once(s, aim)) return false;
    s.hit = true;
    if (!me.can_cast()) return true;
    for (const bc::ability& a : me.abilities()) {
        if (!a.is_attack_reset() || a.cooldown() > 0) continue;
        // A reset throws ("an available attack cannot be reset") when the attack allowance was not
        // consumed - skip it then (v11: this fired 3x per bench through fight footwork).
        if (me.can_attack()) break;
        if (a.use() && me.can_attack()) basic_attack_once(s, aim);   // a second hit this turn
        break;
    }
    return true;
}

bool heal_best(Snapshot& s) {
    auto& me = bc::self();
    if (!me.can_cast()) return false;
    // me.abilities() returns the array BY VALUE: keep a copy, never a pointer into the temporary
    // (a dangling element made the cast throw "action is only available for self" every turn).
    std::optional<bc::ability> heal;
    for (const bc::ability& a : me.abilities())
        if (a.is_heal() && a.cooldown() == 0 && a.can_target_allies()) heal = a;
    if (!heal) return false;
    const float range = facts_of(*heal).range;
    const AllyNow* best = nullptr;
    float best_missing = static_cast<float>(HEAL_MIN_MISSING);
    for (const AllyNow& a : s.allies) {
        if (!a.handle.is_visible() || a.handle.health() <= 0) continue;
        if (a.pos.chebyshev_distance(s.me) > static_cast<long>(range)) continue;   // out of range would throw
        const float missing = a.handle.max_health() - a.handle.health();
        if (missing > best_missing) best_missing = missing, best = &a;
    }
    if (!best) return false;
    if (!heal->use(best->handle)) return false;
    mark_cast(s, facts_of(*heal));
    return true;
}

}  // namespace actions
