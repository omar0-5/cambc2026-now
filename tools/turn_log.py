#!/usr/bin/env python3
"""Per-turn analysis of our own games from LOG_TURNS lines ("M <round> <champ> <MODE>:<step>/<detail> hit <0|1> target <g>
state <s> bag <b> hp <h> at <x>,<y> ...").

1. Time breakdown per champion per 100 rounds (first 1,000 rounds): walking vs idle split by position change.
2. Camp outcomes: every camp group a champion damaged -> captured by us, captured by the enemy, or abandoned (and why).

Usage:
  python3 tools/turn_log.py run BOT_DIR OPPONENT_DIR NAME [--half A|B]   # BOT_DIR built with LOG_TURNS=true; side a only
  python3 tools/turn_log.py report NAME [NAME ...]
"""
import collections
import json
import pathlib
import re
import statistics
import subprocess
import sys
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "replays" / "turnlog"
LINE = re.compile(r"^M (\d+) (\S+) (\S*):(\S*)/(\S*) hit (\d) target (-?\d+) state (-?\d+) bag (\d+) hp (-?\d+) at (-?\d+),(-?\d+)")
CAMP_DETAILS = {"tank", "ranged-covered", "ranged-alone", "clusters"}
FIGHT_STEPS = {"enemies", "standoff", "contest", "disrupt"}
FIGHT_DETAILS = {"fight", "interrupt", "walk-away", "contest"}
CATS = ["dead", "fight", "camp", "capture", "respawn_wait", "bank_walk", "tribute", "retreat", "home", "walk", "idle"]
RESPAWNING = 3


def run(bot, opp, name, half=None):
    maps = sorted((ROOT / "maps").glob("*.map26a"))
    if half:
        maps = maps[0 if half == "A" else 1::2]
    d = OUT / name
    d.mkdir(parents=True, exist_ok=True)
    rows = []
    with open(d / "manifest.txt", "w") as f:
        for m in maps:
            stem = d / m.stem
            for p in (pathlib.Path(f"{stem}.repl26a"), pathlib.Path(f"{stem}.json")):
                p.unlink(missing_ok=True)
            f.write(f"{(ROOT / bot).resolve()} {(ROOT / opp).resolve()} {m} {stem}.repl26a --statistics {stem}.json --json\n")
            rows.append(stem)
    subprocess.run([str(ROOT / ".venv/bin/cambc"), "run-batch", str(d / "manifest.txt"), "--release", "--jobs", "2",
                    "--log-limit", "30000000"], stdout=subprocess.DEVNULL, check=False)
    for stem in rows:   # keep our stdout and the statistics; drop the replay (~1 MB)
        rp = pathlib.Path(f"{stem}.repl26a")
        if rp.exists():
            pathlib.Path(f"{stem}.log").write_bytes(zipfile.ZipFile(rp).read("team-a.stdout").replace(b"\0", b""))
            rp.unlink()


def parse(path):
    turns = collections.defaultdict(list)
    for line in open(path, errors="replace"):
        m = LINE.match(line)
        if m:
            r, champ, mode, step, detail, hit, target, state, bag, hp, x, y = m.groups()
            turns[champ].append(dict(r=int(r), mode=mode, step=step, detail=detail, hit=hit == "1", target=int(target),
                                     state=int(state), bag=int(bag), pos=(int(x), int(y))))
    return turns


def category(t, prev):
    moved = prev is not None and prev["pos"] != t["pos"]
    if t["mode"] == "FIGHT" or t["step"] in FIGHT_STEPS or t["detail"] in FIGHT_DETAILS:
        return "fight"
    if t["step"] == "capture" or t["detail"] == "capture":
        return "capture"
    if t["detail"] in CAMP_DETAILS:
        return "camp"
    if t["detail"] == "respawn-wait":
        return "respawn_wait"
    if t["step"] in ("bank", "endgame-bank") or t["mode"] == "CARRY":
        return "bank_walk" if moved else "tribute"
    if t["mode"] == "RETREAT" or t["step"] == "retreat":
        return "retreat"
    if t["step"] in ("heal", "shop-trip"):
        return "home"
    return "walk" if moved else "idle"


def time_breakdown(turns, horizon=1000):
    per = {}
    for champ, ts in turns.items():
        c = collections.Counter()
        seen = set()
        prev = None
        for t in ts:
            if t["r"] >= horizon:
                break
            c[category(t, prev)] += 1
            seen.add(t["r"])
            prev = t
        first = ts[0]["r"] if ts else 0
        n = max(1, min(horizon, (ts[-1]["r"] + 1) if ts else horizon) - first)
        c["dead"] += max(0, n - len(seen))   # rounds with no turn line: dead (or draft)
        per[champ] = {k: c[k] / n * 100 for k in CATS}
    return per


