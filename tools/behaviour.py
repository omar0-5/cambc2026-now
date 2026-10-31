#!/usr/bin/env python3
"""Behaviour from decoded replays (tools/replay_decode.mjs): how a team plays, per champion.

Groups: each top-10 team (replays/top10/teams.json) and us (scrim plan files). For each group and champion type:
ability casts per 100 rounds by slot, attacks on champions vs camps, recalls / shop visits / wards / locks / teleports,
tribute bag size, and fights: health % when a fight starts, allies and enemies within 5 tiles, and how it ended.

Usage: python3 tools/behaviour.py [--us PLAN.json ...] [--top]   -> replays/top10/behaviour.md
"""
import argparse
import collections
import json
import pathlib
import statistics
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

ROOT = pathlib.Path(__file__).resolve().parent.parent
DEC = ROOT / "replays/decoded"
TYPES = ["Ryth", "Vesper Vale", "Vendetta", "Cassi", "Issac", "Stellar", "Prism", "Marcella", "Willow", "Ren", "Varg", "Tavi", "Heart of Ruin"]
ABIL = ["None", "Attack reset", "Damage", "AoE damage", "CC", "Heal", "Dash", "Shield"]
MOVE, ATTACK, CAST, RECALL, SUBMIT, SHOP, WARD, CONSUMABLE, LOCK, TELEPORT = 4, 5, 6, 11, 12, 13, 14, 15, 20, 21
NEAR = 5
FIGHT_GAP = 10   # rounds without a champion attack before a new fight starts


def cheb(a, b):
    return max(abs(a[0] - b[0]), abs(a[1] - b[1]))


def analyse(d, team):
    """Per champion of `team` (engine team 0 = side a, 1 = side b) in one decoded game."""
    champs = {c["index"]: c for c in d["champions"]}
    mine = {i for i, c in champs.items() if c["team"] == team}
    frames = {int(r): {row[0]: row for row in rows} for r, rows in d["frames"].items()}
    rounds = max(frames) if frames else 1
    per = {i: collections.Counter() for i in mine}
    fights = []
    deaths = []
    tribute_bags = []
    last_attack = collections.defaultdict(lambda: -1000)
    for a in d["actions"]:
        r, kind, actor = a[0], a[1], a[2]
        if actor not in mine:
            # attacks on our champions start fights too
            if kind == ATTACK:
                f = frames.get(r)
                if f:
                    for i in mine:
                        row = f.get(i)
                        if row and row[5] and (row[1], row[2]) == (a[7], a[8]):
                            if r - last_attack[i] > FIGHT_GAP:
                                fights.append(fight_record(frames, champs, i, r, attacked_first=True))
                            last_attack[i] = r
            continue
        c = per[actor]
        if kind == CAST:
            c[f"cast_s{a[3]}_{ABIL[a[4]] if 0 <= a[4] < len(ABIL) else a[4]}"] += 1
        elif kind == ATTACK:
            f = frames.get(r)
            target_champ = False
            if f:
                for j, row in f.items():
                    if champs[j]["team"] != team and row[5] and (row[1], row[2]) == (a[7], a[8]):
                        target_champ = True
            c["attack_champion" if target_champ else "attack_camp"] += 1
            if target_champ:
                if r - last_attack[actor] > FIGHT_GAP:
                    fights.append(fight_record(frames, champs, actor, r, attacked_first=False))
                last_attack[actor] = r
        elif kind == SUBMIT:
            c["tributes"] += 1
            row = frames.get(r, {}).get(actor)
            if row:
                tribute_bags.append(row[6])
        elif kind in (RECALL, SHOP, WARD, CONSUMABLE, LOCK, TELEPORT):
            c[{RECALL: "recall", SHOP: "shop", WARD: "ward", CONSUMABLE: "consumable", LOCK: "lock", TELEPORT: "teleport"}[kind]] += 1
        elif kind == MOVE:
            c["move"] += 1
    # deaths: alive 1 -> 0 between frames
    for i in mine:
        prev = None
        for r in sorted(frames):
            row = frames[r].get(i)
            if not row:
                continue
            if prev and prev[5] and not row[5]:
                pos = (prev[1], prev[2])
                allies = sum(1 for j, x in frames[r - 1].items() if j != i and j in mine and x[5] and cheb(pos, (x[1], x[2])) <= NEAR) if (r - 1) in frames else 0
                enemies = sum(1 for j, x in frames[r - 1].items() if j not in mine and x[5] and cheb(pos, (x[1], x[2])) <= NEAR) if (r - 1) in frames else 0
                h10 = frames.get(r - 10, {}).get(i)
                deaths.append({"type": champs[i]["type"], "round": r, "allies": allies, "enemies": enemies, "gems": prev[6],
                               "hp_pct_10_before": (h10[3] / h10[4]) if h10 and h10[4] else None})
            prev = row
    return {"per": {champs[i]["type"]: per[i] for i in mine}, "rounds": rounds, "fights": fights, "deaths": deaths, "bags": tribute_bags}


def fight_record(frames, champs, i, r, attacked_first):
    f = frames.get(r, {})
    me = f.get(i)
    team = champs[i]["team"]
    pos = (me[1], me[2]) if me else (0, 0)
    allies = sum(1 for j, x in f.items() if j != i and champs[j]["team"] == team and x[5] and cheb(pos, (x[1], x[2])) <= NEAR)
    enemies = sum(1 for j, x in f.items() if champs[j]["team"] != team and x[5] and cheb(pos, (x[1], x[2])) <= NEAR)
    died = any(not frames.get(r + k, {}).get(i, [0, 0, 0, 0, 0, 1])[5] for k in range(1, 11))
    return {"type": champs[i]["type"], "round": r, "hp_pct": (me[3] / me[4]) if me and me[4] else None, "allies": allies, "enemies": enemies,
            "attacked_first": attacked_first, "died_within_10": died}


