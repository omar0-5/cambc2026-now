#!/usr/bin/env python3
"""Exact A/B scrims: requeue earlier scrim groups with --source-group (same opponent bot versions, maps and sides), so
the live version plays exactly what the earlier version played. Matches are deterministic, so differences are ours.

Usage: python3 tools/scrim_ab.py OUT.json PLAN.json[:SIDES] [PLAN.json[:SIDES] ...]
  SIDES: "fixed" = a,b,a,b,a for every group (the reference batch), "alternate" = scrim_queue.py's a,b,a,b,a / b,a,b,a,b by index."""
import json
import pathlib
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
C = str(ROOT / ".venv/bin/cambc")


def main():
    # resume: keep groups already queued into OUT (matched by opponent + maps)
    out = json.load(open(sys.argv[1])) if pathlib.Path(sys.argv[1]).exists() else []
    done = {(tid, tuple(maps)) for tid, maps, _ in out}
    for spec in sys.argv[2:]:
        path, _, mode = spec.partition(":")
        for i, (tid, maps, gid) in enumerate(json.load(open(path))):
            if not gid:
                continue
            if (tid, tuple(maps)) in done:
                continue
            sides = "a,b,a,b,a" if mode != "alternate" or i % 2 == 0 else "b,a,b,a,b"
            new = None
            for wait in (0, 3, 10, 30, 60):   # HTTP 429 when other tools hit the API at the same time
                time.sleep(wait)
                r = subprocess.run([C, "match-groups", "create", tid, "--source-group", gid, "--maps", ",".join(maps), "--sides", sides, "--json"],
                                   capture_output=True, text=True)
                try:
                    new = json.loads(r.stdout)["groupId"]
                    break
                except (ValueError, KeyError):
                    if "429" not in r.stdout + r.stderr:
                        print("ERR", gid[:8], r.stdout[-200:], r.stderr[-200:])
                        break
            if not new:
                continue
            out.append((tid, maps, new))
            time.sleep(1)
    json.dump(out, open(sys.argv[1], "w"))
    print(f"queued {len(out)} groups")


if __name__ == "__main__":
    main()
