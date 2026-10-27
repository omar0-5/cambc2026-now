// mapinfo.cpp - distance maps, zones, camp groups and altars. [S4]
//
// Three distance maps are computed with bc::distance_field and copied into plain arrays:
//   from_us    roots = our spawn      (walls: terrain, monster tiles, enemy spawn)
//   from_enemy roots = enemy spawn    (walls: terrain, monster tiles, our spawn)
//   to_altar   roots = altar ring     (walls: terrain, monster tiles, enemy spawn)
// Worked example (eight, 41x30): a wolf group whose best neighbour tile is 12 steps from our spawn and
// 25 from theirs has ratio 12 / 37 = 0.32 -> not middle band (0.35..0.65), inside our home region.
// FUEL: each field costs ~1k (40x40) to ~28k (100x100); build() runs once on the 2x-fuel first turn.
#include "mapinfo.h"
#include <algorithm>
#include <array>
#include <span>
#include "config.h"

namespace mapinfo {
namespace {


struct MapData {
    bool built = false;
    int width = 0, height = 0;
    std::vector<GroupInfo> groups;
    std::vector<bc::map::camp_group> engine_groups;
    std::vector<std::int16_t> group_of_tile;   // width*height, NO_GROUP if no monster spawns there
    std::vector<vec2> altars;
    bc::bitboard altar_ring, terrain_blocked, camp_spawns;
    std::vector<std::uint16_t> from_us, from_enemy, to_altar;
    std::vector<std::uint16_t> to_altar_open;   // like to_altar, but monster spawn tiles are walkable (gate camps dead)
    bool any_gate = false;
    std::vector<std::uint8_t> wall_bits;        // walls | holes | altars, row-major bits
    bc::bitboard safe_altar_ring;               // altar ring of altars outside the enemy zone
    bool thresholds_set = false;
    int d15 = 0, d60 = 0, home_to_altar = FAR_AWAY;
    vec2 home_centre{}, enemy_centre{};
};

MapData& data() {
    static MapData d;
    return d;
}

int tile_index(vec2 p) { return p.y * data().width + p.x; }

bool on_map(vec2 p) { return p.x >= 0 && p.y >= 0 && p.x < data().width && p.y < data().height; }

// Plain-memory wall test (an engine is_set() call costs ~90 fuel; this costs a few).
bool wall_at(vec2 p) {
    if (!on_map(p)) return true;
    const int i = tile_index(p);
    return (data().wall_bits[static_cast<std::size_t>(i >> 3)] >> (i & 7)) & 1;
}

int read_distance(const std::vector<std::uint16_t>& field, vec2 p) {
    if (!on_map(p) || field.empty()) return FAR_AWAY;
    const std::uint16_t v = field[static_cast<std::size_t>(tile_index(p))];
    return v == bc::distance_field::unreachable ? FAR_AWAY : v;
}

bc::bitboard ring_of(vec2 altar) {
    bc::bitboard b;
    b.clear();
    b.draw_chebyshev(altar, 1.0f);
    return b;
}

// Altars are walls in the fields: an altar's distance is its best walkable neighbour's.
int best_altar_neighbour_enemy(vec2 a) {
    int best = FAR_AWAY;
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy) best = std::min(best, read_distance(data().from_enemy, vec2{a.x + dx, a.y + dy}));
    return best;
}

// Run one engine distance field and copy it into a plain array (cheap lookups afterwards).
std::vector<std::uint16_t> compute_field(const bc::bitboard& walls, const bc::bitboard& roots) {
    bc::distance_field field;
    bc::bitboard visited;
    field.compute(walls, roots & ~walls, visited);
    // Unreached tiles (and walls) already hold distance_field::unreachable (probe, Oct 21).
    // FUEL: never loop over every tile in C++ - a 100x100 loop costs ~80k fuel; the engine call is ~27k.
    return field.export_linear_buffer();
}

vec2 centre_of(const bc::bitboard& mask) {
    const auto tiles = mask.positions();   // spawn regions are small, so this is cheap
    if (tiles.empty()) return vec2{0, 0};
    long sum_x = 0, sum_y = 0;
    for (vec2 t : tiles) sum_x += t.x, sum_y += t.y;
    const long n = static_cast<long>(tiles.size());
    return vec2{static_cast<int>(sum_x / n), static_cast<int>(sum_y / n)};
}

float exposure_at(const GroupInfo& g, vec2 tile) {
    float total = 0;
    for (std::size_t i = 0; i < g.tiles.size(); ++i)
        if (g.tiles[i].chebyshev_distance(tile) <= g.ranges[i]) total += g.strengths[i];
    return total;
}

bool is_group_tile(const GroupInfo& g, vec2 q) {
    return std::find(g.tiles.begin(), g.tiles.end(), q) != g.tiles.end();
}

// The least damage a melee champion takes standing next to any monster of the group.
float best_edge_exposure(const GroupInfo& g) {
    float best = 1e9f;
    for (vec2 t : g.tiles)
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy) {
                const vec2 q{t.x + dx, t.y + dy};
                if ((!dx && !dy) || wall_at(q) || is_group_tile(g, q)) continue;
                best = std::min(best, exposure_at(g, q));
            }
    return best >= 1e9f ? g.total_strength : best;
}

