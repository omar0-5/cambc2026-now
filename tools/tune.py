#!/usr/bin/env python3
"""One tuning run: copy bots/v13 with config.h overrides, bench one map half vs both baselines, and compare
with step c (replays/bench/c-vs-*/results.csv) on the same matches.

Usage: python3 tools/tune.py NAME --half A [LEVEL2=1 LAMBDA_T=2.0 LEVEL2_CARRY=true ...]
"""
import argparse
import csv
import pathlib
import re
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent


def override(config, key, value):
    if key == "LEVEL2":
        new, n = re.subn(r"#define LEVEL2 \d", f"#define LEVEL2 {value}", config)
    else:
        new, n = re.subn(rf"(constexpr [\w:]+ {key} = )[^;]+;", rf"\g<1>{value};", config)
    if n != 1:
        sys.exit(f"config.h: {key} not found exactly once")
    return new


def summary(rows):
    w = sum(r["result"] == "W" for r in rows)
    gems = sum(int(r["gems_a"]) - int(r["gems_b"]) for r in rows) / max(1, len(rows))
    fuel = sum(int(r["fuel_out_a"]) for r in rows)
    err = sum(int(r["errors_a"]) for r in rows)
    return w, len(rows) - w, gems, fuel, err


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("name")
    ap.add_argument("--half", choices=["A", "B"], required=True)
    ap.add_argument("--src", default=str(ROOT / "bots" / "v13" / "src"), help="bot source to copy (default bots/v13/src)")
    ap.add_argument("sets", nargs="*")
    args = ap.parse_intermixed_args()
    work = ROOT / "replays" / "tune" / args.name
    shutil.rmtree(work, ignore_errors=True)
    shutil.copytree(args.src, work / "bot" / "src")
    cfg = work / "bot" / "src" / "config.h"
    text = cfg.read_text()
    for kv in args.sets:
        k, v = kv.split("=", 1)
        text = override(text, k, v)
    cfg.write_text(text)
    for base in ("v13-baseline", "v15-baseline"):
        out = work / f"vs-{base}"
        subprocess.run([sys.executable, str(ROOT / "tools/bench.py"), str(work / "bot"), f"bots/{base}", "--half", args.half,
                        "--out", str(out)], check=False, stdout=subprocess.DEVNULL)
        mine = list(csv.DictReader(open(out / "results.csv")))
        keys = {(r["map"], r["a_side"]) for r in mine}
        ref = [r for r in csv.DictReader(open(ROOT / "replays/bench" / f"c-vs-{base}" / "results.csv")) if (r["map"], r["a_side"]) in keys]
        a, c = summary(mine), summary(ref)
        flips = sum(1 for x in mine for y in ref if (x["map"], x["a_side"]) == (y["map"], y["a_side"]) and x["result"] != y["result"])
        print(f"{args.name} half {args.half} vs {base}: {a[0]}-{a[1]} {a[2]:+.0f} gems, fuel out {a[3]}, errors {a[4]}"
              f" | step c {c[0]}-{c[1]} {c[2]:+.0f}, fuel out {c[3]} | win diff {a[0] - c[0]:+d}, {flips} flips", flush=True)


if __name__ == "__main__":
    main()
