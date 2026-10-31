#!/usr/bin/env python3
"""Where champion time goes, from match summaries (no positions needed). Each champion-round of the first 1,000 rounds
gets the first category that fits:
  dead (60 rounds after a death) > fight (champion damage done/taken) > camp (monster damage done/taken)
  > tribute (ceil(gems/10) rounds before each tribute) > capture (5 rounds before a capture reward)
  > healed (healing received: spawn or ally) > walk_idle (everything else; walking and idle cannot be split without positions).
Also: champions per camp clear, and team camp-attack rounds per clear.

Usage: python3 tools/time_breakdown.py [--match ID ...]   (default: every summary in replays/gap, ours vs top teams)
"""
import argparse
import collections
import json
import math
import pathlib
import statistics

ROOT = pathlib.Path(__file__).resolve().parent.parent
G = ROOT / "replays/gap"
CATS = ["dead", "fight", "camp", "tribute", "capture", "healed", "walk_idle"]
HORIZON = 1000
RESPAWN = 60


def per_round(runs, n):
    out, t = [0.0] * n, 0
    for run in runs:
        for r in range(t, min(n, t + int(run["duration"]))):
            out[r] = float(run["value"])
        t += int(run["duration"])
    if t < n and runs:
        for r in range(t, n):
            out[r] = float(runs[-1]["value"])
    return out


def rises(series):
    return [i > 0 and series[i] > series[i - 1] for i in range(len(series))]


def breakdown(d, team):
    """Per champion: {cat: rounds} over the first min(rounds, HORIZON) rounds; plus camp-clear stats for the team."""
    n = min(HORIZON, d["match"]["roundsCompleted"])
    champs = {c["id"]: c["name"] for c in d["champions"] if c["team"] == team}
    tl = collections.defaultdict(dict)
    for t in d["championTimelines"]:
        o = t["owner"]
        if o.get("team") == team:
            tl[o["id"]][t["metric"]] = per_round(t["runs"], n)
    ev = collections.defaultdict(lambda: collections.defaultdict(list))
    for e in d["events"]:
        o = e["owner"]
        if o.get("team") == team and o.get("kind") == "champion":
            ev[o["id"]][e["kind"]] += e["entries"]
    out = {}
    for cid, name in champs.items():
        cat = [None] * n
        m = tl[cid]
        for x in ev[cid]["death"]:
            for r in range(x["timestamp"]["round"], min(n, x["timestamp"]["round"] + RESPAWN)):
                cat[r] = cat[r] or "dead"
        for label, keys in (("fight", ("championDamageDone", "championDamageTaken")), ("camp", ("monsterDamageDone", "monsterDamageTaken"))):
            hits = [any(rises(m.get(k, [0.0] * n))[r] for k in keys) for r in range(n)]
            for r in range(n):
                if hits[r] and not cat[r]:
                    cat[r] = label
        for x in ev[cid]["gemSubmission"]:
            end = x["timestamp"]["round"]
            for r in range(max(0, end - max(1, math.ceil(float(x["value"]) / 10))), min(n, end + 1)):
                cat[r] = cat[r] or "tribute"
        for x in ev[cid]["gemCollection"]:
            end = x["timestamp"]["round"]
            for r in range(max(0, end - 5), min(n, end + 1)):
                cat[r] = cat[r] or "capture"
        healed = rises(m.get("healingReceived", [0.0] * n))
        for r in range(n):
            if healed[r] and not cat[r]:
                cat[r] = "healed"
        c = collections.Counter(x or "walk_idle" for x in cat)
        out[cid] = (name, {k: c[k] / n * 100 for k in CATS})
    clears = collections.defaultdict(set)
    for cid in champs:
        for x in ev[cid]["campClear"]:
            if x["timestamp"]["round"] < n:
                clears[(x["target"].get("id"), x["timestamp"]["round"])].add(cid)
    team_attack_rounds = sum(1 for r in range(n) if any(
        any(rises(tl[cid].get(k, [0.0] * n))[r] for k in ("monsterDamageDone",)) for cid in champs))
    return out, clears, team_attack_rounds, n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--match", nargs="*")
    args = ap.parse_args()
    R = json.load(open(G / "ranks.json"))
    idx = json.load(open(G / "index.json"))
    me = R["me"]["id"]
    top_ids = {t["teamId"] for t in R["top"] if not t["isDev"]}
    groups = {"us": [], "top": []}
    for m in idx["ours"]:
        side = next(t for t in m["teams"] if t["teamId"] == me)["side"]
        groups["us"].append((m, "team" + side.upper()))
    for tid, ms in idx["top"].items():
        if tid not in top_ids:
            continue
        for m in ms:
            side = next(t for t in m["teams"] if t["teamId"] == tid)["side"]
            groups["top"].append((m, "team" + side.upper()))
    if args.match:
        groups = {k: [x for x in v if x[0]["matchId"] in args.match] for k, v in groups.items()}
    for label, recs in groups.items():
        per_champ = collections.defaultdict(list)
        allc, sizes, clears_n, attack_per_clear, clears_per_100 = [], [], 0, [], []
        for m, team in recs:
            f = G / "s" / f"{m['matchId']}.json"
            if not f.exists():
                continue
            out, clears, attack, n = breakdown(json.load(open(f)), team)
            for name, cats in out.values():
                per_champ[name].append(cats)
                allc.append(cats)
            sizes += [len(v) for v in clears.values()]
            clears_n += len(clears)
            clears_per_100.append(len(clears) / n * 100)
            if clears:
                attack_per_clear.append(attack / len(clears))
        if not allc:
            continue
        mean = statistics.mean
        print(f"\n== {label}: {len(recs)} matches, first {HORIZON} rounds; % of champion-rounds (per 100 rounds)")
        print("champion".ljust(16) + "n".rjust(5) + "".join(c.rjust(10) for c in CATS))
        print("ALL".ljust(16) + f"{len(allc):5d}" + "".join(f"{mean(x[c] for x in allc):10.1f}" for c in CATS))
        for name, xs in sorted(per_champ.items(), key=lambda kv: -len(kv[1]))[:8]:
            print(name[:15].ljust(16) + f"{len(xs):5d}" + "".join(f"{mean(x[c] for x in xs):10.1f}" for c in CATS))
        print(f"  camp clears per 100 rounds {mean(clears_per_100):.2f}; champions per clear {mean(sizes) if sizes else 0:.2f}; "
              f"team camp-attack rounds per clear {mean(attack_per_clear) if attack_per_clear else 0:.1f}")


if __name__ == "__main__":
    main()
