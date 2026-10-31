// config.h - every tunable number of the Interceptor strategy, in one place.
//
// What it decides: nothing by itself; every module reads its thresholds from here.
// Strategy sections: [S4] map regions, [S5] survival, [S6] fighting, [S7] banking, [S8] shopping,
// [S9] Cassi & Issac, [S11] fuel. Change ONE number at a time and re-run the bench (docs/HOW_TO_RUN.md).
//
// Worked example (banking, [S7]): Marcella carries 160 gems at 75% health, 14 steps from an altar.
//   T_bank = 14 steps + ceil(160 / 10) = 16 tribute turns = 30 rounds.
//   P_die  = 1 - exp(-H0 * (1 / 0.75) * 30) = 1 - exp(-0.002 * 1.333 * 30) = 7.7%
//   7.7% <= FARM_KEEP_PDIE (25%)  ->  she keeps farming.
#pragma once
#include <cstdint>

// ---- Map regions [S4] ----------------------------------------------------------------------
constexpr double ENEMY_ZONE_SHARE = 0.15;  // share of reachable tiles nearest the enemy spawn
constexpr double HOME_SHARE = 0.60;        // share of reachable tiles nearest our spawn
constexpr double MIDDLE_BAND_LOW = 0.35;   // interceptor area: d_us / (d_us + d_enemy) in [low, high]
constexpr double MIDDLE_BAND_HIGH = 0.65;
// Enemy-zone exception (Omar's rule): a champion may enter the enemy zone only if all three hold.
constexpr int ZONE_ENTRY_RANGE = 10;        // steps from the champion to its target
constexpr double ZONE_ENTRY_HEALTH = 0.70;  // "high health" = health / max_health
constexpr int ZONE_ENTRY_BAG = 70;          // largest bag (carried gems) allowed to enter
// Gap 3c variant: scale v13's P_die (bank_choice, zone rule) toward calibrated values (Oct 30: actual deaths ~0.16x
// predicted). 1.0 = v18.
constexpr double P_DIE_SCALE = 1.0;
constexpr double ZONE_FARM_MAX_PDIE = 0.50; // farm inside the zone if P_die over the trip is below this

