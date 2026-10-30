// farm.h - which camp group to farm, how to stand around monsters, and capturing. [S4, S5]
//
// What it decides: the target group of each squad and every farming step (walk, hold, fight, capture).
// Worked example (group value): 2 gem clusters (14 gems), 12 steps away, our squad kills both in
//   1 round, no monster damage:  value = 14 / (12 walk + 0 wait + 1 kill + 5 capture + 0 healing)
//   = 0.78 gems per round. A wolf (4 gems, 40 gold) 6 steps away that takes 12 rounds to kill:
//   (4 + 0.1 x 40) / (6 + 12 + 5 + healing) < 0.35 -> the clusters win.
#pragma once
#include <vector>
#include "types.h"

namespace farm {

struct SquadStats {
    std::vector<bc::champion_type> members;   // alive members
    double dps = 0, spell_dps = 0;
    bc::champion_type tank = bc::champion_type::marcella;
    double tank_health = 1000, tank_max_health = 1000;
    bool has_melee = false;
    int melee_count = 0;                      // melee members split the monsters' attention
    bool tank_has_jungle = false;             // my own items, or Marcella's shared-memory flag
};
SquadStats squad_stats(const Snapshot& s, int squad);

// value = (gems + LAMBDA * gold) / (walk + respawn_wait + kill_time + CAPTURE_ROUNDS + healing_cost)
double group_value(const GroupInfo& g, const SquadStats& squad, const Snapshot& s);
bool group_feasible(const GroupInfo& g, const SquadStats& squad);   // tank damage <= share x tank health
int choose_target(const Snapshot& s, int squad);                    // skips claimed groups, zone rule
bool harassment_merge();                                            // step b: v12 merge condition (leader / fallback)
bool squad1_should_join(const Snapshot& s);                         // step b: v13 join check, cached (leader)

// Ladder step 9: we stand on a cleared group's spawn tile -> stay until the reward is ours.
bool finish_capture(Snapshot& s);
void note_gold(int gold_now);       // v13: lifetime gold earned (zero-gold capture rule)
// Ladder step 10 for every role: farm this champion's target (or wait / idle). True = turn used.
bool farm_step(Snapshot& s);
// The group this champion is working on this turn (NO_GROUP if none); for the ward and the log.
int current_group();
void avoid_current(int until_round);
void avoid_group(int group, int until_round);   // skip this group until that round (watchdog)   // skip the group I work on until that round (walked away from it)

}  // namespace farm
