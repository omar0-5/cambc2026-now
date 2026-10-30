// comms.h - the two team scratchpads and their exact byte layouts. [S10]
//
// What it decides: nothing; it moves facts between our bot instances (no shared vision!).
// Worked example: Stellar sees two enemies at (20, 14) in round 133: emergency type 2 (2+ enemies),
//   x = 20, y = 14, stamp = 133 mod 8 = 5. In round 135 Marcella reads age = (135 - 5) mod 8 = 2 <= 3
//   -> fresh, and reacts. (Probe, Oct 21: a write is visible to teammates later in the SAME round.)
//
// SHARED MEMORY, one 64-bit word (design step b; bit 0 = lowest bit of byte 0). Read once at turn start;
// at turn end the fields this champion set are merged into a fresh read and all 8 bytes written back.
//   bits   size field                       values
//   0-6    7    claim: squad 0 target         camp-group index + 1 (groups 0-126), 0 = none
//   7-13   7    claim: squad 1 target         same
//   14-20  7    claim: split target           second farmer's own cluster group; same
//   21-23  3    role: scout                   body index + 1 (bodies 0-5), 0 = none
//   24-26  3    role: scorer                  same
//   (+1 so the all-zero memory a game starts with means "none"; the doc's 127 / 7 = none read a zeroed
//   word as "every claim is group 0".)
//   27-28  2    objective type                0 none, 1 bank at altar, 2 priority camp, 3 regroup point
//   29-35  7    objective x                   0-99
//   36-42  7    objective y                   0-99
//   43-44  2    objective stamp               round mod 4 (valid at age <= 1)
//   45-46  2    emergency type                0 none, 1 danger (1 enemy), 2 danger (2+), 3 help
//   47-53  7    emergency x
//   54-60  7    emergency y
//   61-63  3    emergency stamp               round mod 8 (fresh at age <= 3)
// Claims carry no stamp: the owner re-asserts every turn. Two claims naming one group: the lower field
// keeps it (squad 0 < squad 1 < split). An emergency replaces the current one only if its type is
// higher or the current one is stale. Bad values (off the map, past the groups or bodies) are ignored.
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

// Shared memory, design step b. begin_turn / end_turn bracket every champion turn (main.cpp).
void begin_turn();   // read the 64-bit word once
void end_turn();     // merge my fields into a fresh read and write all 8 bytes back

// Claims (squads 0 and 1, and the split). NO_GROUP if none or invalid.
int squad_target(int squad);
void set_squad_target(int squad, int group);
int split_target();
void set_split_target(int group);

// Roles on top of the draft defaults: body indices, -1 if none / invalid. Written by the leader.
int scout_body();
int scorer_body();
void set_roles(int scout, int scorer);

// Objective (leader): valid when age <= OBJECTIVE_VALID_AGE.
enum class ObjectiveType : std::uint8_t { None = 0, BankAtAltar = 1, PriorityCamp = 2, Regroup = 3 };
struct Objective { bool valid = false; ObjectiveType type = ObjectiveType::None; vec2 pos{}; };
Objective read_objective();
void write_objective(ObjectiveType type, vec2 pos);

// Emergency: fresh when age <= EMERGENCY_FRESH_AGE.
enum class EmergencyType : std::uint8_t { None = 0, DangerOne = 1, DangerMany = 2, Help = 3 };
struct Emergency { bool fresh = false; EmergencyType type = EmergencyType::None; vec2 pos{}; int age = 99; };
Emergency read_emergency();
void raise_emergency(EmergencyType type, vec2 pos);   // applies the replace rule

// Body index: a body's position among our bodies ordered by champion type value (Cassi before Issac,
// then lower unit id on a tie); -1 if not ours. body_type() is the reverse.
int body_index(bc::champion_type type);
int body_count();
bc::champion_type body_type(int index);

}  // namespace comms
