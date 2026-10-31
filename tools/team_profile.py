#!/usr/bin/env python3
"""Behaviour profile of one team from its ranked match summaries: drafts, record, economy, items, per-champion play.

Usage: python3 tools/team_profile.py TEAM_ID [--matches 60]   (summaries cached in replays/gap/s)
"""
import argparse
import collections
import json
import pathlib
import statistics
import subprocess
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from gap_report import team_stats  # noqa: E402
from time_breakdown import breakdown, per_round  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent
C = str(ROOT / ".venv/bin/cambc")
S = ROOT / "replays/gap/s"


def cj(*a):
    for wait in (0, 3, 10, 30):
        time.sleep(wait)
        try:
            return json.loads(subprocess.run([C, *a, "--json"], capture_output=True, text=True).stdout)
        except ValueError:
            continue
    raise RuntimeError(f"no answer: {a}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("team")
    ap.add_argument("--matches", type=int, default=60)
    args = ap.parse_args()
    S.mkdir(parents=True, exist_ok=True)
    got, cursor = [], None
    while len(got) < args.matches:
        page = cj("matches", "list", "--team", args.team, "--kind", "ranked", "--limit", "50", *(["--cursor", cursor] if cursor else []))
        got += [m for m in page["matches"] if m["status"] == "finished"]
        cursor = page.get("nextCursor")
        if not cursor or not page["matches"]:
            break
    got = got[:args.matches]
    for m in got:
        f = S / f"{m['matchId']}.json"
        if not f.exists():
            subprocess.run([C, "matches", "summary", m["matchId"], "--json", "-o", str(f)], capture_output=True)
            time.sleep(1)
    rows, champ, items, drafts, rec = [], collections.defaultdict(list), collections.Counter(), collections.Counter(), collections.defaultdict(lambda: [0, 0])
    opp_ranks = []
    ranks = json.load(open(ROOT / "replays/gap/ranks.json"))["ranks"]
    for m in got:
        f = S / f"{m['matchId']}.json"
        if not f.exists():
            continue
        d = json.load(open(f))
        me = next(t for t in m["teams"] if t["teamId"] == args.team)
        th = next(t for t in m["teams"] if t["teamId"] != args.team)
        team = "team" + me["side"].upper()
        s = team_stats(d, team)
        s["won"] = me["result"] == "win"
        per = breakdown(d, team)[0]
        s["dead_pct"] = statistics.mean(c["dead"] for _, c in per.values()) if per else 0.0
        rows.append(s)
        opp_ranks.append(ranks.get(th["teamId"], 999))
        drafts[tuple(sorted(s["draft"]))] += 1
        rec[s["size"]][0 if s["won"] else 1] += 1
        n = d["match"]["roundsCompleted"]
        tl = {(t["owner"].get("id"), t["metric"]): t["runs"] for t in d["championTimelines"]}
        for c in d["champions"]:
            if c["team"] != team:
                continue
            x = {k: float(c["metrics"][k]) for k in ("earnedGold", "spentGold", "championDamageDone", "championDamageTaken", "monsterDamageDone",
                                                       "kills", "deaths", "assists", "collectedGems", "submittedGems", "shieldingDone", "healingDone")}
            g = per_round(tl.get((c["id"], "earnedGold"), []), n) if (c["id"], "earnedGold") in tl else [0.0] * n
            x["gold_r250"], x["gold_r500"] = g[min(249, n - 1)], g[min(499, n - 1)]
            x["n"] = n
            x["items"] = [(i["item"], int(i["quantity"])) for i in c["itemsBought"]]
            x["consumables"] = [(i["consumable"], int(i["quantity"])) for i in c.get("consumablesBought", [])]
            champ[c["name"]].append(x)
            for it, q in x["items"]:
                items[(c["name"], it)] += q
    mean = statistics.mean
    k = lambda key: mean(r[key] for r in rows)
    print(f"{len(rows)} ranked matches; record {sum(r['won'] for r in rows)}-{sum(not r['won'] for r in rows)}; "
          f"opponents' median rank {statistics.median(opp_ranks):.0f}")
    print("  by team size:", {sz: f"{w}-{l}" for sz, (w, l) in sorted(rec.items())})
    print(f"  per game: rounds {k('rounds'):.0f}, gems by r1000 {k('gems_r1000'):.0f}, first tribute r{k('t_first'):.0f}, tributes {k('tributes'):.1f} "
          f"(avg {k('avg_tribute'):.0f} gems), camps/100 rounds {mean(r['camps'] / r['rounds'] * 100 for r in rows):.2f}, kills {k('kills'):.1f}, "
          f"deaths {k('deaths'):.1f}, death time {k('dead_pct'):.1f}%, wards {k('wards'):.0f}")
    print("  drafts:", drafts.most_common(6))
    print(f"\n{'champion':14} {'games':>5} {'gold/100r':>9} {'gold@250':>8} {'gold@500':>8} {'spent':>6} {'cdmg/100r':>9} {'taken/100r':>10} {'mdmg/100r':>9} "
          f"{'kills':>5} {'deaths':>6} {'assists':>7} {'tributed':>8} {'shield':>6} {'heal':>6}")
    for name, xs in sorted(champ.items(), key=lambda kv: -len(kv[1])):
        r = lambda key: mean(x[key] / x["n"] * 100 for x in xs)
        a = lambda key: mean(x[key] for x in xs)
        print(f"{name:14} {len(xs):5d} {r('earnedGold'):9.0f} {a('gold_r250'):8.0f} {a('gold_r500'):8.0f} {a('spentGold'):6.0f} {r('championDamageDone'):9.0f} "
              f"{r('championDamageTaken'):10.0f} {r('monsterDamageDone'):9.0f} {a('kills'):5.1f} {a('deaths'):6.1f} {a('assists'):7.1f} {a('submittedGems'):8.0f} "
              f"{a('shieldingDone'):6.0f} {a('healingDone'):6.0f}")
    print("\nItems per game, by champion (top 6 each):")
    for name, xs in sorted(champ.items(), key=lambda kv: -len(kv[1]))[:6]:
        mine = sorted(((it, q / len(xs)) for (cn, it), q in items.items() if cn == name), key=lambda x: -x[1])[:6]
        cons = collections.Counter()
        for x in xs:
            for c, q in x["consumables"]:
                cons[c] += q
        print(f"  {name}: " + ", ".join(f"{it} {q:.1f}" for it, q in mine) + " | consumables: " + ", ".join(f"{c} {q / len(xs):.1f}" for c, q in cons.most_common(3)))


if __name__ == "__main__":
    main()
