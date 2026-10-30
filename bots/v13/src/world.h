// world.h - who I am, what I see this turn, and what I remember. [S5]
//
// What it decides: nothing; it builds the Snapshot every other module reads.
// Worked example: Stellar (slot 2) in a 3-champion game reads the plan on her first turn:
//   type_of_slot[2] = stellar, squad_of_slot[2] = 0, role_of_slot[2] = Farmer -> she farms with Marcella.
//   Each turn sense() lists the visible enemies, allies and camps, updates group respawn timers
//   ("camp_group_respawning{timer = 31}" in round 100 -> back in round 131), and writes a sighting
//   to shared memory if an enemy is within 8 tiles.
#pragma once
#include <array>
#include <vector>
#include "comms.h"
#include "types.h"

namespace world {

bool setup_step();            // first turn(s): plan, map info, my role, priority. True when finished
bool is_setup();
Snapshot sense();             // every turn, once

const comms::Plan& plan();
const std::array<EnemyMemory, 13>& enemy_memory();   // indexed by champion_type value
bool tile_visible(vec2 p);    // in my vision this turn (plain-memory copy: cheap)

// Teammates (from the plan; works without vision).
int squad_of(bc::champion_type type);
Role role_of(bc::champion_type type);
bool ally_alive(bc::champion_type type);
std::vector<bc::champion_type> squad_members(int squad, bool alive_only);   // includes me
bool team_has(bc::champion_type type);
int alive_enemy_count();
int team_deaths_since(int since);   // v12: our deaths in rounds >= since (harassment detection)

}  // namespace world
