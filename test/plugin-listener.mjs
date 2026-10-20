#!/usr/bin/env node
// SPDX-License-Identifier: GPL-2.0-or-later
// Listens to a channel on the hub and checks what the OBS plugin sends: well-formed chunks
// (every announced stem × 25 Opus frames, consecutive seqs) carrying real audio (not silence).
//   node plugin-listener.mjs --server ws://localhost:8787 --channel <name> [--seconds 20] [--kbps 128]
//                            [--stems 3] [--names voice,game,music,rest] [--rest-audible]
// --kbps: the per-track bitrate the plugin should be sending (its default, or what the hub asked for).
// --stems: how many stems the plugin must announce ({"type":"stems"}, before the first live chunk).
// --rest-audible: the "Everything else" stem must carry audio (the headless host puts a source on
//   the streamed track only); otherwise it may be silent (0 frames) or absent.
// Exits non-zero on failure (used by CI).
import { parseArgs } from "node:util";

const { values: args } = parseArgs({
  options: {
    server: { type: "string", default: "ws://localhost:8787" },
    channel: { type: "string" },
    seconds: { type: "string", default: "20" },
    kbps: { type: "string", default: "128" },
    stems: { type: "string", default: "3" },
    "rest-audible": { type: "boolean", default: false },
    // The announced names, lower-case, "rest" for the Everything else stem (plugin 0.4.0 sources mode).
    names: { type: "string" },
  },
});
const seconds = Number(args.seconds);
const expectStems = Number(args.stems);
const chunks = [];
let phase = "live";
let announced = null;
let stemsBeforeAudio = false;
const ws = new WebSocket(`${args.server}/listen/${args.channel}`);
ws.binaryType = "arraybuffer";
ws.onmessage = (ev) => {
  if (typeof ev.data === "string") {
    const m = JSON.parse(ev.data);
    if (m.type === "backlog" || m.type === "live") phase = m.type;
    if (m.type === "stems") {
      announced = m.stems;
      if (chunks.length === 0) stemsBeforeAudio = true;
    }
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
    let mono = 0;
    for (let f = 0; f < frames; f++) {
      const len = v.getUint16(off, true);
      bytes += len;
      if (len && !(v.getUint8(off + 2) & 0x04)) mono++; // Opus TOC byte: bit 2 is the stereo flag
      off += 2 + len;
    }
    stems.push({ id, frames, bytes, mono });
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
if (!announced) fail.push("no stems message from the plugin");
else if (announced.length !== expectStems) fail.push(`plugin announced ${announced.length} stems, expected ${expectStems}`);
else if (!stemsBeforeAudio) fail.push("stems message arrived after the first chunk");
const names = announced ? announced.map((s) => (s.rest ? "rest" : s.name.toLowerCase())) : ["voice", "game", "music"];
const restId = announced?.find((s) => s.rest)?.id ?? -1;
if (expectStems > 3 && restId !== expectStems - 1) fail.push(`the last stem should be the rest stem (${JSON.stringify(announced)})`);
if (args.names && names.join(",") !== args.names.toLowerCase()) fail.push(`stems announced as ${names.join(",")}, expected ${args.names}`);
// Every stem is present with 25 frames, or marked silent (0 frames: its whole chunk was below -60 dBFS).
if (chunks.some((c) => c.stems.length !== expectStems || c.stems.some((s) => s.frames !== 25 && s.frames !== 0))) fail.push(`chunk without ${expectStems} stems × 25 (or 0) frames`);
const gaps = chunks.slice(1).filter((c, i) => c.seq !== chunks[i].seq + 1).length;
if (gaps) fail.push(`${gaps} sequence gaps`);
if (chunks.some((c) => c.stems.some((s) => s.mono))) fail.push("mono Opus packets (stems must be stereo)");
// Silence encodes to ~3 bytes per Opus frame; real audio at 128 kbps is ~320.
const perStem = names.map((_, id) => chunks.reduce((n, c) => n + (c.stems.find((s) => s.id === id)?.bytes ?? 0), 0) / Math.max(1, chunks.length) / 25);
perStem.forEach((b, id) => id !== restId && b < 40 && fail.push(`stem ${names[id]} looks silent (${b.toFixed(1)} bytes/frame)`));
if (restId >= 0) {
  const silentChunks = chunks.filter((c) => c.stems.find((s) => s.id === restId)?.frames === 0).length;
  if (args["rest-audible"] && perStem[restId] < 40) fail.push(`"Everything else" looks silent (${perStem[restId].toFixed(1)} bytes/frame, ${silentChunks} silent chunks)`);
  if (!args["rest-audible"] && silentChunks < chunks.length * 0.9) fail.push(`"Everything else" should be silent when every source is on the stream track (${silentChunks}/${chunks.length} silent chunks)`);
}
// The busiest stem runs close to the target bitrate (VBR: quieter stems use less).
const expectBytes = (Number(args.kbps) * 1000 * 0.02) / 8;
const busiest = Math.max(...perStem.slice(0, 3));
if (busiest < expectBytes * 0.8 || busiest > expectBytes * 1.2) fail.push(`busiest stem at ${busiest.toFixed(0)} bytes/frame, expected ~${expectBytes} for ${args.kbps} kbps`);

console.log(JSON.stringify({ chunks: chunks.length, avgChunkBytes: Math.round(chunks.reduce((n, c) => n + c.size, 0) / Math.max(1, chunks.length)), stems: names, bytesPerFrame: Object.fromEntries(perStem.map((b, i) => [names[i], +b.toFixed(1)])), gaps, result: fail.length ? "FAIL" : "PASS", fail }, null, 2));
process.exit(fail.length ? 1 : 0);
