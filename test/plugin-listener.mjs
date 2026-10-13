#!/usr/bin/env node
// SPDX-License-Identifier: GPL-2.0-or-later
// Listens to a channel on the hub and checks what the OBS plugin sends: well-formed chunks
// (3 stems × 25 Opus frames, consecutive seqs) carrying real audio (not silence).
//   node plugin-listener.mjs --server ws://localhost:8787 --channel <name> [--seconds 20]
// Exits non-zero on failure (used by CI).
import { parseArgs } from "node:util";

const { values: args } = parseArgs({
  options: { server: { type: "string", default: "ws://localhost:8787" }, channel: { type: "string" }, seconds: { type: "string", default: "20" } },
});
const seconds = Number(args.seconds);
const chunks = [];
let phase = "live";
const ws = new WebSocket(`${args.server}/listen/${args.channel}`);
ws.binaryType = "arraybuffer";
ws.onmessage = (ev) => {
  if (typeof ev.data === "string") {
    const m = JSON.parse(ev.data);
    if (m.type === "backlog" || m.type === "live") phase = m.type;
    return;
  }
  if (phase !== "live") return;
  const v = new DataView(ev.data);
  const stems = [];
  let off = 16;
  for (let s = 0; s < v.getUint8(1); s++) {
    const id = v.getUint8(off);
    const frames = v.getUint16(off + 1, true);
    off += 3;
    let bytes = 0;
    for (let f = 0; f < frames; f++) {
      const len = v.getUint16(off, true);
      bytes += len;
      off += 2 + len;
    }
    stems.push({ id, frames, bytes });
  }
  chunks.push({ seq: v.getUint32(2, true), size: ev.data.byteLength, parsedAll: off === ev.data.byteLength, stems });
};
ws.onerror = () => {
  console.error("could not connect to hub");
  process.exit(2);
};
await new Promise((r) => setTimeout(r, seconds * 1000));
ws.close();

const fail = [];
const expect = Math.floor(seconds * 2 * 0.8);
if (chunks.length < expect) fail.push(`only ${chunks.length} chunks in ${seconds}s (expected ≥ ${expect})`);
if (chunks.some((c) => !c.parsedAll)) fail.push("malformed chunk (bytes left over or missing)");
if (chunks.some((c) => c.stems.length !== 3 || c.stems.some((s) => s.frames !== 25))) fail.push("chunk without 3 stems × 25 frames");
const gaps = chunks.slice(1).filter((c, i) => c.seq !== chunks[i].seq + 1).length;
if (gaps) fail.push(`${gaps} sequence gaps`);
// Silence encodes to ~3 bytes per Opus frame; real audio at 64 kbps is ~160.
const perStem = [0, 1, 2].map((id) => chunks.reduce((n, c) => n + (c.stems.find((s) => s.id === id)?.bytes ?? 0), 0) / Math.max(1, chunks.length) / 25);
perStem.forEach((b, id) => b < 40 && fail.push(`stem ${["voice", "game", "music"][id]} looks silent (${b.toFixed(1)} bytes/frame)`));

console.log(JSON.stringify({ chunks: chunks.length, avgChunkBytes: Math.round(chunks.reduce((n, c) => n + c.size, 0) / Math.max(1, chunks.length)), bytesPerFrame: Object.fromEntries(perStem.map((b, i) => [["voice", "game", "music"][i], +b.toFixed(1)])), gaps, result: fail.length ? "FAIL" : "PASS", fail }, null, 2));
process.exit(fail.length ? 1 : 0);
