// belief.h - each champion's own four-layer picture of the map (design step a). [Phase 1 doc, "Belief map"]
//
// What it decides: nothing; it remembers. Terrain and camp positions are known from turn 1, so the
// layers track state: which tiles were seen lately (known safe), what camps and altars hold (known
// valuable), how long each group has gone unseen (unknown), and where enemies may be (potentially
// dangerous). Readers arrive in steps b-d (EXPLORE, P(death), RETREAT / CARRY avoid tiles, emergency).
// Memory: every board and array is built once at the first update and changed in place afterwards; the
// per-turn code never constructs a bitboard (the engine's visible_tiles() is the one exception, in world).
// Worked example: an enemy Stellar seen at (20, 14) in round 300 with reach 4 is a threat record; in
//   round 303 it is drawn on the danger board as a square of radius min(4 + 3, 8) = 7, weighs
//   0.85^3 = 0.61 in P(death), and is dropped after round 315 unless seen again.
#pragma once
#include <array>
#include "types.h"

namespace belief {

void update(const Snapshot& s, const bc::bitboard& visible);   // once per turn, from world::sense

// Known safe: seen within the last 8 rounds and not on the danger board.
const bc::bitboard& safe();
// Known valuable / unknown.
bool group_stale(const GroupInfo& g, int round);   // Alive (or never seen) and unseen for STALE_ROUNDS+
int rounds_unseen(const GroupInfo& g, int round);  // round - last seen (round + 1 if never seen)
struct AltarNote { int last_seen = -1; bool enemy_lock = false; };
const AltarNote& altar_note(int altar_index);      // index into mapinfo::altars()
// Potentially dangerous.
enum class ThreatSource : std::uint8_t { OwnSight, Team };
struct Threat {
    bool used = false;
    vec2 pos{};
    int round = -1000;          // last seen
    int count = 1;              // enemies at that spot (team sightings carry a count)
    ThreatSource source = ThreatSource::OwnSight;
    int reach = 4;              // tiles it threatens when seen (attack or spell range)
    int key = -1;               // champion_type value for own sightings, -1 for the team record
};
const std::array<Threat, 8>& threats();
const bc::bitboard& danger();
double threat_weight(const Threat& t, int round);   // 0.85^age, 0 once dropped

}  // namespace belief
