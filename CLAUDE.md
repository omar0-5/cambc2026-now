# CAMBC 2026a — v15 bot

We are building a competition bot for **CAMBC 2026a** (Battlecode-style). Our bot is **`bots/v15`**, written in **C++** and compiled to WebAssembly. It is the only bot in this repo; do not recreate the Python bots or the other-language SDKs.

## The game in one paragraph

Each team drafts 3–5 champions. Champions kill monster camps, stand on the cleared camp for 5 rounds to capture its gems and gold, then tribute the gems at an altar. The first team to tribute 1,000 gems wins; otherwise, at round 2,000, the team with more tributed gems wins. Gold buys shards and emblems on your own spawn. Champions don't share vision; they coordinate only through 1,024 bytes of draft memory (written in the draft) and 8 bytes of shared memory. A C++ bot gets 100k fuel (compute) per turn, 2x on the first turn of play and 100x on the first draft turn.

Full rules: `docs/GameRules.md`. C++ API: `docs/BotAPICpp.md` (large: search it, don't read it whole).

## Strategy document

Every formula the bot uses (draft, group value V(g), P_die, banking, kill race, shopping, comms byte layouts, all `config.h` constants) is written up here:
https://claude.ai/artifact/Tiey7oXuPuQF8Y28kCD7QT ("v15 Bot — Strategy & Equations", a Claude Docs doc). When a change alters a formula or threshold, update the matching section of that doc too.

## Layout

| Path | What |
| --- | --- |
| `bots/v15/src/` | The bot. One module per job (below) |
| `bots/v15/src/config.h` | Every tunable number, in one place |
| `sdk/cpp/` | The C++ SDK headers (`cambc.h`) — do not edit; refreshed by `cambc init` |
| `sdk/templates/cpp/` | Template for `cambc new` |
| `cmake/CMakeLists.txt` | Build file used by `cambc build` |
| `maps/` | The 70 competition maps (`*.map26a`) |
| `cambc.toml` | `sdk_version = "4.0.0"`, `languages = ["cpp"]` |
| `.venv/` | Holds the `cambc` CLI — keep it |

Modules in `bots/v15/src/`: `main` (entry, try/catch, always `end_turn`) · `draft` (fixed lineup per team size, writes plan) · `mapinfo` (distance fields, zones d15/d60, group facts) · `world` (per-turn Snapshot, enemy memory, roles) · `roles` (the 12-step turn ladder) · `farm` (target choice, standing, capture) · `bank` (P_die, when to tribute) · `combat` (kill race, fight/interrupt/walk away) · `actions` (spell and attack targets) · `move` (pathing) · `shop` (buy lists) · `comms` (memory layouts) · `disrupt` (Cassi & Issac only).

## Commands

```sh
.venv/bin/cambc build bots/v15                        # compile
.venv/bin/cambc run bots/v15 bots/v15 --release --map 00anvil   # self-play on one map
.venv/bin/cambc run bots/v15 <other-bot> --release --statistics out.json --json
.venv/bin/cambc view                                  # open the latest replay
```

Write replays and scratch output outside the repo (or in `replays/`, which is gitignored). For an opponent, create one with `cambc new` and delete it after.

## Rules for working on v15

- Change **one number in `config.h` at a time**, then re-run matches on several maps before keeping it.
- A thrown `cambc::error` costs 10k+ fuel: check `can_*` before acting; exceptions are a safety net, not control flow.
- Watch fuel: never loop over every tile in C++ (a 100x100 loop costs ~80k); use engine bitboards and distance fields.
- A draft turn must pick before doing anything else; running out of fuel in the draft makes the rest of the picks random.
- Known dead code: `MONOLITH_SAFE_DISTANCE`, `LOCK_CARRIER_RANGE`, `SUPPORT_FOLLOW_RANGE`, `ETA_UNSEEN` are defined but unused. The table in the `shop.cpp` header comment is stale; the code is the truth.
- Only `DRAFT_SET = 0` ships (Marcella, Vesper Vale, Vendetta / + Ryth / + Heart of Ruin). Sets 1–2 are untested alternatives.

## Change log

**After every change to this project, add an entry at the top of this list** (date, what changed, why, and how it was verified — e.g. build result and match scores). Keep each entry to a few lines.

- **2026-10-30** — Step c (Level 1 modes) committed on main: the ladder picks one mode per turn in the doc's rule order (CARRY, RETREAT, FIGHT, CARRY, SUPPORT, EXPLORE, FARM, EXPLORE) with 3-turn hysteresis for FARM / EXPLORE / SUPPORT; shop trip, ward, locks, watchdog stay outside. Rules reading step b fields (scorer escort, scout) behind `USE_STEP_B_FIELDS` = false; v13 comms kept. Bench vs v13-baseline 77–63 (+33), vs v15-baseline 77–63 (+50); overruns 483 + 481; 0 errors. Step b isolations on step a (vs v13-baseline): shop pairing 70–70, spawn-hold count 68–72, leader join/merge 69–71, emergency rule 77–63 (+48) — none explains step b's loss.
- **2026-10-30** — Lock fix on main (step a): any champion breaks an enemy altar lock (was Marcella only). Bench vs v13-baseline 73–67 (+36), vs v15-baseline 72–68 (+45); overruns 481 + 499 (step a 474 + 504); 0 errors. Not submitted (waits for the v17 trial report). Step b isolation 1 (shop pairing by vision, alone on step a): 70–70 (+10) — not the cause.
- **2026-10-30** — Step b (64-bit shared memory) built per the design doc and FAILED its bench; parked on branch `step-b-wip`, main stays at step a. Found and fixed a doc contradiction: with 127 = none, the zeroed memory a game starts with reads as "every claim is group 0"; claims and bodies are now stored as value + 1. Bench vs v13-baseline 60–80 (−89), vs v15-baseline 75–65 (+23); a same-type emergency refresh variant 61–79 / 63–77. Fuel per turn +3k over step a. Debug self-checks (codec round trip, belief) pass, 0 errors.
- **2026-10-30** — Step 0c re-worked (any champion breaks enemy altar locks — root cause of 02treasure-box; periodic-job fuel rule with `PERIODIC_FUEL_MARGIN` 15k, staggered by body slot) and parked on branch `step0c`: vs v13-baseline 77–63 (+43), vs v15-baseline 74–66 (+32), but overruns 540 + 533 > step 0's 484 + 476 and 4 maps newly lost on both sides by 500+.
- **2026-10-30** — Ladder trial v17 = step 0 + no far pair (branch `trial-nofar`), live since ~16:00 UTC; fallback v16 (`cambc submissions activate 16`). Local: vs v15-baseline on the 42 five-champion maps 43–41 (+126), deaths 751 vs step 0's 1,125.
- **2026-10-30** — Step a (belief map, `bots/v13/src/belief.cpp`) committed on main on top of step 0; not submitted (goes out with step b). Four layers per the design doc, updated once a turn in `world::sense`, all boards built once and updated in place; team threats read v13's sighting bytes until step b. Cost ~4k fuel per turn. Bench: vs v13-baseline 69–71 (−1), vs v15-baseline 71–69 (+20); overruns 474 + 504 (step 0: 484 + 476); 0 errors; debug self-check 0 failures.
- **2026-10-30** — Step 0c (real walking distance in target choice) built, NOT committed or submitted: borderline. Per-group walking fields built from leftover fuel (`FUEL_FOR_GROUP_FIELD` 75k), used in group_value, value bound, zone rule, side-job range, contest_capture; join test skipped below the scan floor. Bench: vs v13-baseline 68–72 (−22; step 0 was 69–71, +27), vs v15-baseline 73–67 (−2; step 0 65–75, −42). Fuel per turn mean 46.0k→44.9k, p95 equal, but overruns +14–33%. A 12k "skip the step" guard was tried and removed (it cost 02treasure-box / 00split). Matches are deterministic: repeating one changes nothing. Variant "no far pair" (5th body joins squad 0) vs 0c on 42 five-champion maps: 41–43, +21 gems, deaths 630 vs 891 (report only). Step a (belief.cpp) written on top, passes its debug self-check; not benched yet.
- **2026-10-30** — Step 0b (guarded altars) attempted, FAILED its test, parked on branch `step0b-wip` (main stays at step 0). Live altar field past camps known dead, awake-camp tile check, guard respawn window, `GUARDED_BANK_BAG` 60, guard-camp targets. 01boss-battle / 02guardians tribute on side b only; 02the-pack never (48 wolves in view starve the turn of fuel). Full bench: vs v13-baseline 69–71 (+20), vs v15-baseline 62–78 (−79). Also fixed `tools/bench.py` relative `--out`. Draft test on the 42 five-champion maps (ladder code, both sides): Heart of Ruin → Stellar 29–55 (−209), → Ren 41–43 (−45); draft unchanged.
- **2026-10-30** — Step 0 (v13 fuel overruns). Profiling showed the group scans cost ~30k each: `by_bound` is now cached per round and tile, every scan stops at `FUEL_SCAN_FLOOR`, the side job needs `FUEL_FOR_SIDE_JOB` (60k), and a basic attack needs `FUEL_FOR_ATTACK` (5k; a turn suspended mid-attack threw attack_bad_target on 00vault). Bench, 140 matches each: overruns 1,834 → 484 (lanes 12, islands 6, boss-battle still 54); 0 errors. vs v13-baseline 69–71, +27 gems; vs starter 138–2, +817; vs v15-baseline 65–75, −42 (old v13: 73–67).
- **2026-10-30** — Design doc approved with the six defaults plus four changes (squad C_time, Cassi/Issac tie-break, kill gold in L_D, tuning against both baselines) and the schedule; added to the doc.
- **2026-10-30** — Wrote the Phase 1 design doc for the advisor's design (modes, Q(a), 64-bit shared memory, belief map): https://claude.ai/artifact/RV3juWzzFKb3Mzy8FA9Vyz. Awaiting approval; no design code yet.
- **2026-10-30** — Compared v13 and v15 (release, 70 maps, both sides). v13 vs v15: 73–67, +47 gems/match. vs starter: v13 139–1 (+828, 0 errors), v15 139–1 (+768, 99 dash errors). v13 recommended as base and for the ladder; v15 kept until the user confirms. Both bots run out of fuel ~11 turns/match, 60% on 01boss-battle, 00lanes, 01islands.
- **2026-10-30** — Test setup: froze `bots/v13-baseline` and `bots/v15-baseline`; restored the starter bot as `bots/starter` (gitignored; `cambc init` also re-adds the other-language SDKs, which were deleted again); added `tools/bench.py` (all maps, both sides, per-map CSV). In v13 and v15 `main.cpp` now prints `fuel out: ...` when a turn is suspended; v13's `SHOPLEFT` print is now debug-only.
- **2026-10-27** — Put the project under git and pushed it to https://github.com/omar0-5/cambc2026-now (branch `main`), on top of the repo's existing initial commit. Merged its C++ `.gitignore` with ours; `.venv/`, `.cambc/`, build output, `compile_commands.json` and `.DS_Store` are not committed. 120 files.
- **2026-10-27** — Created this CLAUDE.md with project context and this change log.
- **2026-10-27** — Wrote the strategy document (link above) from a full read of `bots/v15/src`. Found 4 unused constants and a stale comment in `shop.cpp` (listed above). No code changed.
- **2026-10-27** — Cleaned the repo to v15 only: deleted `bots/my-bot`, `bots/starter-python`, `bots/starter-cpp`, and the OCaml, Python and Rust SDKs and templates. Set `languages = ["cpp"]` in `cambc.toml`. Verified with a clean rebuild and a v15 vs v15 match on 00anvil (2000 rounds, 0 crashes). A backup from before the cleanup was kept in a temporary folder, which may since have been cleared.
- **2026-10-27** — Fixed the v15 build: the project SDK was 2.2.5 but v15 targets the newer API (Heart of Ruin, Golem, Capture emblems, `vision::in_vision`, `capture_state`). Ran `cambc init --yes` to update the SDK to 4.0.0; no bot code changed. Build went from 26 errors to clean. Verified: v15 beat the C++ starter bot 1039–374 (1000 gems reached in round 1697), 0 crashes.
