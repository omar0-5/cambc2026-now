#!/usr/bin/env python3
"""Queue scrim groups (cambc match-groups) against rank bands, rotating maps and sides; saves the plan for scrim_report.py.

Usage: python3 tools/scrim_queue.py OUT.json --band 1-10:15 --band 11-50:15 --band 51-150:15 [--seed N]
       (BAND:GROUPS; teams spread evenly across the band, non-dev only)"""
import argparse
import json
import pathlib
import subprocess
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
C = str(ROOT / ".venv/bin/cambc")


def cj(*a):
    return json.loads(subprocess.run([C, *a, "--json"], capture_output=True, text=True).stdout)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--band", action="append", required=True)
    ap.add_argument("--seed", type=int, default=0)
    args = ap.parse_args()
    q = cj("match-groups", "quota")
    left = q["quota"]["limit"] - q["quota"]["used"]
    maps = [m["name"] if isinstance(m, dict) else m for m in q["maps"]]
    teams = []
    for page in (1, 2, 3, 4):
        teams += cj("ladder", "--page", str(page), "--limit", "50")["teams"]
    me = q["team"]["id"]
    plan, mi = [], args.seed * 5
    for spec in args.band:
        band, n = spec.split(":")
        lo, hi = map(int, band.split("-"))
        pool = [t for t in teams if lo <= t["rank"] <= hi and not t["isDev"] and t["teamId"] != me]
        for i in range(int(n)):
            t = pool[(i * len(pool)) // int(n) % len(pool)]
            ms = [maps[(mi + k) % len(maps)] for k in range(5)]
            mi += 5
            sides = "a,b,a,b,a" if len(plan) % 2 == 0 else "b,a,b,a,b"
            plan.append((t["teamId"], ms, sides, t["rank"], t["name"]))
    plan = plan[:left]
    out = []
    for tid, ms, sides, rank, name in plan:
        r = subprocess.run([C, "match-groups", "create", tid, "--maps", ",".join(ms), "--sides", sides, "--json"], capture_output=True, text=True)
        try:
            gid = json.loads(r.stdout)["groupId"]
        except (ValueError, KeyError):
            print("ERR", name, r.stdout[-200:], r.stderr[-200:])
            continue
        out.append((tid, ms, gid))
        print(f"#{rank:<4} {name[:24]:24} {sides} {ms}")
        time.sleep(1)
    json.dump(out, open(args.out, "w"))
    print(f"queued {len(out)} groups (quota had {left} left)")


if __name__ == "__main__":
    main()
