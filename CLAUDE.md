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

- **2026-10-27** — Created this CLAUDE.md with project context and this change log.
- **2026-10-27** — Wrote the strategy document (link above) from a full read of `bots/v15/src`. Found 4 unused constants and a stale comment in `shop.cpp` (listed above). No code changed.
- **2026-10-27** — Cleaned the repo to v15 only: deleted `bots/my-bot`, `bots/starter-python`, `bots/starter-cpp`, and the OCaml, Python and Rust SDKs and templates. Set `languages = ["cpp"]` in `cambc.toml`. Verified with a clean rebuild and a v15 vs v15 match on 00anvil (2000 rounds, 0 crashes). A backup from before the cleanup was kept in a temporary folder, which may since have been cleared.
- **2026-10-27** — Fixed the v15 build: the project SDK was 2.2.5 but v15 targets the newer API (Heart of Ruin, Golem, Capture emblems, `vision::in_vision`, `capture_state`). Ran `cambc init --yes` to update the SDK to 4.0.0; no bot code changed. Build went from 26 errors to clean. Verified: v15 beat the C++ starter bot 1039–374 (1000 gems reached in round 1697), 0 crashes.