// ---- Group scoring [S4] --------------------------------------------------------------------
constexpr double LAMBDA_GEMS_PER_GOLD = 0.10;  // 1 gold is worth 0.1 gem when ranking groups
constexpr int VESPER_CAPTURE_RANGE = 3;       // v13: Vesper within this of a gold group captures it
constexpr int VESPER_GOLD_RATIO = 6;          // v13: spec §10.4 - Vesper captures iff gold > 6 x gems
constexpr int VESPER_YIELD_ROUNDS = 3;        // v13: others wait at most this long for her
constexpr bool ZERO_GOLD_JOIN = false;          // v13: measured -5 pts vs v12 (t1c vs t1d): off
constexpr int ZERO_GOLD_JOIN_ROUND = 100;     // v13: from here a champion at 0 gold joins gold captures
constexpr double TANK_FEASIBLE_SHARE = 0.80;   // tank damage on one group must be <= this x its health...
// ...OR (multi-trip) the tank can still walk out of range alive when it retreats: asleep monsters do
// NOT heal (probe, Oct 21: a wolf hit to 1,170 stayed at 1,170 after 60 rounds asleep), so a big group
// can be killed over several trips, paying healing_cost for each trip home.
constexpr bool ALLOW_MULTI_TRIP = true;
constexpr int MAX_MONSTERS_HITTING = 2;        // we fight a group from its edge: at most this many hit us
constexpr int CAPTURE_ROUNDS = 5;              // rounds standing on a cleared group to get its reward
constexpr int RECALL_TURNS = 5;                // 3 with a Home emblem
constexpr int CAST_PERIOD = 8;                 // rounds between two casts of one slot (probe: measured 8)
constexpr int RESPAWN_HOLD = 15;               // keep a target that is back within this many rounds
constexpr double GOLD_ONLY_FACTOR = 0.5;
constexpr bool TEAM_ENDGAME = true;     // everyone banks when banked + all bags >= WIN_GEMS
constexpr int TEAM_ENDGAME_SLACK = 0;
constexpr int STUN_WAIT_TURNS = 3;
constexpr int AVOID_ROUNDS = 15;
constexpr int AVOID_STREAK = 3;       // walk-aways in a row (each within 4 rounds) before a group is avoided      // after walking away from a threat, skip that group this long   // max turns to wait next to an altar for a stun threat to pass
constexpr bool SIDE_JOBS = true;               // v9: take a solo group nearby instead of waiting
constexpr int SIDE_JOB_RANGE = 12;
// Gap 1 (b): keep a side job until done unless the main target is workable within SIDE_JOB_MAIN_CLOSE tiles; take only
// side jobs whose detour to the main target is <= SIDE_JOB_MAX_DETOUR tiles. false = v18.
constexpr bool SIDE_JOB_HYSTERESIS = true;
constexpr int SIDE_JOB_MAIN_CLOSE = 3;
constexpr int SIDE_JOB_MAX_DETOUR = 99;   // the 2-tile detour filter cost as much as the hysteresis saved (farm test 4.20 vs 5.04)
// Gap 1 (c): wait at the nearest feasible group when it respawns within (walk + kill time) of the chosen one. false = v18.
constexpr bool RESPAWN_WAIT_TARGET = false;
// Test 2 (health discipline, Oct 31): retreat below DISCIPLINE_RETREAT_HEALTH within DISCIPLINE_WINDOW rounds of a camp
// or an enemy nearby; no champion fight below FIGHT_MIN_HEALTH unless p_win >= FIGHT_LOW_HP_PWIN. false = v19.
constexpr bool HEALTH_DISCIPLINE = false;
// Test 1b: Heart of Ruin shields whenever off cooldown and damage is likely (awake camp / enemy within HOR_SHIELD_ENEMY_RANGE /
// already hurt); HOR_FOCUS_BUILD: his shards are all Focus. false = v19.
constexpr bool HOR_SHIELD_EAGER = false;
constexpr int HOR_SHIELD_ENEMY_RANGE = 6;
constexpr bool HOR_FOCUS_BUILD = false;
constexpr double DISCIPLINE_RETREAT_HEALTH = 0.50;
constexpr int DISCIPLINE_WINDOW = 10;
constexpr double FIGHT_MIN_HEALTH = 0.40;
constexpr double FIGHT_LOW_HP_PWIN = 0.85;
// Gap 1 (a): target value uses walking steps (per-group fields built in leftover fuel at turn end) instead of
// 1.2 x straight-line distance. false = v18.
#define WALK_TARGETING 0   // preprocessor switch: 0 compiles exactly v18 (a constexpr flag still changed the wasm)
constexpr bool WALK_DISTANCE_TARGETING = WALK_TARGETING;
constexpr std::uint64_t WALK_FIELD_FIRST_GUARD = 35000;   // before the first field is measured (one field ~1k-28k)
constexpr std::uint64_t WALK_FIELD_MARGIN = 15000;        // periodic-job margin over the largest field measured             // ...within this many tiles       // gold-only groups are worth half (gems win the game)

// ---- Fighting monsters ----------------------------------------------------------------------
constexpr int RANGED_CAMP_DISTANCE = 3;  // ranged champions shoot monsters from this distance
constexpr int MONOLITH_SAFE_DISTANCE = 6;// never stand closer than this to a monolith (its range is 5)

// ---- Survival [S5] -------------------------------------------------------------------------
constexpr double RETREAT_HEALTH = 0.30;      // below this share of max health: stop and go home
constexpr double RETREAT_ROUNDS = 3.0;       // ...or when the damage we take kills us within 3 rounds
constexpr int RECALL_SAFE_ENEMY = 5;         // no enemy within this many tiles when we start a recall
constexpr double SPAWN_STAY_HEALTH = 0.95;   // on our spawn below this: stay one turn to heal

