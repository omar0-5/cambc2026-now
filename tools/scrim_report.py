#!/usr/bin/env python3
"""Results of queued scrim groups (cambc match-groups), with the gap metrics per match.
Usage: python3 tools/scrim_report.py replays/gap/scrims_before.json [--wait]   (file from the queueing step: [[team, maps, groupId], ...])"""
import json
import pathlib
import subprocess
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from gap_report import team_stats  # noqa: E402
from time_breakdown import breakdown  # noqa: E402
import statistics  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent
C = str(ROOT / ".venv/bin/cambc")
S = ROOT / "replays/gap/s"


def show_group(gid):
    """cambc match-groups show with retries (bursts of requests come back empty: rate limit)."""
    for wait in (0, 3, 10, 30, 60):
        time.sleep(wait)
        out = subprocess.run([C, "match-groups", "show", gid, "--json"], capture_output=True, text=True).stdout
        try:
            return json.loads(out)
        except ValueError:
            continue
    raise RuntimeError(f"match-groups show {gid}: no answer after retries")


def main():
    groups = json.load(open(sys.argv[1]))
    if "--wait" in sys.argv:   # until every match of the batch has finished
        while True:
            left = sum(m.get("status") != "finished" for _t, _m, gid in groups if gid for m in show_group(gid)["matches"])
            if left == 0:
                break
            print(f"{time.strftime('%H:%M', time.gmtime())} unfinished {left}", flush=True)
            time.sleep(180)
    me = json.load(open(ROOT / "replays/gap/ranks.json"))["me"]["id"]
    ranks = json.load(open(ROOT / "replays/gap/ranks.json"))["ranks"]
    S.mkdir(parents=True, exist_ok=True)
    wins = total = 0
    agg = []
    for team, _maps, gid in groups:
        g = show_group(gid)
        for m in g["matches"]:
            if m.get("status") != "finished":
                print(f"  {m.get('map')}: {m.get('status')}")
                continue
            us = next(t for t in m["teams"] if t["teamId"] == me)
            th = next(t for t in m["teams"] if t["teamId"] != me)
            f = S / f"{m['matchId']}.json"
            if not f.exists():
                subprocess.run([C, "matches", "summary", m["matchId"], "--json", "-o", str(f)], capture_output=True)
                time.sleep(1)
            a = b = None
            if f.exists():
                d = json.load(open(f))
                a, b = team_stats(d, "team" + us["side"].upper()), team_stats(d, "team" + th["side"].upper())
                per, _clears, _att, _n = breakdown(d, "team" + us["side"].upper())
                a["dead_pct"] = statistics.mean(c["dead"] for _, c in per.values()) if per else 0.0
                ours = [c for c in d["champions"] if c["team"] == "team" + us["side"].upper()]
                a["dmg_done"] = sum(float(c["metrics"]["championDamageDone"]) for c in ours)
                a["dmg_taken"] = sum(float(c["metrics"]["championDamageTaken"]) for c in ours)
                agg.append(a)
            total += 1
            wins += us["result"] == "win"
            line = f"{us['result']:4} {m['map']:22} vs #{ranks.get(th['teamId'], '?'):<4} {th['name'][:18]:18} side {us['side']}"
            if a:
                line += (f"  tributed {a['tributed']:.0f}-{b['tributed']:.0f}  @r1000 {a['gems_r1000']:.0f}-{b['gems_r1000']:.0f}"
                         f"  camps {a['camps']:.0f}-{b['camps']:.0f}  deaths {a['deaths']:.0f}-{b['deaths']:.0f}  {m['matchId'][:8]}")
            print(line)
    print(f"scrims: {wins}-{total - wins} of {total} finished")
    if agg:
        m = lambda k: statistics.mean(x[k] for x in agg)
        print(f"  gems by r1000 {m('gems_r1000'):.0f}, deaths {m('deaths'):.1f}, kills {m('kills'):.1f}, "
              f"camps/100 rounds {statistics.mean(x['camps'] / x['rounds'] * 100 for x in agg):.2f}, death time {m('dead_pct'):.1f}%, "
              f"damage done/taken {sum(x['dmg_done'] for x in agg) / max(1.0, sum(x['dmg_taken'] for x in agg)):.2f}")


if __name__ == "__main__":
    main()
