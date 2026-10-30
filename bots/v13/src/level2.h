// level2.h - Q(a): the score of one candidate plan inside the chosen mode (design step d).
// [Phase 1 doc, "Level 2" and "Q(a): estimating each term"]
//
//   Q(a) = dS + LAMBDA_E * dE - LAMBDA_D * P(death | a) * L_D - LAMBDA_T * C_time
//   r      = 0.3 before round 100, else max(0.15, banked / (max(100, round) * bodies))  gems per body-round
//   L_D    = (bag + 10) * e + bag + 60 * r      (e = share of the death risk from enemies; a killer takes
//                                               the bag and 100 gold = 10 gems)
//   C_time = T_a * r * (bodies in the plan: 1, or the squad for a squad farming plan)
// Worked example: round 600, banked 300, 5 bodies -> r = max(0.15, 300 / 3000) = 0.15. Ryth with a 40-gem
//   bag weighs walking 20 rounds to a 60-gem group with P(death) = 0.1, all of it from enemies (e = 1):
//   L_D = 50 + 40 + 9 = 99; Q = 0.9 * 60 - 0.1 * 99 - 20 * 0.15 = 54 - 9.9 - 3 = 41.1.
#pragma once
#include "config.h"
#include "types.h"

// The mode whose candidates are being scored (for the LOG_PDEATH lines). A rule is labelled before it
// runs, because the ladder enters a mode only after the rule acted. Compiles to nothing when LEVEL2 is 0.
#if LEVEL2
namespace level2 { inline const char* g_label = "-"; }
#define LEVEL2_LABEL(x) (level2::g_label = (x))
#else
#define LEVEL2_LABEL(x) ((void)0)
#endif

namespace level2 {

struct Plan {
    double dS = 0;        // gems scored by the plan (CARRY: the bag)
    double dE = 0;        // expected gems (or gold / 10) gained
    double p_death = 0;   // P(death | a)
    double enemy_share = 0;
    double T = 1;         // rounds the plan takes
    int bodies = 1;       // bodies whose time it costs
};

bool active();                                   // switch on and fuel above LEVEL2_FUEL_FLOOR at Level 2's start
double income_rate(const Snapshot& s);           // r
double loss_on_death(const Snapshot& s, double enemy_share);
// v13's P_die over T rounds, with enemy contact measured to me AND to dest, plus stale danger records
// (belief threats not in sight) weighted 0.85^age. Writes the enemy share of the risk.
double p_death(const Snapshot& s, double T, vec2 dest, double* enemy_share);
double q(const Snapshot& s, const Plan& p);

}  // namespace level2
