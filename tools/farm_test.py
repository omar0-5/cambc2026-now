#!/usr/bin/env python3
"""Farm test: a bot vs bots/passive (drafts, never acts), both sides, full games (statistics export only for finished games; round-1,000 values come from timelines). Measures farming speed with
the same metrics as the ladder gap analysis (tools/gap_report.py). Top-team ladder reference: 628 gems tributed by
round 1,000, first tribute ~r230, 5.29 camps per 100 rounds, ~4,900 gold earned before round 1,000.

Usage: python3 tools/farm_test.py BOT_DIR [--half A|B] [--name NAME] [--jobs 2]
"""
import argparse
import csv
import pathlib
import statistics
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from gap_report import team_stats  # noqa: E402

import json  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent
KEYS = ["gems_r1000", "t_first", "camps_per_100", "gold_lt1000", "collected", "tributes", "deaths", "fuel_out", "size"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("bot")
    ap.add_argument("--half", choices=["A", "B"])
    ap.add_argument("--name")
    ap.add_argument("--jobs", type=int, default=2)
    args = ap.parse_args()
    bot = (ROOT / args.bot).resolve()
    passive = ROOT / "bots" / "passive"
    maps = sorted((ROOT / "maps").glob("*.map26a"))
    if args.half:
        maps = maps[0 if args.half == "A" else 1::2]
    out = ROOT / "replays" / "farm" / (args.name or bot.name)
    out.mkdir(parents=True, exist_ok=True)
    for old in [*out.glob("*.repl26a"), *out.glob("*.json")]:
        old.unlink()
    rows = []
    with open(out / "manifest.txt", "w") as f:
        for m in maps:
            for side in ("a", "b"):
                first, second = (bot, passive) if side == "a" else (passive, bot)
                stem = out / f"{m.stem}-{side}"
                f.write(f"{first} {second} {m} {stem}.repl26a --statistics {stem}.json --json\n")
                rows.append((m.stem, side, stem))
    subprocess.run([str(ROOT / ".venv/bin/cambc"), "run-batch", str(out / "manifest.txt"), "--release", "--jobs", str(args.jobs)],
                   check=False, stdout=subprocess.DEVNULL)
    results, failed = [], []
    for name, side, stem in rows:
        try:
            s = team_stats(json.load(open(f"{stem}.json")), "team" + side.upper())
        except (OSError, KeyError, ValueError):
            failed.append(name + "-" + side)
            continue
        s["camps_per_100"] = s["camps"] / max(1, s["rounds"]) * 100
        results.append({"map": name, "side": side, **{k: s[k] for k in KEYS}})
        for p in (pathlib.Path(f"{stem}.json"), pathlib.Path(f"{stem}.repl26a")):
            p.unlink(missing_ok=True)
    with open(out / "results.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["map", "side", *KEYS])
        w.writeheader()
        w.writerows(results)
    mean = lambda k: statistics.mean(r[k] for r in results) if results else float("nan")
    print(f"{args.name or bot.name}: {len(results)} matches vs passive ({len(failed)} failed {failed[:5]})")
    print(f"  gems tributed by r1000 {mean('gems_r1000'):.0f} (top 628) | first tribute r{mean('t_first'):.0f} (top ~230) | "
          f"camps/100 rounds {mean('camps_per_100'):.2f} (top 5.29) | gold by r1000 {mean('gold_lt1000'):.0f} (top ~4,900)")
    print(f"  collected {mean('collected'):.0f}, tributes {mean('tributes'):.1f}, deaths {mean('deaths'):.1f}, fuel out {mean('fuel_out'):.1f} | {out / 'results.csv'}")


if __name__ == "__main__":
    main()
