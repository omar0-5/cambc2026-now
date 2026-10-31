#!/usr/bin/env python3
"""How and where we die, from match summaries of scrim batches (tools/scrim_queue.py plan files).

Per death of ours: killer (champion name or monster), gems carried (gemLoss in the same round), round, what we were doing in
the 5 rounds before (fight / camp / tribute / capture / healed / walk_idle, from tools/time_breakdown.py's per-round timelines),
and how many allies died within 5 rounds of it (multi-deaths = lost team fights).

Usage: python3 tools/death_report.py PLAN.json [PLAN.json ...]
"""
import collections
import json
import math
import pathlib
import statistics
import subprocess
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from time_breakdown import per_round, rises  # noqa: E402

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


def activity(tl, ev, r):
    """What a champion did in rounds r-5 .. r-1 (first category that fits, like time_breakdown)."""
    window = range(max(1, r - 5), r)
    done = rises(tl.get("championDamageDone", []))
    taken = rises(tl.get("championDamageTaken", []))
    hit_back = any(done[i] for i in window if i < len(done))
    caught = any(taken[i] for i in window if i < len(taken))
    if caught or hit_back:
        if hit_back:
            return "fight (we hit back)"
        first = next(i for i in window if i < len(taken) and taken[i])   # what we did before the first enemy hit
        before = activity(tl, ev, first)
        if before.startswith("fight") or before.startswith("caught") or before.startswith("stopped"):
            return "fought, then stopped hitting back (fled)"
        return "caught, no hit back, while " + before
    for x in ev["gemSubmission"]:
        end = x["timestamp"]["round"]
        if end - max(1, math.ceil(float(x["value"]) / 10)) <= r <= end + 1:
            return "tributing"
    if any(rises(tl.get("monsterDamageDone", []))[i] or rises(tl.get("monsterDamageTaken", []))[i] for i in window if i < len(tl.get("monsterDamageDone", []))):
        return "attacking a camp"
    if any(abs(x["timestamp"]["round"] - r) <= 5 for x in ev["gemCollection"]):
        return "capturing"
    return "walking / idle"


def deaths_in(d, team):
    n = d["match"]["roundsCompleted"]
    tl = collections.defaultdict(dict)
    for t in d["championTimelines"]:
        if t["owner"].get("team") == team:
            tl[t["owner"]["id"]][t["metric"]] = per_round(t["runs"], n)
    ev = collections.defaultdict(lambda: collections.defaultdict(list))
    for e in d["events"]:
        o = e["owner"]
        if o.get("team") == team and o.get("kind") == "champion":
            ev[o["id"]][e["kind"]] += e["entries"]
    names = {c["id"]: c["name"] for c in d["champions"] if c["team"] == team}
    all_rounds = [x["timestamp"]["round"] for cid in names for x in ev[cid]["death"]]
    out = []
    for cid, name in names.items():
        lost = {x["timestamp"]["round"]: float(x["value"]) for x in ev[cid]["gemLoss"]}
        for x in ev[cid]["death"]:
            r = x["timestamp"]["round"]
            killer = x["target"].get("name", "?") if x["context"] == "champion" else "monster"
            out.append({"victim": name, "killer": killer, "round": r, "gems": lost.get(r, 0.0),
                        "doing": activity(tl[cid], ev[cid], r),
                        "allies_dying_within_5": sum(1 for q in all_rounds if abs(q - r) <= 5) - 1})
    return out


def main():
    me = json.load(open(ROOT / "replays/gap/ranks.json"))["me"]["id"]
    deaths, games = [], 0
    for plan in sys.argv[1:]:
        for _tid, _maps, gid in json.load(open(plan)):
            g = show_group(gid)
            for m in g["matches"]:
                if m.get("status") != "finished":
                    continue
                f = S / f"{m['matchId']}.json"
                if not f.exists():
                    subprocess.run([C, "matches", "summary", m["matchId"], "--json", "-o", str(f)], capture_output=True)
                if not f.exists():
                    continue
                us = next(t for t in m["teams"] if t["teamId"] == me)
                games += 1
                for x in deaths_in(json.load(open(f)), "team" + us["side"].upper()):
                    x["match"] = m["matchId"][:8]
                    x["map"] = m["map"]
                    deaths.append(x)
    n = len(deaths)
    if not n:
        print("no deaths")
        return
    pct = lambda c: ", ".join(f"{k} {v} ({100 * v / n:.0f}%)" for k, v in c.most_common(8))
    print(f"{n} deaths in {games} games ({n / max(1, games):.1f} per game)")
    print("  killed by:", pct(collections.Counter(x["killer"] for x in deaths)))
    print("  doing (5 rounds before):", pct(collections.Counter(x["doing"] for x in deaths)))
    print("  victim:", pct(collections.Counter(x["victim"] for x in deaths)))
    print("  round:", pct(collections.Counter(f"{x['round'] // 250 * 250}-{x['round'] // 250 * 250 + 249}" for x in deaths)))
    print("  allies dying within 5 rounds:", pct(collections.Counter(min(x["allies_dying_within_5"], 4) for x in deaths)))
    gems = [x["gems"] for x in deaths]
    print(f"  gems carried: mean {statistics.mean(gems):.1f}, 0 gems {sum(g == 0 for g in gems)}, 30+ gems {sum(g >= 30 for g in gems)}, "
          f"total lost {sum(gems):.0f} ({sum(gems) / max(1, games):.0f} per game)")
    worst = sorted(deaths, key=lambda x: -x["gems"])[:5]
    print("  biggest losses:", [(x["match"], x["map"], x["victim"], x["killer"], int(x["gems"]), x["doing"]) for x in worst])


if __name__ == "__main__":
    main()
