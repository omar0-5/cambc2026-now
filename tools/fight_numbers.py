#!/usr/bin/env python3
"""Numbers in our champion fights, from match summaries (no positions needed).

A fight = a 10-round window ending at a champion death (ours or theirs). Engaged champions = champions of each team that
dealt or took champion damage in that window (proxy for "within ~5 tiles"). Situation = "<ours>v<theirs>"; per situation:
how many fights, our deaths and kills in them, and our champion damage dealt / taken over the window.

Usage: python3 tools/fight_numbers.py PLAN.json [PLAN.json ...]   (scrim plan files; summaries cached in replays/gap/s)
"""
import collections
import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from scrim_report import show_group  # noqa: E402
from time_breakdown import per_round  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent
S = ROOT / "replays/gap/s"
WINDOW = 10


def fights_in(d, us):
    n = d["match"]["roundsCompleted"]
    them = "teamB" if us == "teamA" else "teamA"
    series = collections.defaultdict(dict)   # (team, id) -> metric -> per-round cumulative
    for t in d["championTimelines"]:
        o = t["owner"]
        if t["metric"] in ("championDamageDone", "championDamageTaken"):
            series[(o["team"], o["id"])][t["metric"]] = per_round(t["runs"], n)
    deaths = []
    for e in d["events"]:
        if e["kind"] == "death" and e["owner"].get("kind") == "champion":
            for x in e["entries"]:
                if x["context"] == "champion":
                    deaths.append((x["timestamp"]["round"], e["owner"]["team"]))
    out = []
    used = set()
    for r, victim_team in sorted(deaths):
        lo = max(1, r - WINDOW)
        key = (r // WINDOW, victim_team)
        engaged = {us: 0, them: 0}
        dmg = {"done": 0.0, "taken": 0.0}
        for (team, _cid), m in series.items():
            done, taken = m.get("championDamageDone"), m.get("championDamageTaken")
            dd = (done[min(r, n - 1)] - done[lo - 1]) if done else 0.0
            dt = (taken[min(r, n - 1)] - taken[lo - 1]) if taken else 0.0
            if dd > 0 or dt > 0:
                engaged[team] += 1
            if team == us:
                dmg["done"] += dd
                dmg["taken"] += dt
        out.append({"ours": engaged[us], "theirs": engaged[them], "our_death": victim_team == us,
                    "done": dmg["done"], "taken": dmg["taken"], "dup": key in used})
        used.add(key)
    return out


def label(a, b):
    a, b = min(a, 3), min(b, 3)
    return f"{a}{'+' if a == 3 else ''}v{b}{'+' if b == 3 else ''}"


def main():
    me = json.load(open(ROOT / "replays/gap/ranks.json"))["me"]["id"]
    rows, games = [], 0
    for plan in sys.argv[1:]:
        for _tid, _maps, gid in json.load(open(plan)):
            for m in show_group(gid)["matches"]:
                f = S / f"{m['matchId']}.json"
                if not f.exists():
                    continue
                us = "team" + next(t for t in m["teams"] if t["teamId"] == me)["side"].upper()
                rows += fights_in(json.load(open(f)), us)
                games += 1
    by = collections.defaultdict(lambda: {"n": 0, "deaths": 0, "kills": 0, "done": 0.0, "taken": 0.0})
    for x in rows:
        s = by[label(x["ours"], x["theirs"])]
        s["n"] += 1
        s["deaths" if x["our_death"] else "kills"] += 1
        if not x["dup"]:   # count each window's damage once
            s["done"] += x["done"]
            s["taken"] += x["taken"]
    tot_d = sum(s["deaths"] for s in by.values())
    tot_done = sum(s["done"] for s in by.values())
    tot_taken = sum(s["taken"] for s in by.values())
    print(f"{games} games, {len(rows)} champion deaths in fights ({tot_d} ours, {len(rows) - tot_d} theirs); "
          f"damage done/taken in fight windows {tot_done / max(1, tot_taken):.2f}")
    print("situation (ours v theirs engaged)   fights  our deaths  our kills  dmg done/taken  share of our deaths  share of damage gap")
    gap = max(1.0, tot_taken - tot_done)
    for k, s in sorted(by.items(), key=lambda kv: -kv[1]["n"]):
        print(f"  {k:8} {s['n']:30d} {s['deaths']:11d} {s['kills']:10d} {s['done'] / max(1, s['taken']):15.2f} "
              f"{100 * s['deaths'] / max(1, tot_d):19.0f}% {100 * (s['taken'] - s['done']) / gap:19.0f}%")
    outn = [s for k, s in by.items() if int(k[0]) < int(k[k.index('v') + 1])]
    print(f"outnumbered (fewer of us engaged): {100 * sum(s['deaths'] for s in outn) / max(1, tot_d):.0f}% of our deaths, "
          f"{100 * sum(s['taken'] - s['done'] for s in outn) / gap:.0f}% of the damage gap")


if __name__ == "__main__":
    main()
