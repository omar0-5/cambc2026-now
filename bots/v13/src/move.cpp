// move.cpp - path steps, dashes and fleeing, using only bc::distance_field and bc::bitboard. [S5]
//
// Worked example (dash): Heart of Ruin is 14 steps from his target and no enemy is in sight. The field
//   says step 1 = (10, 5), step 2 = (11, 6); both free -> dash to (11, 6) (2 tiles for one turn), then
//   still walk 1 more step this turn. In a fight the dash is kept for its landing stun instead.
// FUEL: one compute_to per call (~1k on 40x40, up to ~28k on 100x100) + ~2k per move attempt.
#include "move.h"
#include <algorithm>
#include <functional>
#include "config.h"
#include "mapinfo.h"

namespace move {
namespace {

bool on_map(vec2 p) { return p.x >= 0 && p.y >= 0 && p.x < bc::map::width() && p.y < bc::map::height(); }

// Known occupants the vision lists can miss (an ally inside a bush we cannot see into): moving there
// throws (10k+ fuel), so ask the engine first (~200 fuel for the three checks).
bool known_occupied(vec2 p) {
    return bc::vision::has_champion(p) || bc::vision::has_camp(p) || bc::vision::has_teleport(p);
}

// Try each step in order; a false move means "hidden unit there", so try the next one.
bool try_steps(Snapshot& s, const std::vector<vec2>& deltas, const bc::bitboard& walls) {
    auto& me = bc::self();
    // v13 correctness: a stunned/rooted or dead (Ren retaliation) champion cannot move (error 21).
    if (!me.is_alive() || !me.can_move()) return false;
    for (vec2 d : deltas) {
        const vec2 to = s.me + d;
        if (!on_map(to) || walls.is_set(to) || known_occupied(to)) continue;
        if (me.move(d)) {
            s.me = to;
            s.moved = true;
            return true;
        }
    }
    return false;
}

int field_to_me(const Snapshot& s, const bc::bitboard& walls, const bc::bitboard& goals, bc::distance_field& field) {
    bc::bitboard visited;
    const bc::bitboard me_only = square(s.me, 0);
    field.compute_to(walls, goals, me_only, visited);
    const std::uint16_t d = field.get(s.me);
    return d == bc::distance_field::unreachable ? FAR_AWAY : d;
}

const bc::ability* ready_dash(const std::array<bc::ability, 2>& abilities) {
    for (const bc::ability& a : abilities)
        if (a.is_dash() && a.cooldown() == 0) return &a;
    return nullptr;
}

// Dash two steps along the field on long trips with no enemy in sight. True if we dashed.
bool try_dash(Snapshot& s, const bc::distance_field& field, int distance) {
    auto& me = bc::self();
    if (distance < 4 || !s.enemies.empty() || !me.can_cast()) return false;
    const auto abilities = me.abilities();
    const bc::ability* dash = ready_dash(abilities);
    if (!dash) return false;
    vec2 cur = s.me;
    for (int k = 0; k < 2; ++k) {
        const auto next = field.next_move_from(cur);
        if (!next) break;
        cur = cur + *next;
        if (known_occupied(cur)) return false;   // a dash path through a unit is rejected (error 32)
    }
    if (cur == s.me || field.get(cur) >= distance) return false;
    // Heart of Ruin's dash lands with damage: never land next to a sleeping monster (it would wake up).
    for (const CampNow& c : s.camps)
        if (c.pos.chebyshev_distance(cur) <= 1) return false;
    if (!dash->use(cur)) return false;
    s.me = cur;
    s.moved = true;
    return true;
}

// Every neighbour that does not increase the distance (used when all best steps are blocked).
std::vector<vec2> sideways_steps(const Snapshot& s, const bc::bitboard& walls, const bc::distance_field& field, int distance) {
    std::vector<vec2> out;
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy) {
            const vec2 q{s.me.x + dx, s.me.y + dy};
            if ((dx || dy) && on_map(q) && !walls.is_set(q) && field.get(q) <= distance) out.push_back(vec2{dx, dy});
        }
    return out;
}

bool step_with_walls(Snapshot& s, const bc::bitboard& walls, const bc::bitboard& goals) {
    auto& me = bc::self();
    if (bc::fuel_remaining() < FUEL_FOR_PATH) return false;   // a field can cost ~25k: skip this turn
    bc::distance_field field;
    const int distance = field_to_me(s, walls, goals, field);
    if (distance >= FAR_AWAY) return false;
    const bool dashed = try_dash(s, field, distance);
    if (dashed) {
        if (!me.can_move()) return true;
        return step_with_walls(s, walls, goals) || true;
    }
    if (try_steps(s, field.next_moves_from(s.me), walls)) return true;
    return try_steps(s, sideways_steps(s, walls, field, distance), walls);
}

// The engine keeps the field; we never export it (exporting a 100x100 field costs ~23k fuel, while
// reading the 9 tiles around us costs a few hundred).
struct CachedField { int key = -1; bc::distance_field field; int last_used = -1; };

std::vector<CachedField>& field_cache() {
    static std::vector<CachedField> cache(6);
    return cache;
}

// The cached field for `key`, or nullptr when there is not enough fuel to build it this turn.
const bc::distance_field* cached_field(int key, const bc::bitboard& goals, int round) {
    auto& cache = field_cache();
    CachedField* slot = &cache[0];
    for (CachedField& c : cache) {
        if (c.key == key) { c.last_used = round; return &c.field; }
        if (c.last_used < slot->last_used) slot = &c;
    }
    if (bc::fuel_remaining() < FUEL_FOR_PATH) return nullptr;
    const bc::bitboard walls = mapinfo::terrain_blocked() | mapinfo::all_camp_spawns();
    bc::bitboard visited;
    slot->field.compute(walls, goals & ~walls, visited);
    slot->key = key;
    slot->last_used = round;
    return &slot->field;
}

int read(const bc::distance_field& field, vec2 p) {
    if (!on_map(p)) return FAR_AWAY;
    const std::uint16_t v = field.get(p);
    return v == bc::distance_field::unreachable ? FAR_AWAY : v;
}

// One step downhill on a precomputed distance map, skipping tiles blocked right now.
bool step_downhill(Snapshot& s, const std::function<int(vec2)>& distance_at) {
    const int here = distance_at(s.me);
    if (here >= FAR_AWAY || here == 0) return false;
    const bc::bitboard walls = blocked_now(s, false);
    std::vector<std::pair<int, vec2>> options;
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy) {
            const vec2 q{s.me.x + dx, s.me.y + dy};
            const int d = on_map(q) ? distance_at(q) : FAR_AWAY;
            if ((dx || dy) && d < here) options.push_back({d, vec2{dx, dy}});
        }
    std::sort(options.begin(), options.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<vec2> deltas;
    for (const auto& o : options) deltas.push_back(o.second);
    return try_steps(s, deltas, walls);
}

}  // namespace

