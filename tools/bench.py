#!/usr/bin/env python3
"""Play bot A against bot B on every map, both sides, and report per map from A's point of view.

Usage: python3 tools/bench.py bots/v13 bots/v15 [--maps 00anvil,00eight] [--jobs 8] [--out replays/bench/NAME]

Per map and side: result, gems tributed (A and B), round A reached 1000 gems, deaths, bot errors
("bot error" lines) and out-of-fuel turns ("fuel out" lines; only bots with that log line in main.cpp
report them). Writes results.csv in the output folder and prints a summary.
"""
import argparse
import csv
import json
import pathlib
import subprocess
import sys
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent


def round_reaching(runs, target=1000):
    """First round whose submitted-gems value is >= target, from a run-length timeline; None if never."""
    t = 0
    for r in runs:
        if int(r["value"]) >= target:
            return t
        t += int(r["duration"])
    return None


def team_logs(replay):
    out = {}
    with zipfile.ZipFile(replay) as z:
        for side in ("a", "b"):
            text = z.read(f"team-{side}.stdout").decode(errors="replace")
            out[side] = (text.count("bot error"), text.count("fuel out"))
    return out


def parse(stats_path, replay, a_side):
    d = json.loads(pathlib.Path(stats_path).read_text())
    me, them = ("teamA", "teamB") if a_side == "a" else ("teamB", "teamA")
    gems = {t["team"]: int(t["submittedGems"]) for t in d["teams"]}
    deaths = {"teamA": 0, "teamB": 0}
    for c in d["champions"]:
        deaths[c["team"]] += int(c["metrics"]["deaths"])
    reach = {tl["owner"]: round_reaching(tl["runs"]) for tl in d["teamTimelines"] if tl["metric"] == "submittedGems"}
    outcome = d["match"]["outcome"]
    result = "W" if outcome == me else "L" if outcome == them else "D"
    logs = team_logs(replay)
    mine, theirs = (logs["a"], logs["b"]) if a_side == "a" else (logs["b"], logs["a"])
    faults = d["diagnostics"]["botFaults"]
    return {
        "result": result, "gems_a": gems[me], "gems_b": gems[them], "reach_a": reach.get(me), "reach_b": reach.get(them),
        "deaths_a": deaths[me], "deaths_b": deaths[them], "errors_a": mine[0] + faults[me], "errors_b": theirs[0] + faults[them],
        "fuel_out_a": mine[1], "fuel_out_b": theirs[1],
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("bot_a")
    ap.add_argument("bot_b")
    ap.add_argument("--maps", default="all")
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--out")
    args = ap.parse_args()
    a, b = (ROOT / args.bot_a).resolve(), (ROOT / args.bot_b).resolve()
    maps = sorted((ROOT / "maps").glob("*.map26a")) if args.maps == "all" else [ROOT / "maps" / f"{m}.map26a" for m in args.maps.split(",")]
    out = pathlib.Path(args.out) if args.out else ROOT / "replays" / "bench" / f"{a.name}-vs-{b.name}"
    out.mkdir(parents=True, exist_ok=True)
    # run-batch refuses to overwrite, and stale files would be parsed as this run's results.
    for old in [*out.glob("*.repl26a"), *out.glob("*.json")]:
        old.unlink()

    rows = []
    with open(out / "manifest.txt", "w") as f:
        for m in maps:
            for side in ("a", "b"):
                first, second = (a, b) if side == "a" else (b, a)
                stem = out / f"{m.stem}-{side}"
                f.write(f"{first} {second} {m} {stem}.repl26a --statistics {stem}.json --json\n")
                rows.append((m.stem, side, stem))
    subprocess.run([str(ROOT / ".venv/bin/cambc"), "run-batch", str(out / "manifest.txt"), "--release", "--jobs", str(args.jobs)],
                   check=False, stdout=subprocess.DEVNULL)

    results = []
    for name, side, stem in rows:
        try:
            r = parse(f"{stem}.json", f"{stem}.repl26a", side)
        except (OSError, KeyError, ValueError) as e:
            r = {"result": "FAILED", "error": str(e)}
        results.append({"map": name, "a_side": side, **r})

    fields = ["map", "a_side", "result", "gems_a", "gems_b", "reach_a", "reach_b", "deaths_a", "deaths_b",
              "errors_a", "errors_b", "fuel_out_a", "fuel_out_b"]
    with open(out / "results.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields, extrasaction="ignore")
        w.writeheader()
        w.writerows(results)

    ok = [r for r in results if r["result"] in "WLD"]
    failed = len(results) - len(ok)
    wins = sum(r["result"] == "W" for r in ok)
    losses = sum(r["result"] == "L" for r in ok)
    diff = sum(r["gems_a"] - r["gems_b"] for r in ok) / max(1, len(ok))
    reach = [r["reach_a"] for r in ok if r["reach_a"] is not None]
    print(f"{a.name} vs {b.name}: {len(ok)} matches ({failed} failed)")
    print(f"  wins {wins}  losses {losses}  draws {len(ok) - wins - losses}  win rate {wins / max(1, len(ok)):.1%}")
    print(f"  mean gem difference {diff:+.1f}")
    print(f"  reached 1000: {a.name} {len(reach)} times, mean round {sum(reach) / max(1, len(reach)):.0f}; "
          f"{b.name} {sum(r['reach_b'] is not None for r in ok)} times")
    for k in ("deaths", "errors", "fuel_out"):
        print(f"  {k}: {a.name} {sum(r[k + '_a'] for r in ok)}  {b.name} {sum(r[k + '_b'] for r in ok)}")
    print(f"  results: {out / 'results.csv'}")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
