#!/usr/bin/env python3
"""Top-10 study: for each top non-dev team, its recent ranked matches -> a per-team profile (drafts, economy curves, items,
fights, tribute cadence) and what differs between its wins and its losses (who beats it, where, how). Writes a markdown
report to replays/top10/report.md and the raw per-team numbers to replays/top10/teams.json.

Usage: python3 tools/top10.py [--teams 10] [--matches 40]
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
OUT = ROOT / "replays/top10"


def cj(*a):
    for wait in (0, 3, 10, 30, 60):
        time.sleep(wait)
        try:
            return json.loads(subprocess.run([C, *a, "--json"], capture_output=True, text=True).stdout)
        except ValueError:
            continue
    raise RuntimeError(f"no answer: {a}")


def listing(team, n):
    got, cursor = [], None
    while len(got) < n:
        page = cj("matches", "list", "--team", team, "--kind", "ranked", "--limit", "50", *(["--cursor", cursor] if cursor else []))
        got += [m for m in page["matches"] if m["status"] == "finished"]
        cursor = page.get("nextCursor")
        if not cursor or not page["matches"]:
            break
    return got[:n]


def fetch(mid):
    f = S / f"{mid}.json"
    for wait in (0, 5, 20):
        if f.exists():
            return True
        time.sleep(wait)
        subprocess.run([C, "matches", "summary", mid, "--json", "-o", str(f)], capture_output=True)
    return f.exists()


def series(d, cid, metric, n):
    for t in d["championTimelines"]:
        if t["owner"].get("id") == cid and t["metric"] == metric:
            return per_round(t["runs"], n)
    return [0.0] * n


def team_series(d, team, metric, n):
    for t in d["teamTimelines"]:
        if t["owner"] == team and t["metric"] == metric:
            return per_round(t["runs"], n)
    return [0.0] * n


def game_record(d, team):
    """Everything we keep about one team in one game."""
    n = d["match"]["roundsCompleted"]
    s = team_stats(d, team)
    per = breakdown(d, team)[0]
    s["dead_pct"] = statistics.mean(c["dead"] for _, c in per.values()) if per else 0.0
    sub = team_series(d, team, "submittedGems", n)
    s["gems_at"] = {r: sub[min(r, n - 1)] for r in (250, 500, 750, 1000)}
    champs = []
    for c in d["champions"]:
        if c["team"] != team:
            continue
        g = series(d, c["id"], "earnedGold", n)
        spent = series(d, c["id"], "spentGold", n)
        first_buy = next((i for i, v in enumerate(spent) if v > 0), n)
        m = {k: float(c["metrics"][k]) for k in c["metrics"]}
        champs.append({"name": c["name"], "n": n, "gold_at": {r: g[min(r, n - 1)] for r in (250, 500, 1000)}, "first_buy": first_buy,
                       "items": [(i["item"], int(i["quantity"])) for i in c["itemsBought"]],
                       "consumables": [(i["consumable"], int(i["quantity"])) for i in c.get("consumablesBought", [])], **m})
    s["champs"] = champs
    # kills and deaths by round bucket; tribute cadence
    ev = collections.defaultdict(list)
    for e in d["events"]:
        if e["owner"].get("team") == team:
            ev[e["kind"]] += e["entries"]
    s["kill_rounds"] = [x["timestamp"]["round"] for x in ev["kill"]]
    s["death_rounds"] = [x["timestamp"]["round"] for x in ev["death"]]
    s["tribute_rounds"] = sorted(x["timestamp"]["round"] for x in ev["gemSubmission"])
    s["locks"] = sum(int(q) for c in champs for k, q in c["consumables"] if k == "Altar Lock")
    s["teleports"] = sum(int(q) for c in champs for k, q in c["consumables"] if k == "Teleport Crystal")
    s["camp_clears_early"] = sum(1 for x in ev["campClear"] if x["timestamp"]["round"] < 300)
    s["rounds_total"] = n
    return s


def mean(xs):
    xs = list(xs)
    return statistics.mean(xs) if xs else float("nan")


def summarize(games):
    k = lambda f: mean(f(g) for g in games)
    return {
        "games": len(games),
        "win_rate": k(lambda g: g["won"]),
        "rounds": k(lambda g: g["rounds_total"]),
        "gems_250": k(lambda g: g["gems_at"][250]), "gems_500": k(lambda g: g["gems_at"][500]),
        "gems_750": k(lambda g: g["gems_at"][750]), "gems_1000": k(lambda g: g["gems_at"][1000]),
        "first_tribute": k(lambda g: g["t_first"]), "tributes": k(lambda g: g["tributes"]), "avg_tribute": k(lambda g: g["avg_tribute"]),
        "camps_100": k(lambda g: g["camps"] / g["rounds"] * 100), "early_clears": k(lambda g: g["camp_clears_early"]),
        "kills": k(lambda g: g["kills"]), "deaths": k(lambda g: g["deaths"]), "dead_pct": k(lambda g: g["dead_pct"]),
        "gems_lost": k(lambda g: g["gems_lost"]), "wards": k(lambda g: g["wards"]), "locks": k(lambda g: g["locks"]),
        "teleports": k(lambda g: g["teleports"]),
        "gold_100": k(lambda g: sum(c["earnedGold"] for c in g["champs"]) / max(1, len(g["champs"])) / g["rounds_total"] * 100),
        "dmg_ratio": sum(sum(c["championDamageDone"] for c in g["champs"]) for g in games) /
                     max(1.0, sum(sum(c["championDamageTaken"] for c in g["champs"]) for g in games)),
        "early_kills": k(lambda g: sum(r < 300 for r in g["kill_rounds"])), "early_deaths": k(lambda g: sum(r < 300 for r in g["death_rounds"])),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--teams", type=int, default=10)
    ap.add_argument("--matches", type=int, default=40)
    args = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    S.mkdir(parents=True, exist_ok=True)
    ladder = cj("ladder", "--limit", "50")["teams"]
    top = [t for t in ladder if not t["isDev"]][:args.teams]
    ranks = {t["teamId"]: t["rank"] for p in (1, 2, 3, 4) for t in cj("ladder", "--page", str(p), "--limit", "50")["teams"]}
    names = {}
    data = {}
    for t in top:
        tid = t["teamId"]
        names[tid] = t["name"]
        ms = listing(tid, args.matches)
        games = []
        for m in ms:
            if not fetch(m["matchId"]):
                continue
            d = json.load(open(S / f"{m['matchId']}.json"))
            me = next(x for x in m["teams"] if x["teamId"] == tid)
            th = next(x for x in m["teams"] if x["teamId"] != tid)
            g = game_record(d, "team" + me["side"].upper())
            g["won"] = me["result"] == "win"
            g["map"] = m["map"]
            g["opp"] = th["name"]
            g["opp_rank"] = ranks.get(th["teamId"], 999)
            g["opp_draft"] = sorted(c["name"] for c in d["champions"] if c["team"] != "team" + me["side"].upper())
            g["match"] = m["matchId"]
            g["end"] = m.get("endReason")
            games.append(g)
        data[tid] = {"name": t["name"], "rank": t["rank"], "rating": t["rating"], "games": games}
        print(f"#{t['rank']} {t['name']}: {len(games)} games", flush=True)
    json.dump(data, open(OUT / "teams.json", "w"))
    write_report(data)


def write_report(data):
    L = []
    L.append("# Top-10 study (ranked matches)\n")
    L.append("Per team: recent ranked games (opponents are mostly other top teams). Gold = earned per champion per 100 rounds. "
             "Damage ratio = champion damage dealt / taken. Early = before round 300.\n")
    rows = []
    for tid, t in data.items():
        s = summarize(t["games"])
        rows.append((t, s))
    L.append("## Overview\n")
    L.append("| Rank | Team | Games | Win % | Gems r250 / r500 / r1000 | First tribute | Tributes × size | Camps / 100r | Gold / 100r | Kills / deaths | Death time | Damage ratio | Wards | Locks / teleports |")
    L.append("| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |")
    for t, s in rows:
        L.append(f"| {t['rank']} | {t['name']} | {s['games']} | {100 * s['win_rate']:.0f} | {s['gems_250']:.0f} / {s['gems_500']:.0f} / {s['gems_1000']:.0f} | "
                 f"r{s['first_tribute']:.0f} | {s['tributes']:.0f} × {s['avg_tribute']:.0f} | {s['camps_100']:.2f} | {s['gold_100']:.0f} | "
                 f"{s['kills']:.1f} / {s['deaths']:.1f} | {s['dead_pct']:.1f}% | {s['dmg_ratio']:.2f} | {s['wards']:.0f} | {s['locks']:.1f} / {s['teleports']:.1f} |")
    for t, s in rows:
        games = t["games"]
        wins = [g for g in games if g["won"]]
        losses = [g for g in games if not g["won"]]
        L.append(f"\n## #{t['rank']} {t['name']}\n")
        drafts = collections.Counter(tuple(sorted(g["draft"])) for g in games)
        L.append("**Drafts:** " + "; ".join(f"{' + '.join(d)} ({n})" for d, n in drafts.most_common(4)) + "\n")
        by_size = collections.defaultdict(lambda: [0, 0])
        for g in games:
            by_size[len(g["draft"])][0 if g["won"] else 1] += 1
        L.append("**Record by bodies:** " + ", ".join(f"{k}: {w}–{l}" for k, (w, l) in sorted(by_size.items())) + "\n")
        # champions
        per = collections.defaultdict(list)
        for g in games:
            for c in g["champs"]:
                per[c["name"]].append(c)
        L.append("| Champion | Games | Gold / 100r | Gold r250 / r500 | First buy | Champion dmg / 100r | Taken / 100r | Monster dmg / 100r | K / D / A | Gems tributed | Shield | Heal | Top items |")
        L.append("| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |")
        for name, cs in sorted(per.items(), key=lambda kv: -len(kv[1])):
            r = lambda k: mean(c[k] / c["n"] * 100 for c in cs)
            a = lambda k: mean(c[k] for c in cs)
            items = collections.Counter()
            for c in cs:
                for it, q in c["items"]:
                    items[it] += q
            top_items = ", ".join(f"{it} {q / len(cs):.1f}" for it, q in items.most_common(4))
            L.append(f"| {name} | {len(cs)} | {r('earnedGold'):.0f} | {mean(c['gold_at'][250] for c in cs):.0f} / {mean(c['gold_at'][500] for c in cs):.0f} | "
                     f"r{a('first_buy'):.0f} | {r('championDamageDone'):.0f} | {r('championDamageTaken'):.0f} | {r('monsterDamageDone'):.0f} | "
                     f"{a('kills'):.1f} / {a('deaths'):.1f} / {a('assists'):.1f} | {a('submittedGems'):.0f} | {a('shieldingDone'):.0f} | {a('healingDone'):.0f} | {top_items} |")
        # wins vs losses
        if losses:
            sw, sl = summarize(wins), summarize(losses)
            L.append(f"\n**Wins vs losses** ({len(wins)}–{len(losses)}):\n")
            L.append("| | Gems r250 / r500 / r1000 | First tribute | Camps / 100r | Gold / 100r | Kills / deaths | Early kills / deaths | Death time | Damage ratio | Gems lost |")
            L.append("| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |")
            for lab, x in (("Wins", sw), ("Losses", sl)):
                L.append(f"| {lab} | {x['gems_250']:.0f} / {x['gems_500']:.0f} / {x['gems_1000']:.0f} | r{x['first_tribute']:.0f} | {x['camps_100']:.2f} | {x['gold_100']:.0f} | "
                         f"{x['kills']:.1f} / {x['deaths']:.1f} | {x['early_kills']:.1f} / {x['early_deaths']:.1f} | {x['dead_pct']:.1f}% | {x['dmg_ratio']:.2f} | {x['gems_lost']:.0f} |")
            beat_by = collections.Counter(g["opp"] for g in losses)
            maps = collections.Counter(g["map"] for g in losses)
            opp_drafts = collections.Counter(tuple(g["opp_draft"]) for g in losses)
            L.append(f"\n**Lost to:** " + ", ".join(f"{o} ({n})" for o, n in beat_by.most_common(5)))
            L.append(f"\n**Lost on maps:** " + ", ".join(f"{m} ({n})" for m, n in maps.most_common(6)))
            L.append(f"\n**Opponent drafts in losses:** " + "; ".join(f"{' + '.join(d)} ({n})" for d, n in opp_drafts.most_common(3)) + "\n")
    (OUT / "report.md").write_text("\n".join(L) + "\n")
    print(f"report: {OUT / 'report.md'}")


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--report-only":
        write_report(json.load(open(OUT / "teams.json")))
    else:
        main()
