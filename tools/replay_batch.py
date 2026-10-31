#!/usr/bin/env python3
"""Download match replays and decode them (tools/replay_decode.mjs) into replays/decoded/<matchId>.json.

Usage: python3 tools/replay_batch.py IDS.txt [--jobs 2]   (one match id per line; already-decoded ids are skipped)
"""
import argparse
import pathlib
import subprocess
import time
from concurrent.futures import ThreadPoolExecutor

ROOT = pathlib.Path(__file__).resolve().parent.parent
C = str(ROOT / ".venv/bin/cambc")
RAW = ROOT / "replays/raw"
DEC = ROOT / "replays/decoded"
VIS = ROOT / "replays/work/vis"


def download(mid):
    f = RAW / f"{mid}.repl26a"
    for wait in (0, 5, 20, 60):
        if f.exists() and f.stat().st_size > 0:
            return f
        time.sleep(wait)
        subprocess.run([C, "matches", "download", mid, "-o", str(f)], capture_output=True)
    return f if f.exists() else None


def decode(mid):
    out = DEC / f"{mid}.json"
    if out.exists():
        return True
    r = subprocess.run(["nice", "-n", "19", "node", str(ROOT / "tools/replay_decode.mjs"), str(VIS), str(RAW / f"{mid}.repl26a"), str(out)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print("decode failed", mid, r.stderr[-300:])
    return out.exists()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ids")
    ap.add_argument("--jobs", type=int, default=2)
    args = ap.parse_args()
    RAW.mkdir(parents=True, exist_ok=True)
    DEC.mkdir(parents=True, exist_ok=True)
    if not (VIS / "package.json").exists():
        subprocess.run(["sh", str(ROOT / "tools/replay_setup.sh")], check=True)
    ids = [x.strip() for x in open(args.ids) if x.strip()]
    todo = [m for m in ids if not (DEC / f"{m}.json").exists()]
    ok = 0
    with ThreadPoolExecutor(args.jobs) as ex:
        futures = []
        for mid in todo:            # downloads one at a time (rate limit), decodes in parallel
            if download(mid):
                futures.append(ex.submit(decode, mid))
        ok = sum(f.result() for f in futures)
    print(f"decoded {ok} of {len(todo)} new ({len(ids)} listed)")


if __name__ == "__main__":
    main()