bool travel_toward(Snapshot& s, int key, const bc::bitboard& goals) {
    if (s.cast_exclusive || !bc::self().can_move() || goals.is_set(s.me)) return false;
    const bc::distance_field* field = cached_field(key, goals, s.round);
    if (!field) return false;   // out of fuel for a new field this turn: wait one turn
    const int here = read(*field, s.me);
    if (here < NEAR_TRIP || here >= FAR_AWAY) return step_toward(s, goals);
    return step_downhill(s, [&](vec2 p) { return read(*field, p); }) || step_toward(s, goals);
}

bool travel_to_altar(Snapshot& s, const bc::bitboard& goals) {
    if (s.cast_exclusive || !bc::self().can_move() || goals.is_set(s.me)) return false;
    const int here = mapinfo::steps_to_altar(s.me);
    const bool all_altars = goals == mapinfo::safe_altar_ring();   // the map's field leads to these
    if (here < NEAR_TRIP || here >= FAR_AWAY || !all_altars) return step_toward(s, goals);
    return step_downhill(s, [](vec2 p) { return mapinfo::steps_to_altar(p); }) || step_toward(s, goals);
}

bool step_toward_safe_altar(Snapshot& s) {
    if (s.cast_exclusive || !bc::self().can_move()) return false;
    return step_downhill(s, [](vec2 p) { return mapinfo::steps_to_altar(p); });
}

