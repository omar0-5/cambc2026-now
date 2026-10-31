#!/usr/bin/env python3
"""Focused behaviour report from decoded replays: (1) fight entry and exit rules, (2) Marcella over a game, (3) deaths.
Compares groups of games (e.g. a top team vs our scrims).

Usage: python3 tools/focus.py LABEL=GAMES.json [LABEL=GAMES.json ...]
  GAMES.json: [[matchId, engineTeam (0 = side a, 1 = side b)], ...]; decoded files in replays/decoded/<matchId>.json
"""
import collections
import json
import pathlib
import statistics
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
DEC = ROOT / "replays/decoded"
TYPES = ["Ryth", "Vesper Vale", "Vendetta", "Cassi", "Issac", "Stellar", "Prism", "Marcella", "Willow", "Ren", "Varg", "Tavi", "Heart of Ruin"]
MOVE, ATTACK, CAST, RECALL, SUBMIT, SHOP = 4, 5, 6, 11, 12, 13
QUIET = 10
NEAR = 5


def cheb(a, b):
    return max(abs(a[0] - b[0]), abs(a[1] - b[1]))


def q(xs, p):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(p * len(xs)))] if xs else float("nan")


class Game:
    def __init__(self, d, team):
        self.team = team
        self.champs = {c["index"]: c for c in d["champions"]}
        self.mine = {i for i, c in self.champs.items() if c["team"] == team}
        self.frames = {int(r): {row[0]: row for row in rows} for r, rows in d["frames"].items()}
        self.last = max(self.frames) if self.frames else 0
        self.actions = d["actions"]
        self.spawn = {}
        for r in sorted(self.frames)[:3]:
            for i, row in self.frames[r].items():
                self.spawn.setdefault(i, (row[1], row[2]))
        # champion-on-champion hits: (round, attacker, victim)
        self.hits = []
        for a in self.actions:
            if a[1] != ATTACK:
                continue
            f = self.frames.get(a[0], {})
            for j, row in f.items():
                if row[5] and (row[1], row[2]) == (a[7], a[8]) and self.champs[j]["team"] != self.champs.get(a[2], {}).get("team"):
                    self.hits.append((a[0], a[2], j))
        self.death_rounds = collections.defaultdict(list)
        for i in self.champs:
            prev = None
            for r in sorted(self.frames):
                row = self.frames[r].get(i)
                if row is None:
                    continue
                if prev and prev[5] and not row[5]:
                    self.death_rounds[i].append(r)
                prev = row

    def row(self, i, r):
        for k in (0, -1, 1, -2, 2):
            x = self.frames.get(r + k, {}).get(i)
            if x:
                return x
        return None

    def context(self, i, r):
        me = self.row(i, r)
        if not me:
            return None
        pos = (me[1], me[2])
        f = self.frames.get(r, {})
        allies = [x for j, x in f.items() if j != i and self.champs[j]["team"] == self.team and x[5]]
        enemies = [x for j, x in f.items() if self.champs[j]["team"] != self.team and x[5]]
        near_ally = min((cheb(pos, (x[1], x[2])) for x in allies), default=99)
        return {"hp": me[3] / me[4] if me[4] else 0, "allies": sum(cheb(pos, (x[1], x[2])) <= NEAR for x in allies),
                "enemies": sum(cheb(pos, (x[1], x[2])) <= NEAR for x in enemies), "near_ally": near_ally, "gems": me[6]}

    def fights(self):
        """Per champion of ours: episodes of champion-on-champion hits (dealt or taken) separated by QUIET rounds."""
        out = []
        for i in self.mine:
            evs = sorted((r, att == i) for r, att, vic in self.hits if att == i or vic == i)
            if not evs:
                continue
            ep = [evs[0]]
            for e in evs[1:] + [None]:
                if e is None or e[0] - ep[-1][0] > QUIET:
                    start, end = ep[0][0], ep[-1][0]
                    s, x = self.context(i, start), self.context(i, end)
                    if s and x:
                        died = any(start <= r <= end + 2 for r in self.death_rounds[i])
                        # a real recall lands at spawn (Fish starts and cancels a recall every turn: those do not count)
                        recalled = not died and any((row := self.row(i, end + k)) and row[5] and cheb((row[1], row[2]), self.spawn.get(i, (0, 0))) <= 2
                                                    for k in range(1, 8))
                        kills = sum(1 for j in self.champs if self.champs[j]["team"] != self.team
                                    for r in self.death_rounds[j] if start <= r <= end + 2 and
                                    any(att == i and vic == j for rr, att, vic in self.hits if start <= rr <= end))
                        out.append({"type": self.champs[i]["type"], "start": s, "exit": x, "we_started": ep[0][1],
                                    "died": died, "recalled": recalled, "kills": kills, "length": end - start + 1})
                    if e is not None:
                        ep = [e]
                else:
                    ep.append(e)
        return out

    def deaths(self):
        out = []
        for i in self.mine:
            for r in self.death_rounds[i]:
                c = self.context(i, r - 1)
                b10 = self.context(i, r - 10)
                last = [a for a in self.actions if a[2] == i and r - 5 <= a[0] < r]
                hit_back = any(att == i for rr, att, vic in self.hits if r - 5 <= rr < r)
                hit = any(vic == i for rr, att, vic in self.hits if r - 5 <= rr < r)
                kinds = collections.Counter(a[1] for a in last)
                doing = ("fighting (hit back)" if hit_back else "hit, no hit back" if hit else
                         "attacking camps" if kinds[ATTACK] else "tributing" if kinds[SUBMIT] else "recalling" if kinds[RECALL] else
                         "walking" if kinds[MOVE] else "standing")
                if c:
                    out.append({"type": self.champs[i]["type"], "round": r, "hp10": b10["hp"] if b10 else None, "gems": c["gems"],
                                "alone": c["allies"] == 0, "near_ally": c["near_ally"], "enemies": c["enemies"], "doing": doing})
        return out

    def marcella(self):
        ids = [i for i in self.mine if self.champs[i]["type"] == 7]
        if not ids:
            return None
        m = ids[0]
        gem_gain = collections.Counter()
        prev = {}
        for r in sorted(self.frames):
            for i in self.mine:
                row = self.frames[r].get(i)
                if row and i in prev and row[6] > prev[i][6]:
                    gem_gain[i] += row[6] - prev[i][6]
                if row:
                    prev[i] = row
        tributes = [(a[0], (self.row(m, a[0]) or [0] * 10)[6]) for a in self.actions if a[2] == m and a[1] == SUBMIT]
        shops = [a[0] for a in self.actions if a[2] == m and a[1] == SHOP]
        recalls = [a[0] for a in self.actions if a[2] == m and a[1] == RECALL]
        fake = sum(1 for a in self.actions if a[2] == m and a[1] == 16)   # cancel channel (only kept by decode_all)
        dist = [(r, cheb((self.frames[r][m][1], self.frames[r][m][2]), self.spawn.get(m, (0, 0)))) for r in sorted(self.frames)
                if m in self.frames[r] and self.frames[r][m][5]]
        stat = [(r, self.frames[r][m][8], self.frames[r][m][9]) for r in sorted(self.frames) if m in self.frames[r]]
        return {"rounds": self.last, "gem_share": gem_gain[m] / max(1, sum(gem_gain.values())), "gems_picked": gem_gain[m],
                "tributes": tributes, "shops": shops, "recalls": recalls, "dist": dist, "stats": stat}