// Design stats per camp kind (one engine query per kind instead of one per camp).
const bc::camp_stats& stats_of(const bc::camp& camp) {
    static std::array<bool, 8> loaded{};
    static std::array<bc::camp_stats, 8> stats{};
    const std::size_t k = static_cast<std::size_t>(camp.kind());
    if (!loaded[k]) { stats[k] = camp.base_stats(); loaded[k] = true; }
    return stats[k];
}

// Fill design facts of one group: value, monster health / strength, which tiles it covers.
GroupInfo describe_group(const bc::map::camp_group& engine_group, int index) {
    GroupInfo g;
    g.index = index;
    g.gems = static_cast<int>(engine_group.gem_value());
    g.gold = static_cast<int>(engine_group.gold_value());
    g.respawn_full = engine_group.max_respawn_timer();
    g.spawn.clear();
    g.clusters_only = true;
    for (const bc::camp& camp : engine_group.camps()) {
        const vec2 p = camp.spawn_position();
        const bc::camp_stats& stats = stats_of(camp);
        g.tiles.push_back(p);
        g.strengths.push_back(stats.base_strength);
        g.ranges.push_back(static_cast<int>(stats.base_attack_range));
        g.spawn.set(p, true);
        g.total_health += stats.base_health;
        g.total_strength += stats.base_strength;
        g.max_strength = std::max(g.max_strength, stats.base_strength);
        g.max_attack_range = std::max(g.max_attack_range, static_cast<int>(stats.base_attack_range));
        const bool cluster = camp.kind() == bc::camp_type::gem_cluster || camp.kind() == bc::camp_type::gold_cluster;
        g.clusters_only = g.clusters_only && cluster;
        g.has_monsters = g.has_monsters || !cluster;
        g.has_monolith = g.has_monolith || camp.kind() == bc::camp_type::monolith;
    }
    if (!g.tiles.empty()) g.centre = g.tiles[0];
    g.edge_strength = best_edge_exposure(g);
    for (vec2 t : g.tiles)
        for (vec2 a : data().altars)
            if (t.chebyshev_distance(a) <= 1) g.gate = true;
    return g;
}

// Smallest distance s such that at least `share` of all reachable tiles have distance <= s.
// Example: shares 0.15 of 1,200 tiles = 180 -> the distance of the 180th closest tile.
// FUEL: this loop is the most expensive part of the draft (~60 fuel per tile), so it samples every
// 3rd tile - the threshold only needs to be roughly right.
int share_threshold(const std::vector<std::uint16_t>& field, double share) {
    std::array<int, 1024> count_at{};
    int reachable = 0;
    // v10: at most ~1,200 samples (a 100x100 map sampled every 3rd tile cost ~200k fuel per call).
    const std::size_t stride = std::max<std::size_t>(3, field.size() / 1200);
    for (std::size_t i = 0; i < field.size(); i += stride) {
        const std::uint16_t v = field[i];
        if (v == bc::distance_field::unreachable) continue;
        ++count_at[std::min<std::size_t>(v, count_at.size() - 1)];
        ++reachable;
    }
    const double needed = share * reachable;
    int cumulative = 0;
    for (std::size_t s = 0; s < count_at.size(); ++s) {
        cumulative += count_at[s];
        if (cumulative >= needed) return static_cast<int>(s);
    }
    return static_cast<int>(count_at.size());
}

// Distance to stand next to a group: the best of the 8 neighbours of each of its monster tiles.
int best_neighbour_distance(const GroupInfo& g, const std::vector<std::uint16_t>& field) {
    int best = FAR_AWAY;
    for (vec2 t : g.tiles)
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                best = std::min(best, read_distance(field, vec2{t.x + dx, t.y + dy}));
    return best;
}

int nearest_altar_index(vec2 p) {
    int best = -1;
    long best_distance = 1L << 30;
    const auto& list = data().altars;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const long d = list[i].chebyshev_distance(p);
        if (d < best_distance) best_distance = d, best = static_cast<int>(i);
    }
    return best;
}

