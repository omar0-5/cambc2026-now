// mapinfo.h - static facts about the map: walking distances, zones, camp groups, altars. [S4]
//
// What it decides: which tiles count as "our home", "the middle band" and "the enemy zone".
// Computed once (draft: for the plan; each champion: on its first turn, which has 2x fuel).
// Worked example: 1,200 reachable tiles, ENEMY_ZONE_SHARE = 15% -> 180 tiles. If the 180th-closest
// tile to the enemy spawn is 9 steps away, d15 = 9 and the enemy zone = every tile with
// steps_from_enemy <= 9. With HOME_SHARE = 60% the home region is the 720 tiles nearest our spawn.
// After setup every query below is O(1): a lookup in a plain array, no engine call.
#pragma once
#include <vector>
#include "types.h"

namespace mapinfo {

// Setup, a piece at a time: returns true when finished, false when it stopped to save fuel (call it
// again next turn). `have_plan_thresholds`: the draft already computed d15 / d60 (draft memory).
// FUEL: ~150k-400k in total on big maps; a champion turn has 100k (200k on its first turn).
bool build_step(bool have_plan_thresholds, int plan_d15, int plan_d60);
bool is_built();
void classify_groups_now();   // per-group distances and regions (build() skips it when a plan exists)

// Camp groups, in bc::map::camp_groups() order. Champions update the live fields (world.cpp).
std::vector<GroupInfo>& groups();
const std::vector<bc::map::camp_group>& engine_groups();
int group_at(vec2 spawn_tile);           // group owning this monster spawn tile, or NO_GROUP

// Damage per round a champion standing on `tile` takes if every monster of `g` is awake and nothing
// else is nearer to them (each monster hits within its range). Example: an elk pack where 3 elks reach
// a corner tile -> 3 x 60 = 180.
float exposure(const GroupInfo& g, vec2 tile);

// v11: share of the map's gems held by groups with real monsters (wolves/elks/golems/monoliths), 0..1.
// Cluster-heavy maps (< CAPTURE_FIRST_MONSTER_SHARE) buy the Capture emblem first: fast capture loops
// pay it back sooner than the tank's Jungle (OPTIMAL_STRATEGY.md §12).
double monster_gem_share();

// Altars.
const std::vector<vec2>& altars();
const bc::bitboard& altar_ring();        // walkable tiles within 1 of an altar (where we tribute)
const bc::bitboard& safe_altar_ring();   // the same, only altars outside the enemy zone (if any)
// steps_to_altar() below measures the walk to safe_altar_ring().
int home_to_altar_steps();               // steps from our spawn to the nearest altar ring

// Terrain.
const bc::bitboard& terrain_blocked();   // walls | holes | altars | enemy spawn (never walkable)
const bc::bitboard& all_camp_spawns();   // every monster spawn tile on the map
vec2 home_centre();
vec2 enemy_centre();

// Distances (walking steps; FAR_AWAY when unreachable) and regions.
int steps_from_us(vec2 p);
int steps_from_enemy(vec2 p);
int steps_to_altar(vec2 p);              // to the nearest altar ring tile
int steps_to_altar_open(vec2 p);         // same, walking over monster spawn tiles (FAR_AWAY unless a gate map)
bool any_gate();                         // some altar touches a monster spawn tile
int d15();                               // enemy-zone threshold
int d60();                               // home-region threshold
bool in_enemy_zone(vec2 p);              // steps_from_enemy(p) <= d15
bool in_home(vec2 p);                    // steps_from_us(p) <= d60
bool in_middle_band(vec2 p);             // d_us / (d_us + d_enemy) in [LOW, HIGH]
bool in_enemy_half(vec2 p);              // nearer the enemy spawn than ours

}  // namespace mapinfo
