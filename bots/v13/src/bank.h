// bank.h - when to carry gems to an altar, and doing it. [S7]
//
// What it decides: keep farming, walk to an altar now, or recall home first and walk from there.
// Worked example: Marcella, 160 gems, 75% health, altar 14 steps away, no enemy known nearby:
//   T_bank = 14 + ceil(160 / 10) = 30 rounds,  P_die = 1 - exp(-0.002 x 1.333 x 30) = 7.7%
//   bag >= 150 but P_die <= 25% -> Keep. If an enemy that beats her shows up 6 tiles away, P_die jumps
//   above 25% -> GoTribute; above 60% with a safe recall -> RecallThenTribute.
#pragma once
#include "types.h"

namespace bank {

// P_die = 1 - exp(-H0 * (max_hp / hp) * T_bank) * prod_j (1 - p_contact_j * p_lose_j) * S_mon
double p_die(const Snapshot& s, int t_bank_rounds);
int tribute_turns(int bag);                  // max(1, ceil(bag / 10))
int bank_rounds(const Snapshot& s);          // steps to the nearest altar + tribute turns

enum class BankChoice { Keep, GoTribute, RecallThenTribute };
BankChoice bank_choice(const Snapshot& s);

int team_bag(const Snapshot& s);             // my bag + the bags of allies in sight
bool endgame_or_win(const Snapshot& s);      // this bag wins, or the game ends soon: bank now
bool go_tribute(Snapshot& s);                // walk to the best altar and start the tribute; false = could not act
int reach_steps(const Snapshot& s);          // steps to an altar we can use now (FAR_AWAY if none, gate maps included)
bool tribute_tile_ok(const Snapshot& s, vec2 t);   // step 0b: guarded-map ring tile reachable and out of awake camps' reach
bool handle(Snapshot& s);                    // ladder step 7: act on bank_choice. True = turn used
bool recall_is_safe(const Snapshot& s);      // no enemy can reach us during the recall, no monster on us

}  // namespace bank
