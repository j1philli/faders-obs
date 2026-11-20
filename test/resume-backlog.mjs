// SPDX-License-Identifier: GPL-2.0-or-later
// Real plugin/libobs test: when listeners come back after a pause, the plugin offers its whole
// 60 s backlog again, not just what it captured during the pause. A hub object that hibernated
// while nobody listened has lost its ring, and a viewer joining then needs that history for
// auto-sync (a YouTube stream runs ~20 s behind the plugin).
// Run after building: node test/resume-backlog.mjs
import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { once } from "node:events";
import { mkdtemp, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { resolve } from "node:path";
import WebSocket from "ws";

const root = resolve(import.meta.dirname, "..");
const temp = await mkdtemp(`${tmpdir()}/faders-plugin-resume-`);
const port = Number(process.env.FADERS_RESUME_TEST_PORT || 18791);
const channel = "resume";
const sleep = (ms) => new Promise((done) => setTimeout(done, ms));
const children = [];
let output = "", errors = "";
function start(command, args) {
  const child = spawn(command, args, { cwd: root, env: { ...process.env, TMPDIR: temp, TEMP: temp }, stdio: ["ignore", "pipe", "pipe"] });
  children.push(child);
  child.stdout.on("data", (data) => output += data);
  child.stderr.on("data", (data) => errors += data);
  return child;
}
async function waitFor(text, after = 0) {
  for (let i = 0; i < 300; i++) {
    if (output.indexOf(text, after) >= 0) return;
    await sleep(100);
  }
  assert.fail(`Missing ${text}\n${output}\n${errors}`);
}
async function stop(child) {
  if (child.exitCode === null && child.signalCode === null) {
    child.kill("SIGTERM");
    await once(child, "exit");
  }
}
// Listens for `ms` and returns the seqs of the chunks received (backlog and live).
async function listen(ms) {
  const ws = new WebSocket(`ws://127.0.0.1:${port}/listen/${channel}`);
  const seqs = [];
  ws.on("message", (data, isBinary) => isBinary && seqs.push(data.readUInt32LE(2)));
  await once(ws, "open");
  await sleep(ms);
  ws.close();
  await once(ws, "close");
  return seqs;
}
try {
  start(process.execPath, ["test/mock-hub.mjs", "--port", String(port)]);
  await waitFor("mock hub on");
  start(resolve(root, "build/faders-headless"), [`ws://127.0.0.1:${port}/ingest/${channel}`, "dev-token", ...["voice", "game", "music"].map((name) => `${root}/test/media/${name}.wav`), "45"]);
  await waitFor("connected to");
  await sleep(2000);

  const first = await listen(4000);
  assert.ok(first.length > 0, `first listener got audio\n${output}`);
  const lastHeard = Math.max(...first);
  const paused = output.length;
  await waitFor("pausing uplink", paused - 200);
  await sleep(10_000); // paused: the hub sees nothing, the plugin keeps its backlog
  await fetch(`http://127.0.0.1:${port}/forget`, { method: "POST", body: JSON.stringify({ channel }) });

  const second = await listen(2000);
  const oldest = Math.min(...second);
  assert.ok(second.length > 0, "second listener got audio");
  assert.ok(oldest <= lastHeard, `after a pause the plugin re-offered audio from before it (oldest seq ${oldest}, first listener's last ${lastHeard})`);
  console.log(`resume-backlog: after a 10 s pause and a hub that forgot, the plugin re-sent ${second.length} chunks from seq ${oldest} (first listener's last: ${lastHeard})`);
} finally {
  await Promise.all(children.map(stop));
  await rm(temp, { recursive: true, force: true });
}
