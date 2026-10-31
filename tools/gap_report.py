#!/usr/bin/env python3
"""Gap analysis from tools/gap_fetch.py data: our ranked losses vs much higher-ranked teams, and the top teams'
own matches, compared per team-match and per champion.

Usage: python3 tools/gap_report.py [--above 150]   (opponents ranked <= this count as "well above us")
"""
import argparse
import collections
import json
import pathlib
import statistics

ROOT = pathlib.Path(__file__).resolve().parent.parent
G = ROOT / "replays/gap"
METRICS = ["result", "size", "rounds", "gold_lt1000", "gold_ge1000", "spent", "items", "collected", "tributed", "tributes",
           "avg_tribute", "gems_lost", "deaths", "deaths_by_champ", "kills", "fights_won", "wards", "camps", "fuel_out", "t_first", "gems_r1000"]


def at_round(runs, r):
    t, v = 0, 0.0
    for run in runs:
        if t > r:
            break
        v = float(run["value"])
        t += int(run["duration"])
    return v


def team_stats(d, team):
    champs = [c for c in d["champions"] if c["team"] == team]
    ev = collections.defaultdict(list)
    for e in d["events"]:
        if e["owner"].get("team") == team:
            ev[e["kind"]] += e["entries"]
    tl = {t["metric"]: t["runs"] for t in d["teamTimelines"] if t["owner"] == team}
    earned = [t["runs"] for t in d["championTimelines"] if t["owner"].get("team") == team and t["metric"] == "earnedGold"]
    m = lambda k: sum(float(c["metrics"][k]) for c in champs)
    g1000 = sum(at_round(r, 999) for r in earned)
    subs = ev["gemSubmission"]
    tributed = sum(float(x["value"]) for x in subs)
    deaths_c = sum(1 for x in ev["death"] if x["context"] == "champion")
    rounds = d["match"]["roundsCompleted"]
    return {
        "size": len(champs), "rounds": rounds,
        "gold_lt1000": g1000, "gold_ge1000": m("earnedGold") - g1000, "spent": m("spentGold"),
        "items": sum(int(i["quantity"]) for c in champs for i in c["itemsBought"]),
        "collected": m("collectedGems"), "tributed": tributed, "tributes": len(subs),
        "avg_tribute": tributed / len(subs) if subs else 0.0,
        "gems_lost": sum(float(x["value"]) for x in ev["gemLoss"]),
        "deaths": m("deaths"), "deaths_by_champ": deaths_c, "kills": m("kills"),
        "fights_won": m("kills") / max(1.0, m("kills") + deaths_c),
        "wards": len(ev["wardPlacement"]), "camps": len(ev["campClear"]), "fuel_out": len(ev["fuelExhausted"]),
        "t_first": min((x["timestamp"]["round"] for x in subs), default=rounds),
        "gems_r1000": at_round(tl.get("submittedGems", []), 999),
        "draft": sorted(c["name"] for c in champs),
        "per_champ": {c["name"]: {k: float(c["metrics"][k]) for k in ("collectedGems", "submittedGems", "deaths", "kills", "earnedGold")}
                      | {"wards": sum(1 for e in d["events"] if e["kind"] == "wardPlacement" and e["owner"].get("id") == c["id"] for _ in e["entries"]),
                         "items": [i["item"] for i in c["itemsBought"]]} for c in champs},
    }


def rows(index, ranks, me_id):
    """(match, us_or_top, them) team-stat pairs. side 'a' = summary 'teamA'."""
    out = {"loss_vs_above": [], "ours_all": [], "top": []}
    for m in index["ours"]:
        f = G / "s" / f"{m['matchId']}.json"
        if not f.exists():
            continue
        d = json.load(open(f))
        us = next(t for t in m["teams"] if t["teamId"] == me_id)
        th = next(t for t in m["teams"] if t["teamId"] != me_id)
        a, b = team_stats(d, "team" + us["side"].upper()), team_stats(d, "team" + th["side"].upper())
        a["result"], b["result"] = float(us["result"] == "win"), float(th["result"] == "win")
        rec = (m, a, b, ranks.get(th["teamId"], 999), th["name"])
        out["ours_all"].append(rec)
        if us["result"] == "loss":
            out["loss_vs_above"].append(rec)
    for tid, ms in index["top"].items():
        for m in ms:
            f = G / "s" / f"{m['matchId']}.json"
            if not f.exists():
                continue
            d = json.load(open(f))
            me = next(t for t in m["teams"] if t["teamId"] == tid)
            th = next(t for t in m["teams"] if t["teamId"] != tid)
            a, b = team_stats(d, "team" + me["side"].upper()), team_stats(d, "team" + th["side"].upper())
            a["result"], b["result"] = float(me["result"] == "win"), float(th["result"] == "win")
            out["top"].append((m, a, b, ranks.get(th["teamId"], 999), me["name"]))
    return out