// ---- v11: Capture emblem, capture contests, denial locks (OPTIMAL_STRATEGY.md §10.4, §14) ----
constexpr bool CAPTURE_EMBLEMS = true;    // one buyer per farming squad (the squad's lowest body slot)
constexpr int CAPTURE_EMBLEM_ROUNDS = 3;  // engine capture time with a Capture emblem holder present
// Buy Capture before Jungle only when most gems come from clusters/wolves (fast loops pay the emblem
// back sooner); on elk/golem maps the tank's Jungle still comes first.
constexpr double CAPTURE_FIRST_MONSTER_SHARE = 0.5;
constexpr int CONTEST_MIN_REWARD = 8;     // gems + 0.1*gold needed to walk over and reset an enemy capture
constexpr int CONTEST_MAX_BAG = 30;       // ...but never with a bag this big in hand
constexpr double CONTEST_MIN_HEALTH = 0.5;
constexpr int LOCK_BANK_PRESSURE = 700;   // their banked gems >= this: lock any enemy-half altar we pass
constexpr int LOCK_CARRIER_RANGE = 8;     // ...or lock when their carrier (bag >= LOCK_ENEMY_BAG) is this close

// ---- v11: Support (Willow) -------------------------------------------------------------------
constexpr int HEAL_MIN_MISSING = 60;      // heal an ally missing at least this much health
constexpr int SUPPORT_FOLLOW_RANGE = 5;   // stay within this of the nearest squad member

// ---- v12: honest aggression (loss postmortem Oct 26, match 8d0f4189: 21 deaths, 31k damage taken
// vs 8k dealt while walking away from winnable 2-v-1s; every unseen enemy counted as 7 rounds away) --
constexpr double OUTNUMBER_PWIN = 0.45;   // commit when we outnumber them locally by 1+ fighters
constexpr int UNSEEN_HELPER_CAP = 2;      // at most this many unseen enemies delay a fight decision
constexpr double HEAL_DPS = 52.5;         // a nearby enemy Willow undoes this much of our dps (420 / 8)
constexpr double ALTAR_GUARD_PWIN = 0.45; // fight at our banking altars even below the normal gate
constexpr int ALTAR_GUARD_RANGE = 8;      // ...within this many steps of one of our bank altars
constexpr int HARASS_DEATHS = 3;          // this many team deaths inside HARASS_WINDOW rounds
constexpr int HARASS_WINDOW = 60;
constexpr double GROUP_SWITCH_FACTOR = 1.8;  // anti-oscillation: a new target must beat the old by this
constexpr double MIDDLE_CONTEST_BONUS = 1.15; // middle-band groups score this multiplier when not behind
constexpr int MIDDLE_BEHIND_GEMS = 100;   // "behind" = banked fewer than them by this much

// ---- v11: Prism ------------------------------------------------------------------------------
constexpr double PRISM_BANK_FACTOR = 2.0; // carried gems are +1.5 Focus each: Prism banks this much later

// ---- Fighting champions [S6] ---------------------------------------------------------------
constexpr double PWIN_FIGHT = 0.60;    // fight only if our chance to win the kill race is at least this
constexpr double ETA_UNSEEN = 7.0;     // rounds for an alive-but-unseen enemy to arrive and help
constexpr int SIGHTING_FRESH = 3;      // rounds a shared sighting stays useful
constexpr int HELP_RANGE = 10;         // steps within which teammates react to a sighting
constexpr double RACE_SOFTNESS = 1.0;  // s_w in p_win: how sharp the logistic curve is (rounds)
constexpr int ENGAGE_RANGE = 8;        // enemies within this many tiles take part in a fight
constexpr int HUNT_RANGE = 10;         // interceptor chases a visible target this far