bc::bitboard square(vec2 p, int radius) {
    bc::bitboard b;
    b.clear();
    b.draw_chebyshev(p, static_cast<float>(radius));
    return b;
}

bc::bitboard ring(vec2 p, int radius) {
    bc::bitboard outer = square(p, radius);
    if (radius > 0) outer &= ~square(p, radius - 1);
    return outer;
}

bc::bitboard blocked_now(const Snapshot& s, bool allow_dead_spawns) {
    // Cached for the current round, tile and flag: a turn asks for it several times (~5-10k fuel each).
    static int cached_round = -1;
    static vec2 cached_at{-1, -1};
    static bool cached_flag = false;
    static bc::bitboard cached;
    if (cached_round == s.round && cached_at == s.me && cached_flag == allow_dead_spawns) return cached;
    bc::bitboard b = mapinfo::terrain_blocked() | bc::vision::champion_tiles() | bc::vision::camp_tiles() |
                     bc::vision::teleport_tiles();
    if (!allow_dead_spawns) {
        // Standing on a dead group's spawn tile delays its whole respawn [S4].
        for (const GroupInfo& g : mapinfo::groups())
            if ((g.state == GroupState::Respawning || g.state == GroupState::RewardAvailable) &&
                g.centre.chebyshev_distance(s.me) <= GROUP_SEEN_RANGE)
                b |= g.spawn;
    }
    b.set(s.me, false);
    cached_round = s.round;
    cached_at = s.me;
    cached_flag = allow_dead_spawns;
    cached = b;
    return b;
}

bool step_toward(Snapshot& s, const bc::bitboard& goals, const bc::bitboard* avoid) {
    if (s.cast_exclusive || !bc::self().can_move() || goals.is_set(s.me)) return false;
    const bool capture_goal = !(goals & mapinfo::all_camp_spawns()).is_empty();
    const bc::bitboard walls = blocked_now(s, capture_goal);
    if (avoid) {
        bc::bitboard harder = walls | *avoid;
        harder.set(s.me, false);
        const bc::bitboard safe_goals = goals & ~harder;
        if (!safe_goals.is_empty() && step_with_walls(s, harder, safe_goals)) return true;
    }
    const bc::bitboard free_goals = goals & ~walls;
    if (free_goals.is_empty()) return false;
    return step_with_walls(s, walls, free_goals);
}

int path_steps(const Snapshot& s, const bc::bitboard& goals) {
    if (goals.is_set(s.me)) return 0;
    const bc::bitboard walls = blocked_now(s, false);
    const bc::bitboard free_goals = goals & ~walls;
    if (free_goals.is_empty()) return FAR_AWAY;
    bc::distance_field field;
    return field_to_me(s, walls, free_goals, field);
}

bool step_away(Snapshot& s, const std::vector<vec2>& threats) {
    if (threats.empty() || s.cast_exclusive || !bc::self().can_move()) return false;
    const bc::bitboard walls = blocked_now(s, false);
    auto score = [&](vec2 p) {
        long nearest = 1000;
        for (vec2 t : threats) nearest = std::min(nearest, static_cast<long>(p.chebyshev_distance(t)));
        return nearest * 1000 - mapinfo::steps_from_us(p);   // ties: the tile nearer home
    };
    const long here = score(s.me);
    std::vector<std::pair<long, vec2>> options;
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy) {
            const vec2 q{s.me.x + dx, s.me.y + dy};
            if ((dx || dy) && on_map(q) && !walls.is_set(q) && score(q) > here) options.push_back({score(q), vec2{dx, dy}});
        }
    std::sort(options.begin(), options.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<vec2> deltas;
    for (const auto& o : options) deltas.push_back(o.second);
    return try_steps(s, deltas, walls);
}

}  // namespace move