def load(path):
    games = []
    for mid, team in json.load(open(path)):
        f = DEC / f"{mid}.json"
        if f.exists():
            games.append(Game(json.load(open(f)), team))
    return games


def fight_table(label, games, L):
    fs = [f for g in games for f in g.fights()]
    if not fs:
        return
    started = [f for f in fs if f["we_started"]]
    L.append(f"\n**{label}**: {len(fs)} fights in {len(games)} games; started by them {100 * len(started) / len(fs):.0f}%")
    for name, sub in (("they started", started), ("they were attacked", [f for f in fs if not f["we_started"]])):
        if not sub:
            continue
        hp = [f["start"]["hp"] for f in sub]
        L.append(f"- Entry when {name} ({len(sub)}): health median {100 * statistics.median(hp):.0f}% (25% of entries below {100 * q(hp, 0.25):.0f}%); "
                 f"own side within 5 tiles (incl. self) mean {statistics.mean(f['start']['allies'] + 1 for f in sub):.1f} vs enemies {statistics.mean(f['start']['enemies'] for f in sub):.1f}; "
                 f"outnumbered at entry {100 * sum(f['start']['allies'] + 1 < f['start']['enemies'] for f in sub) / len(sub):.0f}%; "
                 f"nearest ally median {statistics.median(f['start']['near_ally'] for f in sub)} tiles")
    died = [f for f in fs if f["died"]]
    left = [f for f in fs if not f["died"]]
    L.append(f"- Exit: died {100 * len(died) / len(fs):.0f}%, recalled out {100 * sum(f['recalled'] for f in left) / len(fs):.0f}%, walked out / fight ended {100 * sum(not f['recalled'] for f in left) / len(fs):.0f}%. "
             f"Health when leaving alive: median {100 * statistics.median([f['exit']['hp'] for f in left]) if left else float('nan'):.0f}%; "
             f"fights with a kill {100 * sum(f['kills'] > 0 for f in fs) / len(fs):.0f}%; kill-to-death ratio in fights {sum(f['kills'] for f in fs) / max(1, len(died)):.2f}")