// ---- Banking [S7] --------------------------------------------------------------------------
constexpr int FARM_BAG_BASE = 30;       // farming pairs bank at FARM_BAG_BASE + FARM_BAG_PER_STEP x steps to the altar
constexpr double FARM_BAG_PER_STEP = 1.0;
constexpr int FARM_BAG_CAP = 90;
constexpr double COMMIT_PWIN = 0.35;      // v10: fight an enemy in range of us at this chance when we are not outnumbered
constexpr int WALK_HOME_STEPS = -1;        // v10: walking away this close to home means walking home
constexpr bool CORNERED_FIGHT = true;     // v10: hit back when we cannot get away
constexpr bool SPAWN_HOLD = true;         // v10: hold the spawn against a losing fight / a 2+ sighting near home
constexpr int SPAWN_HOLD_RANGE = 12;       // v10: a 2+ enemy sighting this close to home keeps a lone champion on the spawn
constexpr int SPAWN_HOLD_MAX = 25;         // ...for at most this many rounds in a row
constexpr int STANDOFF_HOME_ROUNDS = 20;   // v10: after a stand-off, head home this long
constexpr int STUCK_ROUNDS = 25;            // watchdog: same tile, nothing hit, not a legitimate wait
constexpr int STUCK_AVOID_ROUNDS = 40;      // ...then skip the current group this long
constexpr int STUCK_SKIP_BANK_ROUNDS = 30;  // ...and the bank / endgame steps this long           // v10: the far-altar threshold never exceeds this
constexpr int FARM_BANK_BAG = 150;       // Omar's rule for farming pairs: think about banking from 150
constexpr double FARM_KEEP_PDIE = 0.25;  // at >= 150, keep farming while P_die <= this
constexpr double VERY_HIGH_PDIE = 0.60;  // recall home first if possible
constexpr double H0 = 0.002;             // background risk of dying per round at full health
constexpr int BAG_HOR = 25;              // Heart of Ruin banks at this bag, 3-champion game
constexpr int BAG_FAR = 30;              // far pair banks at this bag, 4-5 champions
constexpr int BAG_CI = 30;               // Cassi and Issac bank at this bag
constexpr int FREE_BANK_STEPS = 3;       // bank any bag if the altar costs <= this many extra steps
constexpr bool FREE_BANK_ENABLED = true; // experiment E3
constexpr int HOR_ANY_BAG_ALTAR = 8;     // Heart of Ruin banks any bag when an altar is this close
// Farming pairs also bank a medium bag when an altar is close (bench, Oct 21: on collision Marcella
// carried 4-gem wolves toward 150 for 1,000+ rounds, died once and lost everything).
constexpr int CHEAP_BANK_BAG = 30;
constexpr int CHEAP_BANK_STEPS = 10;
constexpr int ENDGAME_MARGIN = 10;       // rounds of margin before the last round
constexpr int RECALL_BANK_MARGIN = 6;    // recall-then-walk must save at least this many rounds
constexpr int WIN_GEMS = 1000;
constexpr int LAST_ROUND = 2000;

// ---- Shopping [S8] -------------------------------------------------------------------------
// Gap analysis (Oct 31) variant: buy the ladder top teams' build (Bruiser + Durability of Health, then Bruiser / Jungle /
// Prospector of Strength) instead of v13's Monster-first order. Off = v18.
constexpr bool SHOP_TOP_BUILD = false;
constexpr int SHOP_GOLD = 150;  // gold that triggers a shop trip (recall home to buy)...
constexpr int SHOP_TRIP_STEPS = 25;   // ...when the walk back from spawn is at most this many steps
constexpr int SHOP_RICH_GOLD = 450;   // ...or we are this rich (ambush: spawn is 70 steps from any camp)
constexpr int LOCK_COST = 10;   // altar lock price

// ---- Cassi & Issac [S9] --------------------------------------------------------------------
constexpr int CI_MAX_GAP = 6;        // max Chebyshev distance between Cassi and Issac at turn end
constexpr int LOCK_ENEMY_BAG = 20;   // lock an altar an enemy walks to with at least this bag