void load_masks() {
    MapData& d = data();
    d.width = bc::map::width();
    d.height = bc::map::height();
    d.terrain_blocked = bc::map::walls() | bc::map::holes() | bc::map::altars() | bc::map::spawn_region(bc::team::enemy);
    d.wall_bits = (bc::map::walls() | bc::map::holes() | bc::map::altars()).export_linear_buffer();
    d.altars = bc::map::altars().positions();
    d.altar_ring.clear();
    for (vec2 a : d.altars) d.altar_ring.draw_chebyshev(a, 1.0f);
    d.altar_ring &= ~d.terrain_blocked;
    d.home_centre = centre_of(bc::map::spawn_region(bc::team::ally));
    d.enemy_centre = centre_of(bc::map::spawn_region(bc::team::enemy));
}

// Groups are described a few at a time: on a 42-group map this is ~200k fuel in total, more than one
// turn. Returns true once every group is loaded.
bool load_groups_step() {
    MapData& d = data();
    if (d.engine_groups.empty() && d.groups.empty()) {
        d.engine_groups = bc::map::camp_groups();   // ~11k fuel per call: cached for the whole game
        d.camp_spawns.clear();
        d.group_of_tile.assign(static_cast<std::size_t>(d.width * d.height), NO_GROUP);
    }
    while (d.groups.size() < d.engine_groups.size()) {
        if (bc::fuel_remaining() < SETUP_FUEL_RESERVE) return false;
        const std::size_t i = d.groups.size();
        GroupInfo g = describe_group(d.engine_groups[i], static_cast<int>(i));
        d.camp_spawns |= g.spawn;
        for (vec2 t : g.tiles) d.group_of_tile[static_cast<std::size_t>(tile_index(t))] = static_cast<std::int16_t>(i);
        d.any_gate = d.any_gate || g.gate;
        d.groups.push_back(std::move(g));
    }
    return true;
}

// Altar ring tiles we bank at: every altar outside the enemy zone (all altars if none is outside).
void build_safe_altar_ring() {
    MapData& d = data();
    d.safe_altar_ring.clear();
    for (vec2 a : d.altars)
        if (best_altar_neighbour_enemy(a) > d.d15) d.safe_altar_ring |= ring_of(a);
    d.safe_altar_ring &= d.altar_ring;
    if (d.safe_altar_ring.is_empty()) d.safe_altar_ring = d.altar_ring;
}

// One distance field per call (each ~1k-28k fuel). Returns true once all three exist.
// The zone thresholds are set between the enemy field and the altar field (the altar field only
// leads to altars outside the enemy zone).
bool compute_fields_step(bool have_plan_thresholds, int plan_d15, int plan_d60) {
    MapData& d = data();
    const bc::bitboard& ours = bc::map::spawn_region(bc::team::ally);
    const bc::bitboard& theirs = bc::map::spawn_region(bc::team::enemy);
    const bc::bitboard base = bc::map::walls() | bc::map::holes() | bc::map::altars() | d.camp_spawns;
    if (d.from_us.empty()) {
        if (bc::fuel_remaining() < FIELD_FUEL) return false;
        d.from_us = compute_field(base | theirs, ours);
    }
    if (d.from_enemy.empty()) {
        if (bc::fuel_remaining() < FIELD_FUEL) return false;
        d.from_enemy = compute_field(base | ours, theirs);
    }
    if (!d.thresholds_set) {
        // Two share_threshold calls (~80k each at most). The draft must never run dry, so it waits for plenty;
        // a champion may (it just resumes next round).
        if (bc::fuel_remaining() < (bc::is_draft() ? THRESHOLD_FUEL : SETUP_FUEL_RESERVE * 3)) return false;
        d.d15 = have_plan_thresholds ? plan_d15 : share_threshold(d.from_enemy, ENEMY_ZONE_SHARE);
        d.d60 = have_plan_thresholds ? plan_d60 : share_threshold(d.from_us, HOME_SHARE);
        build_safe_altar_ring();
        d.thresholds_set = true;
    }
    if (d.to_altar.empty() && !d.altars.empty()) {
        if (bc::fuel_remaining() < FIELD_FUEL) return false;
        d.to_altar = compute_field(base | theirs, d.safe_altar_ring);
    }
    // Gate maps only (an altar touches a monster spawn): a second field where spawn tiles are walkable.
    // Example (boss-battle): the only altar (25, 20) is reachable only from the monolith tile (24, 20).
    if (d.any_gate && d.to_altar_open.empty() && !d.altars.empty() && !bc::is_draft()) {   // champions only: the draft does not need it
        if (bc::fuel_remaining() < FIELD_FUEL) return false;
        const bc::bitboard open_walls = bc::map::walls() | bc::map::holes() | bc::map::altars() | theirs;
        bc::bitboard ring;
        ring.clear();
        for (vec2 a : d.altars) ring.draw_chebyshev(a, 1.0f);
        d.to_altar_open = compute_field(open_walls, ring);
    }
    int best = FAR_AWAY;
    for (vec2 p : ours.positions()) best = std::min(best, read_distance(d.to_altar, p));
    d.home_to_altar = best;
    return true;
}

