#!/usr/bin/env python3
"""P(death) calibration: bucket every Level 2 prediction ("PD" lines, LOG_PDEATH = true) and report how often
that champion actually died within the plan's T rounds (death events in the statistics JSON).

Usage: python3 tools/calibrate.py replays/bench/NAME
"""
import collections
import json
import pathlib
import re
import sys
import zipfile

PD = re.compile(r"PD (\w+) r(\d+) T(\d+) p(\d+) m(\S+)")  # p in thousandths


def main(folder):
    rows = collections.defaultdict(lambda: [0, 0, 0.0])   # (mode, bucket) -> [predictions, deaths, sum p]
    for replay in sorted(pathlib.Path(folder).glob("*.repl26a")):
        stats = json.loads(replay.with_suffix(".json").read_text())
        with zipfile.ZipFile(replay) as z:
            logs = {side: z.read(f"team-{side}.stdout").decode(errors="replace") for side in ("a", "b")}
        side = "a" if "PD " in logs["a"] else "b"   # only our bot prints PD lines
        team = "teamA" if side == "a" else "teamB"
        deaths = collections.defaultdict(list)       # champion name -> death rounds (owner = the one who died)
        for ev in stats["events"]:
            if ev["kind"] == "death" and ev["owner"]["team"] == team:
                deaths[ev["owner"]["name"].lower().replace(" ", "_")] += [e["timestamp"]["round"] for e in ev["entries"]]
        for m in PD.finditer(logs[side]):
            name, r, T, p, mode = m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4)) / 1000, m.group(5)
            died = any(r < d <= r + T for d in deaths[name])
            b = min(int(p * 10), 9)
            for key in ((mode, b), ("all", b)):
                rows[key][0] += 1
                rows[key][1] += died
                rows[key][2] += p
    for mode in sorted({k[0] for k in rows}, key=lambda m: (m != "all", m)):
        print(f"mode {mode}:  bucket   predictions  mean predicted  actual death rate")
        for b in range(10):
            n, d, sp = rows.get((mode, b), (0, 0, 0.0))
            if n:
                print(f"  {b * 10:3d}-{b * 10 + 10:3d}%  {n:11d}  {sp / n:13.1%}  {d / n:16.1%}")


if __name__ == "__main__":
    main(sys.argv[1])