// ---- Smaller strategy numbers (one place to tune them) -------------------------------------
constexpr double SHIELD_BELOW_HEALTH = 0.90;   // Heart of Ruin shields when hit and below 90% health
constexpr int CONTACT_SLACK = 8;               // P_die: an enemy this many tiles beyond our trip can still meet us
constexpr int MEMORY_ROUNDS = 10;              // P_die: enemies seen within this many rounds still count
constexpr double JUNGLE_FACTOR = 0.70;         // Jungle emblem: camps deal 70% damage
constexpr int HELP_MAX_BAG = 40;               // do not answer a call for help carrying this many gems
constexpr int FARMER_HELP_RANGE = 6;           // farming pairs only answer calls this close
constexpr int IDLE_BANK_BAG = 10;              // waiting: bank a bag of at least this...
constexpr int IDLE_BANK_STEPS = 12;            // ...when an altar is this close
constexpr double IDLE_HEAL_HEALTH = 0.70;      // waiting below this health: recall to heal if home is near
constexpr int IDLE_HEAL_HOME_STEPS = 30;       // ...within this many steps (always below 50%)
constexpr int RESCORE_ROUNDS = 10;             // the squad chooser re-checks its target this often
constexpr int GROUP_SEEN_RANGE = 12;           // only groups this close can be in vision (cheap filter)
constexpr int NEAR_TRIP = 6;                   // closer than this to a goal: exact live path finding

// ---- Fuel [S11] (a C++ turn has 100,000 fuel) ----------------------------------------------
constexpr std::uint64_t FUEL_FOR_WARD = 20000;     // skip the ward step below this much fuel left
constexpr std::uint64_t FUEL_FOR_FIRST_TARGET = 40000;   // v13: choosing when we have no target at all
constexpr std::uint64_t FUEL_SCAN_FLOOR = 25000;         // v13: the group scan stops below this
constexpr std::uint64_t FUEL_FOR_ATTACK = 5000;          // step 0: enough to finish one basic attack
constexpr std::uint64_t FUEL_FOR_SIDE_JOB = 60000;       // step 0: side-job scan (~30k) + acting on it (~20k)

// ---- Level 1 modes (design step c) ----------------------------------------------------------------
constexpr bool USE_STEP_B_FIELDS = false;   // rules reading scout / scorer / objective / help emergency: off until step b passes
constexpr int MODE_HOLD_TURNS = 3;          // FARM, EXPLORE and SUPPORT are kept this many turns once chosen
constexpr float SUPPORT_HEAL_MISSING = 60;  // SUPPORT (Willow): an ally in range missing this much health
constexpr int SUPPORT_HEAL_RANGE = 4;       // ...within her heal range
constexpr int SUPPORT_SCORER_BAG = 40;      // SUPPORT (escort, step b): the scorer carries this many gems
constexpr int SUPPORT_DANGER_RANGE = 10;    // ...and a danger record lies this close to it

// ---- Level 2: Q(a) (design step d) ----------------------------------------------------------------
// LEVEL2 is a preprocessor switch: 0 compiles step c's code exactly (no Level 2 code runs or costs fuel;
// any code change shifts fuel and so suspension points, which flipped 17 of 140 results on Oct 30).
#ifndef LEVEL2
#define LEVEL2 0                               // 0 = Level 1 only (v13's choice inside each mode). Off: not yet better than step c
#endif
constexpr bool LEVEL2_ON = LEVEL2;
// Per-mode Level 2 switches (each off = v13's choice in that mode). FARM_SQUAD, CARRY and FIGHT lost on the
// 20-map bisect (Oct 30: -150, -245, -85 gems vs Level 1 +123) with the starting weights: off until lambda tuning.
constexpr bool LEVEL2_FARM_SQUAD = false;      // the squad chooser's group among the top 3
constexpr bool LEVEL2_FARM_TURN = true;        // capture here / contest / target, best Q first
constexpr bool LEVEL2_CARRY = false;           // altar or recall
constexpr bool LEVEL2_FIGHT = false;           // which enemy to fight
constexpr bool LEVEL2_RETREAT = true;          // recall / step away / walk home around danger
constexpr bool LEVEL2_EXPLORE = true;          // stale groups instead of the idle point
constexpr std::uint64_t LEVEL2_FUEL_FLOOR = 40000;   // below this at Level 2's start, run v13's choice
constexpr int LEVEL2_CANDIDATES = 3;           // camp groups / altars / stale groups scored per mode
constexpr double LAMBDA_E = 0.9;               // expected gems: slightly below a scored gem, so banking wins ties
constexpr double LAMBDA_D = 1.0;               // L_D is already in gems
constexpr double LAMBDA_T = 1.0;               // C_time is already in gems
constexpr double INCOME_EARLY = 0.3;           // r before LEVEL2_EARLY_ROUNDS (gems per body-round)
constexpr int LEVEL2_EARLY_ROUNDS = 100;
constexpr double INCOME_FLOOR = 0.15;          // r never below this
constexpr double KILL_GOLD_GEMS = 10;          // the 100 kill gold an enemy gains, at 0.1 gem per gold
constexpr double RESPAWN_ROUNDS_DEAD = 60;     // rounds dead after a death
constexpr double P_AVAILABLE_UNKNOWN = 0.5;    // EXPLORE: a stale group is back / alive with this chance
constexpr double RETREAT_STEP_ROUNDS = 2;     // RETREAT: a step away counts as this many rounds of exposure
constexpr int CARRY_KEEP_ROUNDS = 10;          // CARRY: keep the chosen altar this long (no swapping between similar ones)
constexpr double DANGER_UNKNOWN_PLOSE = 0.5;   // a team danger record of unknown type beats me this often
constexpr double P_DEATH_CALIBRATION = 0.16;   // P(death) x this: 30 Oct calibration bench (LOG_PDEATH, tools/calibrate.py)
                                                // found deaths at 0.16x the prediction in every 10% bucket

