#!/usr/bin/env python3
"""Fetch ladder match listings + JSON summaries for the gap analysis: our recent ranked matches and the
top-N teams' recent ranked matches. Writes replays/gap/{index.json, ranks.json, s/<matchId>.json}.

Usage: python3 tools/gap_fetch.py [--ours 150] [--top 8] [--per-top 15]
"""
import argparse
import json
import pathlib
import subprocess
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
C = str(ROOT / ".venv/bin/cambc")
OUT = ROOT / "replays/gap"


def cambc_json(*args):
    r = subprocess.run([C, *args, "--json"], capture_output=True, text=True, cwd=ROOT)
    return json.loads(r.stdout)


def listing(team, n):
    got, cursor = [], None
    while len(got) < n:
        extra = ["--cursor", cursor] if cursor else []
        page = cambc_json("matches", "list", "--team", team, "--kind", "ranked", "--limit", str(min(50, n - len(got))), *extra)
        got += [m for m in page["matches"] if m["status"] == "finished"]
        cursor = page.get("nextCursor")
        if not cursor or not page["matches"]:
            break
    return got[:n]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ours", type=int, default=150)
    ap.add_argument("--top", type=int, default=8)
    ap.add_argument("--per-top", type=int, default=15)
    args = ap.parse_args()
    (OUT / "s").mkdir(parents=True, exist_ok=True)
    q = cambc_json("match-groups", "quota")
    ranks = q["opponentRanks"]
    ladder = cambc_json("ladder", "--limit", str(args.top))
    top = ladder if isinstance(ladder, list) else ladder.get("entries", ladder.get("teams", []))
    json.dump({"ranks": ranks, "me": q["team"], "my_rank": q["requesterRank"], "top": top}, open(OUT / "ranks.json", "w"))
    index = {"ours": listing("me", args.ours), "top": {}}
    for t in top:
        tid = t.get("teamId") or t.get("id")
        index["top"][tid] = listing(tid, args.per_top)
    json.dump(index, open(OUT / "index.json", "w"))
    ids = {m["matchId"] for m in index["ours"]} | {m["matchId"] for ms in index["top"].values() for m in ms}
    todo = [i for i in ids if not (OUT / "s" / f"{i}.json").exists()]

    def fetch(i):   # one at a time with retries: parallel requests were rate-limited (46 of 260 came back)
        for wait in (0, 5, 20):
            time.sleep(wait)
            subprocess.run([C, "matches", "summary", i, "--json", "-o", str(OUT / "s" / f"{i}.json")], capture_output=True, cwd=ROOT)
            if (OUT / "s" / f"{i}.json").exists():
                return

    for i in todo:
        fetch(i)
    print(f"ours {len(index['ours'])}, top teams {len(index['top'])}, summaries {len(list((OUT / 's').glob('*.json')))}/{len(ids)}")


if __name__ == "__main__":
    main()