def mean(xs):
    return statistics.mean(xs) if xs else float("nan")


def table(title, groups):
    print(f"\n== {title}")
    print("metric".ljust(16) + "".join(name.rjust(14) for name, _ in groups))
    for k in METRICS:
        print(k.ljust(16) + "".join(f"{mean([s[k] for s in g]):14.2f}" for _, g in groups))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--above", type=int, default=150)
    args = ap.parse_args()
    R = json.load(open(G / "ranks.json"))
    index = json.load(open(G / "index.json"))
    top_ids = {t["teamId"] for t in R["top"] if not t["isDev"]}
    data = rows(index, R["ranks"], R["me"]["id"])
    above = [r for r in data["loss_vs_above"] if r[3] <= args.above]
    top = [r for r in data["top"] if r[0]["teams"][0]["teamId"] in top_ids or r[0]["teams"][1]["teamId"] in top_ids]
    print(f"our rank {R['my_rank']}; our ranked matches {len(data['ours_all'])} "
          f"({sum(r[1]['result'] for r in data['ours_all']):.0f} wins); losses vs rank <= {args.above}: {len(above)}; top-team matches {len(top)}")
    for size in (3, 4, 5):
        ours = [r for r in data["ours_all"] if r[1]["size"] == size]
        if ours:
            print(f"  our record, {size} champions: {sum(r[1]['result'] for r in ours):.0f}-{len(ours) - sum(r[1]['result'] for r in ours):.0f}")
    table("Per team-match: our losses vs teams ranked well above us",
          [("us", [r[1] for r in above]), ("them", [r[2] for r in above])])
    table("Per team-match: all our matches vs top teams in all their matches",
          [("us (all)", [r[1] for r in data["ours_all"]]), ("top teams", [r[1] for r in top]), ("top's opps", [r[2] for r in top])])
    for label, recs in (("us", [r[1] for r in data["ours_all"]]), ("top teams", [r[1] for r in top])):
        per = collections.defaultdict(list)
        for s in recs:
            for name, c in s["per_champ"].items():
                per[name].append(c)
        print(f"\n== Per champion: {label} (n = champion-matches)")
        print("champion".ljust(24) + "n".rjust(5) + "collected".rjust(11) + "tributed".rjust(10) + "deaths".rjust(8) + "kills".rjust(7) + "gold".rjust(8) + "wards".rjust(7))
        for name, cs in sorted(per.items(), key=lambda kv: -len(kv[1]))[:12]:
            print(name[:23].ljust(24) + f"{len(cs):5d}" + "".join(f"{mean([c[k] for c in cs]):{w}.1f}" for k, w in
                  (("collectedGems", 11), ("submittedGems", 10), ("deaths", 8), ("kills", 7), ("earnedGold", 8), ("wards", 7))))
    drafts = collections.Counter(tuple(r[1]["draft"]) for r in top)
    print("\n== Top teams' most used drafts:", drafts.most_common(6))
    items = collections.Counter(i for r in top for c in r[1]["per_champ"].values() for i in c["items"])
    our_items = collections.Counter(i for r in data["ours_all"] for c in r[1]["per_champ"].values() for i in c["items"])
    print("== Top teams' items (kinds bought, per team-match):", [(k, round(v / max(1, len(top)), 2)) for k, v in items.most_common(10)])
    print("== Our items (per team-match):", [(k, round(v / max(1, len(data['ours_all'])), 2)) for k, v in our_items.most_common(10)])
    print("\n== Example losses vs well-above teams (biggest tribute gap):")
    for m, a, b, rk, name in sorted(above, key=lambda r: r[1]["tributed"] - r[2]["tributed"])[:6]:
        print(f"  {m['matchId']}  {m['map']:22} vs #{rk} {name[:20]:20} size {a['size']}  tributed {a['tributed']:.0f}-{b['tributed']:.0f}"
              f"  deaths {a['deaths']:.0f}-{b['deaths']:.0f}  kills {a['kills']:.0f}-{b['kills']:.0f}  gold<1000 {a['gold_lt1000']:.0f}-{b['gold_lt1000']:.0f}")


if __name__ == "__main__":
    main()
