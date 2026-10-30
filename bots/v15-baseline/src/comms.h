// comms.h - the two team scratchpads and their exact byte layouts. [S10]
//
// What it decides: nothing; it moves facts between our bot instances (no shared vision!).
// Worked example: Stellar sees two enemies at (20, 14) in round 133, one of them tributing.
//   byte 4 = 20, byte 5 = 14, byte 6 = 2 | (1 << 3) | ((133 & 15) << 4) = 2 + 8 + 80 = 90.
//   In round 135 Marcella reads age = (135 - 5) & 15 = 2 <= SIGHTING_FRESH -> fresh, and reacts.
//   (Probe, Oct 21: a write is visible to teammates later in the SAME round.)
//
// SHARED MEMORY, 8 bytes (bc::shared_memory[i]; read-modify-write only your own bits)
//   byte  bits  meaning
//   0     0-7   squad 0 target group + 1 (0 none); Marcella's own cluster target when split
//   1     0-7   second farmer's cluster target + 1 while split (0 = not split)
//   2     0-7   squad 1 target + 1
//   3     0-7   squad 2 target + 1
//   4     0-7   sighting x (255 none)
//   5     0-7   sighting y
//   6     0-2   enemies seen there (0-7)
//   6     3     an enemy there is tributing or recalling
//   6     4-7   round & 15 of the sighting
//   7     0-3   focus: enemy champion_type + 1 (0 none); uses byte 6's round stamp
//   7     4     Marcella inside home region
//   7     5     second farmer inside home region
//   7     6     help wanted at the sighting
//   7     7     Marcella owns a Jungle emblem (-30% damage from camps: bigger groups become safe)
// Conflict rule: if two bodies claim the same group, the lower body slot keeps it.
//
// DRAFT MEMORY, 1,024 bytes (written in the draft, read-only for champions)
//   offset  size  meaning                                        (v11: 13 body slots -> version 2)
//   0       1     magic 0xD5
//   1       1     version 2
//   2       1     team size
//   3       1     body count
//   4       13    champion_type per body slot (0xFF unused)
//   17      13    squad per slot
//   30      13    Role per slot
//   43      2     d15 (u16, little-endian)
//   45      2     d60 (u16)
//   47      1     G groups (cap 117: 49 + 2*16 altars + 8*117 = 1,017 bytes)
//   48      1     A altars (cap 16)
//   49      2A    altar x, y
//   49+2A   8G    per group: u16 steps_from_us, u16 steps_from_enemy, u8 nearest_altar,
//                 u16 steps_to_altar, u8 flags
//   flags: bit0 enemy zone, bit1 home, bit2 middle band, bit3 clusters only,
//          bit4 wolves/elks/golems, bit5 monolith
// If the magic is wrong, each champion computes everything itself on its first turn.
#pragma once
#include <array>
#include <cstdint>
#include <vector>
#include "types.h"

namespace comms {

constexpr std::uint8_t PLAN_MAGIC = 0xD5;
constexpr std::uint8_t PLAN_VERSION = 2;
constexpr int MAX_PLAN_GROUPS = 117;
constexpr int MAX_PLAN_ALTARS = 16;
constexpr std::uint8_t UNUSED_SLOT = 0xFF;

struct GroupPlan {
    std::uint16_t steps_from_us = NO_DISTANCE, steps_from_enemy = NO_DISTANCE;
    std::uint8_t nearest_altar = 0xFF;
    std::uint16_t steps_to_altar = NO_DISTANCE;
    std::uint8_t flags = 0;
};

struct Plan {
    bool valid = false;
    int team_size = 0, body_count = 0;
    std::array<std::uint8_t, BODY_SLOTS> type_of_slot{};   // champion_type value, UNUSED_SLOT if empty
    std::array<std::uint8_t, BODY_SLOTS> squad_of_slot{};
    std::array<std::uint8_t, BODY_SLOTS> role_of_slot{};
    int d15 = 0, d60 = 0;
    std::vector<vec2> altars;
    std::vector<GroupPlan> groups;
};

// Draft memory.
void write_plan(const Plan& plan);   // draft only
Plan read_plan();                    // champions; .valid == false if the magic or version is wrong
Plan plan_from_mapinfo(int team_size);   // draft: fills distances and flags from mapinfo

// Shared memory: squad targets (squad 0 -> byte 0, squad 1 -> byte 2, squad 2 -> byte 3).
int squad_target(int squad);                  // NO_GROUP if none
void set_squad_target(int squad, int group);
int split_target();                           // byte 1: second farmer's own cluster target
void set_split_target(int group);

// Shared memory: enemy sighting (bytes 4-6) and focus / flags (byte 7).
struct Sighting {
    bool fresh = false;      // seen within SIGHTING_FRESH rounds
    vec2 pos{};
    int count = 0;
    bool channeling = false;
    int age = 99;
};
Sighting read_sighting();
void write_sighting(vec2 pos, int count, bool channeling);
int focus_type();                              // enemy champion_type value, or -1 (none / stale)
void set_focus(bc::champion_type enemy);
bool flag(int bit);                            // byte 7 bits 4-6
void set_flag(int bit, bool value);
constexpr int FLAG_MARCELLA_HOME = 4;
constexpr int FLAG_PARTNER_HOME = 5;
constexpr int FLAG_HELP_WANTED = 6;
constexpr int FLAG_MARCELLA_JUNGLE = 7;

}  // namespace comms
