#!/usr/bin/env python3
"""Copy a bot source with config.h overrides (same syntax as tools/tune.py) into a bot directory.

Usage: python3 tools/make_variant.py SRC_DIR OUT_BOT_DIR KEY=VALUE ...
Example (the ganker test opponent, bots/ganker): python3 tools/make_variant.py replays/submit/scA/src bots/ganker \\
    PWIN_FIGHT=0.0 COMMIT_PWIN=0.0 OUTNUMBER_PWIN=0.0 HUNT_RANGE=20 ENGAGE_RANGE=12"""
import pathlib
import shutil
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from tune import override  # noqa: E402

src, out = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
shutil.rmtree(out / "src", ignore_errors=True)
shutil.copytree(src, out / "src")
cfg = out / "src" / "config.h"
text = cfg.read_text()
for kv in sys.argv[3:]:
    k, v = kv.split("=", 1)
    text = override(text, k, v)
cfg.write_text(text)
print(f"{out}: {' '.join(sys.argv[3:])}")