def death_table(label, games, L):
    ds = [d for g in games for d in g.deaths()]
    if not ds:
        return
    hp = [d["hp10"] for d in ds if d["hp10"] is not None]
    doing = collections.Counter(d["doing"] for d in ds)
    L.append(f"\n**{label}**: {len(ds)} deaths in {len(games)} games ({len(ds) / len(games):.1f} per game); alone (no ally within 5) "
             f"{100 * sum(d['alone'] for d in ds) / len(ds):.0f}%; nearest ally median {statistics.median(d['near_ally'] for d in ds)} tiles; "
             f"enemies within 5 mean {statistics.mean(d['enemies'] for d in ds):.1f}; health 10 rounds before median {100 * statistics.median(hp):.0f}%; "
             f"gems carried mean {statistics.mean(d['gems'] for d in ds):.1f} (0 gems in {100 * sum(d['gems'] == 0 for d in ds) / len(ds):.0f}%)")
    L.append("- Doing in the 5 rounds before: " + ", ".join(f"{k} {100 * v / len(ds):.0f}%" for k, v in doing.most_common()))


def marcella_table(label, games, L):
    ms = [g.marcella() for g in games]
    ms = [m for m in ms if m]
    if not ms:
        return
    trib = [t for m in ms for t in m["tributes"]]
    first_t = [m["tributes"][0][0] for m in ms if m["tributes"]]
    first_shop = [m["shops"][0] for m in ms if m["shops"]]
    L.append(f"\n**{label}** ({len(ms)} games): share of the team's gem pickups {100 * statistics.mean(m['gem_share'] for m in ms):.0f}%; "
             f"tributes per 100 rounds {statistics.mean(len(m['tributes']) / max(1, m['rounds']) * 100 for m in ms):.2f}, "
             f"bag median {statistics.median([b for _, b in trib]) if trib else 0:.0f} (25–75%: {q([b for _, b in trib], .25):.0f}–{q([b for _, b in trib], .75):.0f}), "
             f"first tribute median r{statistics.median(first_t) if first_t else float('nan'):.0f}; shop visits per 100 rounds "
             f"{statistics.mean(len(m['shops']) / max(1, m['rounds']) * 100 for m in ms):.2f}, first shop median r{statistics.median(first_shop) if first_shop else float('nan'):.0f}; "
             f"recall starts per 100 rounds {statistics.mean(len(m['recalls']) / max(1, m['rounds']) * 100 for m in ms):.2f} (Fish: started and cancelled every turn)")
    # distance from spawn and stats over time
    for r0 in (100, 250, 500, 750, 1000):
        ds = [min(m["dist"], key=lambda x: abs(x[0] - r0))[1] for m in ms if m["dist"] and m["rounds"] >= r0]
        st = [min(m["stats"], key=lambda x: abs(x[0] - r0)) for m in ms if m["stats"] and m["rounds"] >= r0]
        if ds and st:
            L.append(f"- r{r0}: distance from spawn median {statistics.median(ds)} tiles; Strength {statistics.median(x[1] for x in st)}, Focus {statistics.median(x[2] for x in st)} (n={len(ds)})")


def main():
    groups = []
    for spec in sys.argv[1:]:
        label, path = spec.split("=", 1)
        groups.append((label, load(path)))
    L = ["# Focused behaviour report (decoded replays)\n",
         "Fight = champion-on-champion hits on one champion, separated by 10 quiet rounds. Numbers = living champions within 5 tiles. "
         "Health from round frames.\n", "## 1. Fight entry and exit"]
    for label, games in groups:
        fight_table(label, games, L)
    L.append("\n## 2. Marcella over a game")
    for label, games in groups:
        marcella_table(label, games, L)
    L.append("\n## 3. Deaths")
    for label, games in groups:
        death_table(label, games, L)
    out = ROOT / "replays/top10/focus.md"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(L) + "\n")
    print("\n".join(L))


if __name__ == "__main__":
    main()
