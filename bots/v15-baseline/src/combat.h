// combat.h - the damage model, the kill race, and what to do about visible enemies. [S6]
//
// What it decides: fight, interrupt a channeler, walk away, or ignore the enemies we see.
// Worked example (kill race): Heart of Ruin alone vs a full-health enemy Stellar.
//   Our damage per round = 30 basic + 150 dash / 8 = 48.8; our burst now = 150 dash + 30 basic = 180.
//   rounds_to_kill(Stellar) = 1 + (650 - 180) / 48.8 = 10.6 rounds.
//   Her damage per round on us = 30 + (140 + 98) / 8 = 59.8, burst 140 + 30 = 170.
//   rounds_to_kill(us) = 1 + (1000 - 170) / 59.8 = 14.9 rounds.
//   p_win = 1 / (1 + exp(-(14.9 - 10.6 + 0.5) / 1.0)) = 0.99 -> fight, IF she cannot be helped first
//   (eta_help: her nearest teammate must need more than 10.6 rounds to arrive).
#pragma once
#include <vector>
#include "types.h"

namespace combat {

// Damage one champion deals per round to one target: basic every round + each damaging spell once
// per CAST_PERIOD. Example: Stellar 30 + (140 + 98) / 8 = 59.8.  [S2]
double damage_per_round(bc::champion_type type, float strength, float focus);
// Damage a champion can deal THIS round: one spell (the biggest) + one basic attack.
double burst(bc::champion_type type, float strength, float focus);
double my_burst_now(const Snapshot& s);    // uses the real cooldowns of my slots (cached per round)
// Design stats of a champion type (cached: building a design champion costs engine calls).
const bc::champion_stats& base_stats_of(bc::champion_type type);

// rounds_to_kill = 1 + max(0, ehp - burst) / dps + stuns the defenders can land on the attackers.
double rounds_to_kill(double ehp, double burst_damage, double dps, int defender_stuns);
// Logistic chance that "we" win a race (RACE_SOFTNESS sets how sharp). Acting first is worth 0.5 round.
double p_win(double their_rounds, double our_rounds, bool we_act_now);

enum class FightChoice { Fight, Interrupt, WalkAway, Ignore };
struct FightPlan {
    FightChoice choice = FightChoice::Ignore;
    int target = -1;                 // index into s.enemies
    double chance = 0, our_rounds = 0;
    std::vector<vec2> threats;       // enemies to walk away from
};
// Fight if catchable && p_win >= PWIN_FIGHT && rounds_to_kill <= eta_help.  [S6]
// Else interrupt a channeler if we survive one round there; else walk away if a winning enemy can reach us.
FightPlan decide_fight(const Snapshot& s);

// Ladder step 6: act on decide_fight (or answer a teammate's call for help). True = turn used.
bool handle_enemies(Snapshot& s);

// Damage per round we would take standing here: awake monsters that target us + enemies in reach.
double incoming_damage(const Snapshot& s);

}  // namespace combat
