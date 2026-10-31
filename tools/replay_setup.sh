#!/bin/sh
# Copy the cambc visualiser's engine wasm + JS parsers into replays/work/vis as an ES-module folder for tools/replay_decode.mjs.
V=$(ls -td ~/Library/Caches/cambc/assets/*/visualiser 2>/dev/null | head -1)
[ -n "$V" ] || { echo "no visualiser in ~/Library/Caches/cambc/assets (run 'cambc view' once)"; exit 1; }
D="$(dirname "$0")/../replays/work/vis"
mkdir -p "$D" && cp "$V"/assets/*.js "$V"/assets/v2_0_0-*.wasm "$D"/ && echo '{"type":"module"}' > "$D/package.json" && echo "visualiser copied to $D"