// ---- Belief map (design step a) --------------------------------------------------------------------
constexpr int SAFE_RING_ROUNDS = 8;      // a tile seen within this many rounds is known safe
constexpr int STALE_ROUNDS = 40;         // an Alive group unseen this long is stale (EXPLORE re-checks it)
constexpr int MAX_ALTARS = 16;
constexpr int MAX_THREATS = 8;           // threat records kept
constexpr int THREAT_DROP_ROUNDS = 15;   // a threat record unseen this long is dropped
constexpr int THREAT_MAX_RADIUS = 8;     // danger square: reach + age, at most this
constexpr double THREAT_DECAY = 0.85;    // weight of a record in P(death): 0.85^age
constexpr std::uint64_t FUEL_FOR_DANGER = 20000;   // redraw the danger board only with this much left
constexpr std::uint64_t FUEL_FOR_RESCORE = 70000;  // skip re-choosing a target below this (it scores every group)
constexpr std::uint64_t FUEL_FOR_PATH = 30000;     // skip optional path searches below this
constexpr std::uint64_t FUEL_FOR_TURN = 60000;     // a normal turn needs about this much (big maps)
constexpr std::uint64_t SETUP_FUEL_RESERVE = 12000;// stop a setup step with this much fuel left
// Group setup (fix A, Oct 31): start the next group's step only with (largest step measured this game) +
// margin left; before any is measured, GROUP_SETUP_FIRST_GUARD. The edge pass resumes per tile.
// (One group cost up to 27k against the old 12k guard: 140 of 247 overruns on the 5 worst maps.)
constexpr std::uint64_t GROUP_SETUP_FIRST_GUARD = 30000;
constexpr std::uint64_t GROUP_SETUP_MARGIN = 5000;
constexpr std::uint64_t FIELD_FUEL = 60000;
constexpr std::uint64_t DRAFT_PLAN_RESERVE = 120000;  // draft: write the map plan only with this much left after building it
constexpr std::uint64_t DRAFT_MAP_START_FUEL = 500000;
constexpr std::uint64_t THRESHOLD_FUEL = 180000;      // map build: zone thresholds need this much
constexpr std::uint64_t CLASSIFY_FUEL = 60000;        // map build: per-group distances need this much // draft: start the map build only with this much fuel        // one distance field + export on a 100x100 map ~50k

// ---- Debug ---------------------------------------------------------------------------------
constexpr bool LOG_PDEATH = false; // true: print every Level 2 P(death) prediction (tools/calibrate.py; keep false to submit)
constexpr bool LOG_TURNS = false;  // true: print one line per turn (costs fuel; keep false to submit)