def collect(games):
    """games: list of (decoded dict, engine team)."""
    agg = collections.defaultdict(lambda: collections.Counter())
    rounds = collections.Counter()
    fights, deaths, bags = [], [], []
    for d, team in games:
        a = analyse(d, team)
        for t, c in a["per"].items():
            agg[t].update(c)
            rounds[t] += a["rounds"]
        fights += a["fights"]
        deaths += a["deaths"]
        bags += a["bags"]
    return agg, rounds, fights, deaths, bags


def section(title, games, L):
    agg, rounds, fights, deaths, bags = collect(games)
    L.append(f"\n## {title} ({len(games)} games)\n")
    L.append("| Champion | Rounds | Casts / 100r by slot (ability) | Attacks on champions / camps per 100r | Recall | Shop | Ward | Lock | Teleport | per 100 rounds |")
    L.append("| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |")
    for t, c in sorted(agg.items(), key=lambda kv: -rounds[kv[0]]):
        k = 100.0 / max(1, rounds[t])
        casts = ", ".join(f"{key[5:]} {v * k:.1f}" for key, v in sorted(c.items()) if key.startswith("cast_"))
        L.append(f"| {TYPES[t]} | {rounds[t]} | {casts} | {c['attack_champion'] * k:.1f} / {c['attack_camp'] * k:.1f} | {c['recall'] * k:.2f} | "
                 f"{c['shop'] * k:.2f} | {c['ward'] * k:.1f} | {c['lock'] * k:.2f} | {c['teleport'] * k:.2f} | |")
    if bags:
        L.append(f"\nTribute bag: mean {statistics.mean(bags):.0f} gems, median {statistics.median(bags):.0f} ({len(bags)} tributes)")
    if fights:
        hp = [f["hp_pct"] for f in fights if f["hp_pct"] is not None]
        start = collections.Counter("started" if not f["attacked_first"] else "attacked first" for f in fights)
        L.append(f"\nFights: {len(fights)} ({start['started']} we started, {start['attacked first']} they started). Health at start: mean "
                 f"{100 * statistics.mean(hp):.0f}%, below 50% in {100 * sum(h < 0.5 for h in hp) / len(hp):.0f}%. "
                 f"Died within 10 rounds: {100 * sum(f['died_within_10'] for f in fights) / len(fights):.0f}%.")
        by_num = collections.defaultdict(lambda: [0, 0])
        for f in fights:
            key = f"{min(f['allies'] + 1, 4)}v{min(f['enemies'], 4)}"
            by_num[key][0] += 1
            by_num[key][1] += f["died_within_10"]
        L.append("Numbers within 5 tiles at fight start (us v them): " +
                 ", ".join(f"{k} {n} (died {100 * dd / n:.0f}%)" for k, (n, dd) in sorted(by_num.items(), key=lambda kv: -kv[1][0])[:8]))
    if deaths:
        alone = sum(1 for x in deaths if x["allies"] == 0)
        hp10 = [x["hp_pct_10_before"] for x in deaths if x["hp_pct_10_before"] is not None]
        L.append(f"\nDeaths: {len(deaths)}; no ally within 5 tiles {100 * alone / len(deaths):.0f}%; enemies within 5 tiles mean "
                 f"{statistics.mean(x['enemies'] for x in deaths):.1f}; health 10 rounds before death mean {100 * statistics.mean(hp10):.0f}%; "
                 f"gems carried mean {statistics.mean(x['gems'] for x in deaths):.1f}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--us", nargs="*", default=[])
    ap.add_argument("--top", action="store_true")
    args = ap.parse_args()
    L = ["# Behaviour from decoded replays\n",
         "Casts by ability slot and type; fights start at the first champion-on-champion attack after 10 quiet rounds; numbers are "
         "living champions within 5 tiles; deaths are alive→dead between round frames.\n"]
    if args.us:
        from scrim_report import show_group
        me = json.load(open(ROOT / "replays/gap/ranks.json"))["me"]["id"]
        games = []
        for plan in args.us:
            for _t, _m, gid in json.load(open(plan)):
                for m in show_group(gid)["matches"]:
                    f = DEC / f"{m['matchId']}.json"
                    if f.exists():
                        side = next(t for t in m["teams"] if t["teamId"] == me)["side"]
                        games.append((json.load(open(f)), 0 if side == "a" else 1))
        section("Us (FoolMeOrMe.com) in the given scrims", games, L)
    if args.top:
        teams = json.load(open(ROOT / "replays/top10/teams.json"))
        for tid, t in teams.items():
            games = []
            for g in t["games"]:
                f = DEC / f"{g['match']}.json"
                if not f.exists():
                    continue
                d = json.load(open(f))
                # which engine team is this team: match its draft (sorted names) against each side's champions
                sides = {s: sorted(TYPES[c["type"]] for c in d["champions"] if c["team"] == s) for s in (0, 1)}
                want = sorted(g["draft"])
                hit = [s for s in (0, 1) if sides[s] == want]
                if len(hit) == 1:
                    games.append((d, hit[0]))
            if games:
                section(f"#{t['rank']} {t['name']}", games, L)
    out = ROOT / "replays/top10/behaviour.md"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(L) + "\n")
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