void classify_groups() {
    MapData& d = data();
    for (GroupInfo& g : d.groups) {
        g.steps_from_us = best_neighbour_distance(g, d.from_us);
        g.steps_from_enemy = best_neighbour_distance(g, d.from_enemy);
        g.steps_to_altar = best_neighbour_distance(g, d.to_altar);
        g.nearest_altar = nearest_altar_index(g.centre);
        g.in_enemy_zone = g.steps_from_enemy <= d.d15;
        g.in_home = g.steps_from_us <= d.d60;
        const double total = static_cast<double>(g.steps_from_us + g.steps_from_enemy);
        const double ratio = total > 0 ? g.steps_from_us / total : 0.5;
        g.in_middle_band = ratio >= MIDDLE_BAND_LOW && ratio <= MIDDLE_BAND_HIGH;
    }
}

}  // namespace

bool build_step(bool have_plan_thresholds, int plan_d15, int plan_d60) {
    MapData& d = data();
    if (d.built) return true;
    if (d.width == 0) load_masks();
    if (!load_groups_step()) return false;
    if (!compute_fields_step(have_plan_thresholds, plan_d15, plan_d60)) return false;
    if (bc::fuel_remaining() < CLASSIFY_FUEL) return false;
    if (!have_plan_thresholds) classify_groups();   // with a plan, world.cpp copies the draft's per-group values
    d.built = true;
    return true;
}

bool is_built() { return data().built; }
void classify_groups_now() { classify_groups(); }
float exposure(const GroupInfo& g, vec2 tile) { return exposure_at(g, tile); }

double monster_gem_share() {
    static double cached = -1.0;
    if (cached >= 0.0) return cached;
    long total = 0, monster = 0;
    for (const GroupInfo& g : data().groups) {
        total += g.gems;
        if (g.has_monsters) monster += g.gems;
    }
    cached = total > 0 ? static_cast<double>(monster) / static_cast<double>(total) : 1.0;
    return cached;
}

std::vector<GroupInfo>& groups() { return data().groups; }
const std::vector<bc::map::camp_group>& engine_groups() { return data().engine_groups; }

int group_at(vec2 spawn_tile) {
    if (!on_map(spawn_tile)) return NO_GROUP;
    return data().group_of_tile[static_cast<std::size_t>(tile_index(spawn_tile))];
}

const std::vector<vec2>& altars() { return data().altars; }
const bc::bitboard& altar_ring() { return data().altar_ring; }
const bc::bitboard& safe_altar_ring() { return data().safe_altar_ring; }
int home_to_altar_steps() { return data().home_to_altar; }
const bc::bitboard& terrain_blocked() { return data().terrain_blocked; }
const bc::bitboard& all_camp_spawns() { return data().camp_spawns; }
vec2 home_centre() { return data().home_centre; }
vec2 enemy_centre() { return data().enemy_centre; }

// A monster spawn tile is a wall in the fields; a champion standing on one (capturing) reads the best
// neighbour + 1 instead, so it never looks "unreachable" from where it actually stands.
int distance_here(const std::vector<std::uint16_t>& field, vec2 p) {
    const int direct = read_distance(field, p);
    if (direct < FAR_AWAY) return direct;
    int best = FAR_AWAY;
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy)
            if (dx || dy) best = std::min(best, read_distance(field, vec2{p.x + dx, p.y + dy}));
    return best < FAR_AWAY ? best + 1 : FAR_AWAY;
}

int steps_from_us(vec2 p) { return distance_here(data().from_us, p); }
int steps_from_enemy(vec2 p) { return distance_here(data().from_enemy, p); }
int steps_to_altar(vec2 p) { return distance_here(data().to_altar, p); }
int steps_to_altar_open(vec2 p) { return data().to_altar_open.empty() ? FAR_AWAY : read_distance(data().to_altar_open, p); }
bool any_gate() { return data().any_gate; }
int d15() { return data().d15; }
int d60() { return data().d60; }

bool in_enemy_zone(vec2 p) { return steps_from_enemy(p) <= data().d15; }
bool in_home(vec2 p) { return steps_from_us(p) <= data().d60; }

bool in_middle_band(vec2 p) {
    const int us = steps_from_us(p), them = steps_from_enemy(p);
    if (us >= FAR_AWAY || them >= FAR_AWAY || us + them == 0) return false;
    const double ratio = static_cast<double>(us) / (us + them);
    return ratio >= MIDDLE_BAND_LOW && ratio <= MIDDLE_BAND_HIGH;
}

bool in_enemy_half(vec2 p) { return steps_from_enemy(p) < steps_from_us(p); }

}  // namespace mapinfo
