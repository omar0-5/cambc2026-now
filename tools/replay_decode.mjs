// Decode a match replay with the engine the cambc visualiser ships (its wasm + snapshot parsers), into compact JSON:
//   champions: [{index, team, type}], actions: [[round, kind, actor, slot, abilityType, fx, fy, tx, ty]],
//   frames: {round: [[index, x, y, health, maxHealth, alive, gems, gold, effStrength, effFocus], ...]} (one per round).
// Action kinds (visualiser names): 4 Move, 5 Attack, 6 Cast ability, 11 Recall, 12 Submit gems, 13 Shop, 14 Place ward,
// 15 Consumable, 20 Place altar lock, 21 Teleport channel. Champion types: 0 Ryth, 1 Vesper Vale, 2 Vendetta, 3 Cassi,
// 4 Issac, 5 Stellar, 6 Prism, 7 Marcella, 8 Willow, 9 Ren, 10 Varg, 11 Tavi, 12 Heart of Ruin.
//
// Usage: node tools/replay_decode.mjs VIS_DIR REPLAY.repl26a OUT.json
//   VIS_DIR: a copy of the visualiser assets with package.json {"type":"module"} (see tools/replay_setup.sh).
import fs from "fs";
import path from "path";
import zlib from "zlib";

const [, , visDir, replayPath, outPath] = process.argv;
const PC = await import(path.resolve(visDir, fs.readdirSync(visDir).find((f) => f.startsWith("PanelChrome-"))));
const wasmFile = path.resolve(visDir, fs.readdirSync(visDir).find((f) => f.startsWith("v2_0_0") && f.endsWith(".wasm")));
let memory;
const view = () => new DataView(memory.buffer);
const cstr = (p) => { const b = new Uint8Array(memory.buffer); let e = p; while (b[e]) e++; return new TextDecoder().decode(b.subarray(p, e)); };
const wasi = {
  environ_get: () => 0, environ_sizes_get: (a, b) => { view().setUint32(a, 0, true); view().setUint32(b, 0, true); return 0; },
  fd_close: () => 0, fd_fdstat_get: () => 0, fd_prestat_get: () => 8, fd_prestat_dir_name: () => 8, fd_read: () => 0, fd_seek: () => 70,
  fd_write: (fd, iovs, n, out) => { let c = 0; for (let i = 0; i < n; i++) c += view().getUint32(iovs + i * 8 + 4, true); view().setUint32(out, c, true); return 0; },
  proc_exit: (c) => { throw Error("exit " + c); },
  random_get: (p, l) => { const b = new Uint8Array(memory.buffer, p, l); for (let i = 0; i < l; i++) b[i] = (Math.random() * 256) | 0; return 0; },
};
const { instance } = await WebAssembly.instantiate(fs.readFileSync(wasmFile), {
  wasi_snapshot_preview1: wasi, deep_wasm: { on_assert_fail_impl: () => 1 },
});
const X = instance.exports;
memory = X.memory;
X._initialize();
const out = X.Engine_Malloc(32), args = X.Engine_Malloc(128);
const check = (code, what) => { if (code !== 0) throw Error(`${what}: error ${code} ${cstr(X.GetLastErrorMsg())}`); };

function innerReplay(p) {
  const buf = fs.readFileSync(p);
  if (buf[0] !== 0x50 || buf[1] !== 0x4b) return buf;
  for (let off = 0; off < buf.length - 30;) {
    if (buf.readUInt32LE(off) !== 0x04034b50) { off++; continue; }
    const method = buf.readUInt16LE(off + 8), csize = buf.readUInt32LE(off + 18), nlen = buf.readUInt16LE(off + 26), xlen = buf.readUInt16LE(off + 28);
    const name = buf.subarray(off + 30, off + 30 + nlen).toString();
    const data = buf.subarray(off + 30 + nlen + xlen, off + 30 + nlen + xlen + csize);
    if (name === "replay.repl26a") return method === 8 ? zlib.inflateRawSync(data) : data;
    off += 30 + nlen + xlen + csize;
  }
  throw Error("no replay.repl26a in bundle");
}

const bytes = innerReplay(replayPath);
const p = X.Engine_Malloc(bytes.length);
new Uint8Array(memory.buffer, p, bytes.length).set(bytes);
check(X.Game_FromReplayFileBytes(p, bytes.length, out), "FromReplay");
const game = view().getBigUint64(out, true);
check(X.Game_Visualiser_Timeline_GetMain(game, out), "GetMain");
const tl = view().getBigUint64(out, true);
check(X.Game_Visualiser_Timeline_Info(game, tl, args, args + 8, args + 16), "Info");
const size = view().getUint32(args, true);
const tp = X.Engine_Malloc(X.Game_Visualiser_TimelineSnapshotSize());
check(X.Game_Visualiser_Timeline_Seek(game, tl, 0, 0), "Seek");

const champions = new Map();
const actions = [];
const frames = {};
let lastRound = -1;
const KEEP = new Set([4, 5, 6, 11, 12, 13, 14, 15, 20, 21]);
// ReplayNext advances one substep (not one timeline position): run until the last position or the match ends.
for (let guard = 0; guard < 5_000_000; guard++) {
  if (X.Game_Visualiser_Timeline_ReplayNext(game, 1) !== 0) break;
  check(X.Game_Visualiser_ReadTimelineState(game, tp), "ReadTimelineState");
  const st = PC.I(view(), tp);
  const s = st.step;
  if (st.inputPosition >= size - 1 || st.phase === 11) break;   // 11 = Finished
  if (st.round !== lastRound && st.champions?.length) {   // one frame per round, at its first position
    lastRound = st.round;
    frames[st.round] = st.champions.map((c) => {
      if (!champions.has(c.entity.index)) champions.set(c.entity.index, { index: c.entity.index, team: c.team, type: c.type });
      return [c.entity.index, c.position?.x ?? -1, c.position?.y ?? -1, Math.round(c.health ?? 0), Math.round(c.maxHealth ?? 0), c.alive ? 1 : 0,
              c.gems ?? 0, c.gold ?? 0, Math.round(c.strength?.effective ?? 0), Math.round(c.focus?.effective ?? 0)];
    });
  }
  if (s && KEEP.has(s.kind) && s.actor?.kind === 1)
    actions.push([s.round, s.kind, s.actor.index, s.abilitySlot ?? -1, s.abilityType ?? -1, s.from?.x ?? -1, s.from?.y ?? -1, s.to?.x ?? -1, s.to?.y ?? -1]);
}
fs.writeFileSync(outPath, JSON.stringify({ champions: [...champions.values()], actions, frames }));
console.log(`${replayPath}: ${actions.length} actions, ${Object.keys(frames).length} rounds, ${champions.size} champions`);
