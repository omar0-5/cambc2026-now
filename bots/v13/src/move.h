// move.h - stepping toward goals, fleeing, and the "hidden unit blocked me" retry. [S5]
//
// What it decides: WHICH tile to step to, once another module decided WHERE to go.
// Worked example: Stellar wants the attack ring around a wolf (tiles at distance 3). step_toward()
//   expands a distance field from those ring tiles until it reaches her, then tries every equally
//   short first step. If move() returns false (an enemy hidden in a bush stood there), nothing was
//   spent, so she tries the next candidate tile.
// Rules kept here: never walk onto a respawning group's spawn tile (it delays the respawn),
//   a ranged champion that moved cannot basic-attack this turn (callers decide cast -> attack -> move).
#pragma once
#include <vector>
#include "types.h"

namespace move {

// Tiles we cannot step on right now: terrain, enemy spawn, visible units, teleport reservations,
// and spawn tiles of dead groups (unless `allow_dead_spawns`, used when capturing).
bc::bitboard blocked_now(const Snapshot& s, bool allow_dead_spawns);

// One step (plus a dash on long safe trips) toward the nearest tile of `goals`. True if we moved.
// `avoid` tiles are treated as walls when a path around them exists.
bool step_toward(Snapshot& s, const bc::bitboard& goals, const bc::bitboard* avoid = nullptr);

// Long trips: same as step_toward, but the distance field to `goals` is computed ONCE (static walls:
// terrain + every monster tile) and cached under `key`, so each later step costs ~2k fuel instead of
// up to ~28k on a 100x100 map. Within NEAR_TRIP tiles it hands over to step_toward (exact, live).
// Keys: 1000 + group index (walk to a group), 2000 (idle area). Use the same key for the same goals.
bool travel_toward(Snapshot& s, int key, const bc::bitboard& goals);
// Long trip to the nearest altar ring, using mapinfo's precomputed altar distance map.
bool travel_to_altar(Snapshot& s, const bc::bitboard& goals);
// One cheap step downhill on mapinfo's cached altar field (no new distance field); used while the idle area is unbuilt.
// shortcut: that field leads to the nearest safe altar, which can be a home-half one; cache a mid/enemy-half altar field in setup if that matters.
bool step_toward_safe_altar(Snapshot& s);

// Walking steps from us to the nearest goal tile, FAR_AWAY if none (one distance field).
int path_steps(const Snapshot& s, const bc::bitboard& goals);

// Step to the neighbour that is farthest from every threat (ties: toward home). True if we moved.
bool step_away(Snapshot& s, const std::vector<vec2>& threats);

// Mask of the tiles within Chebyshev `radius` of p (radius 0 = just p).
bc::bitboard square(vec2 p, int radius);
// Tiles at exactly Chebyshev distance `radius` from p (a ring).
bc::bitboard ring(vec2 p, int radius);

}  // namespace move
