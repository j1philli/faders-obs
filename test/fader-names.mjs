// SPDX-License-Identifier: GPL-2.0-or-later
// Real plugin/libobs test: initial labels, ignored invalid update, live rename and cached labels.
// Run after building: node test/fader-names.mjs
import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { once } from "node:events";
import { mkdtemp, readFile, readdir, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { resolve } from "node:path";

const root = resolve(import.meta.dirname, "..");
const temp = await mkdtemp(`${tmpdir()}/faders-plugin-names-`);
const port = Number(process.env.FADERS_NAMES_TEST_PORT || 18789);
const base = `http://127.0.0.1:${port}`;
const initial = { voice: "Mic 🎙", game: '"quality" kbps', music: "<b>Music</b>" };
const updated = { voice: "Commentary", game: "Discord", music: "🎵".repeat(24) };
const sleep = (ms) => new Promise((done) => setTimeout(done, ms));
const children = [];
let output = "", errors = "";
function start(command, args, env = {}) {
  const child = spawn(command, args, { cwd: root, env: { ...process.env, ...env }, stdio: ["ignore", "pipe", "pipe"] });
  children.push(child);
  child.stdout.on("data", (data) => output += data);
  child.stderr.on("data", (data) => errors += data);
  return child;
}
const line = (names) => `fader names: [${names.voice}] [${names.game}] [${names.music}]`;
async function waitFor(text) {
  for (let i = 0; i < 150; i++) {
    if (output.includes(text)) return;
    await sleep(100);
  }
  assert.fail(`Missing ${text}\n${output}\n${errors}`);
}
async function save(names) {
  const response = await fetch(`${base}/names`, { method: "POST", body: JSON.stringify({ channel: "names", names }) });
  assert.equal(response.status, 200);
}
try {
  start(process.execPath, ["test/mock-hub.mjs", "--port", String(port), "--names", JSON.stringify(initial)]);
  await waitFor("mock hub on");
  const host = start(resolve(root, "build/faders-headless"), [`ws://127.0.0.1:${port}/ingest/names`, "dev-token", ...["voice", "game", "music"].map((name) => `${root}/test/media/${name}.wav`), "12"], { TMPDIR: temp, TEMP: temp });
  const exited = once(host, "exit");
  await waitFor(line(initial));
  output = "";
  await save({ ...updated, voice: "x".repeat(25) });
  await waitFor(line(initial)); // A malformed update leaves the complete previous set intact.
  await save(updated);
  await waitFor(line(updated));
  const [code] = await exited;
  assert.equal(code, 0, errors);
  const files = await readdir(temp, { recursive: true });
  const settingsPath = files.find((name) => name.endsWith("settings.json"));
  assert.ok(settingsPath, "plugin cached names locally");
  const settings = JSON.parse(await readFile(resolve(temp, settingsPath), "utf8"));
  assert.equal(settings.name_voice, updated.voice);
  assert.equal(settings.name_game, updated.game);
  assert.equal(settings.name_music, updated.music);
  assert.ok(!errors.includes("no UI task handler"), errors);
  console.log("fader-names: real plugin received names, ignored malformed names, updated live and cached Unicode labels");
} finally {
  await Promise.all(children.map(async (child) => {
    if (child.exitCode === null && child.signalCode === null) {
      child.kill("SIGTERM");
      await once(child, "exit");
    }
  }));
  await rm(temp, { recursive: true, force: true });
}