def abandon_reason(t):
    if t is None:
        return "died/end"
    if t["mode"] == "RETREAT" or t["step"] == "retreat":
        return "retreat"
    if t["mode"] == "FIGHT" or t["step"] in FIGHT_STEPS or t["detail"] in FIGHT_DETAILS:
        return "enemy (fight/walk-away)"
    if t["mode"] == "CARRY" or t["step"] in ("bank", "endgame-bank"):
        return "bank trip"
    if t["step"] in ("heal", "shop-trip"):
        return "heal/shop"
    if t["mode"] == "EXPLORE" or t["step"] in ("idle", "explore"):
        return "explore/idle"
    return "re-target"


def camp_outcomes(turns):
    """One record per (champion, group) damage episode."""
    captures = collections.defaultdict(list)   # group -> rounds an ally was on its capture step
    for ts in turns.values():
        for t in ts:
            if t["step"] == "capture" or t["detail"] == "capture":
                captures[t["target"]].append(t["r"])
    out = []
    for champ, ts in turns.items():
        ep = None
        for i, t in enumerate(ts + [None]):
            if ep and (t is None or t["target"] != ep["g"] or t["state"] == RESPAWNING or t["r"] - ep["last"] > 3):
                if ep["damaged"]:
                    last = ep["last"]
                    ours = any(ep["start"] <= r <= last + 8 for r in captures[ep["g"]])
                    went = t is not None and t["target"] == ep["g"] and t["state"] == RESPAWNING
                    if ours:
                        outcome = "captured by us"
                    elif went:
                        outcome = "captured by enemy"
                    elif t is not None and t["r"] - ep["last"] > 3:
                        outcome = "abandoned: died/end"
                    else:
                        outcome = "abandoned: " + abandon_reason(t)
                    out.append((champ, ep["g"], outcome))
                ep = None
            if t is None:
                break
            if t["target"] >= 0 and (ep is None):
                ep = {"g": t["target"], "start": t["r"], "last": t["r"], "damaged": False}
            if ep:
                ep["last"] = t["r"]
                if t["hit"] and t["detail"] in CAMP_DETAILS:
                    ep["damaged"] = True
    return out


def report(names):
    for name in names:
        d = OUT / name
        logs = sorted(d.glob("*.log"))
        allc, per_champ, outcomes = [], collections.defaultdict(list), collections.Counter()
        stats = []
        for f in logs:
            turns = parse(f)
            for champ, cats in time_breakdown(turns).items():
                allc.append(cats)
                per_champ[champ].append(cats)
            outcomes.update(o for _, _, o in camp_outcomes(turns))
            js = f.with_suffix(".json")
            if js.exists():
                sys.path.insert(0, str(ROOT / "tools"))
                from gap_report import team_stats
                s = team_stats(json.load(open(js)), "teamA")
                stats.append(s)
        mean = statistics.mean
        print(f"\n== {name}: {len(logs)} games (side a), first 1,000 rounds, % of champion-rounds")
        print("champion".ljust(14) + "".join(c[:9].rjust(10) for c in CATS))
        print("ALL".ljust(14) + "".join(f"{mean(x[c] for x in allc):10.1f}" for c in CATS))
        for champ, xs in sorted(per_champ.items()):
            print(champ[:13].ljust(14) + "".join(f"{mean(x[c] for x in xs):10.1f}" for c in CATS))
        tot = sum(outcomes.values())
        print(f"  camp damage episodes {tot}: " + ", ".join(f"{k} {v} ({100 * v / max(1, tot):.0f}%)" for k, v in outcomes.most_common()))
        if stats:
            print(f"  gems by r1000 {mean(s['gems_r1000'] for s in stats):.0f}, camps/100 rounds "
                  f"{mean(s['camps'] / s['rounds'] * 100 for s in stats):.2f}, deaths {mean(s['deaths'] for s in stats):.1f}, "
                  f"wins {sum(s['result'] if 'result' in s else 0 for s in stats)}")


if __name__ == "__main__":
    if sys.argv[1] == "run":
        half = sys.argv[sys.argv.index("--half") + 1] if "--half" in sys.argv else None
        run(sys.argv[2], sys.argv[3], sys.argv[4], half)
    else:
        report(sys.argv[2:])
