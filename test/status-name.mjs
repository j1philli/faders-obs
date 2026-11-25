// SPDX-License-Identifier: GPL-2.0-or-later
// Real plugin/libobs test: a source the mixer moves to Everything else is reported to the hub by
// name ({"type":"status",…,"mixMismatch":"<name>"}). Source names are the streamer's: quotes,
// backslashes and long unicode names must still make valid JSON in valid UTF-8, or the hub
// can't read the status at all.
// Run after building: node test/status-name.mjs
import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { once } from "node:events";
import { mkdtemp, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { resolve } from "node:path";

const root = resolve(import.meta.dirname, "..");
const temp = await mkdtemp(`${tmpdir()}/faders-status-name-`);
const port = Number(process.env.FADERS_STATUS_NAME_TEST_PORT || 18791);
const sleep = (ms) => new Promise((done) => setTimeout(done, ms));
const children = [];
let output = "", errors = "";
function start(command, args, env = {}) {
  const child = spawn(command, args, { cwd: root, env: { ...process.env, TMPDIR: temp, TEMP: temp, ...env }, stdio: ["ignore", "pipe", "pipe"] });
  children.push(child);
  child.stdout.on("data", (data) => output += data);
  child.stderr.on("data", (data) => errors += data);
  return child;
}
async function waitFor(text) {
  for (let i = 0; i < 150; i++) {
    if (output.includes(text)) return;
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
// One stream with a mismatched source called `name`; returns what the hub got from the plugin:
// each text message parsed, or {unparsed: text}.
async function stream(channel, name) {
  output = "";
  const hub = start(process.execPath, ["test/mock-hub.mjs", "--port", String(port)]);
  await waitFor("mock hub on");
  const host = start(resolve(root, "build/faders-headless"), [`ws://127.0.0.1:${port}/ingest/${channel}`, "dev-token", ...["voice", "game", "music"].map((n) => `${root}/test/media/${n}.wav`), "6"], { FADERS_MISMATCH_NAME: name });
  const [code] = await once(host, "exit");
  assert.equal(code, 0, errors);
  await stop(hub);
  const prefix = `plugin ${channel}: `;
  return output.split("\n").filter((line) => line.includes(prefix)).map((line) => {
    const text = line.slice(line.indexOf(prefix) + prefix.length);
    try {
      return JSON.parse(text);
    } catch {
      return { unparsed: text };
    }
  });
}
// The hub must have read a status naming the source (as much of it as the plugin keeps).
function check(messages, name, label) {
  const unparsed = messages.filter((m) => m.unparsed).map((m) => m.unparsed);
  assert.deepEqual(unparsed, [], `${label}: every message from the plugin is JSON`);
  const status = messages.find((m) => m.type === "status" && m.mixMismatch);
  assert.ok(status, `${label}: a status with mixMismatch reached the hub\n${output}`);
  assert.ok(name.startsWith(status.mixMismatch) && status.mixMismatch.length >= 20, `${label}: mixMismatch "${status.mixMismatch}" is the start of "${name}"`);
  assert.ok(!status.mixMismatch.includes("\uFFFD"), `${label}: mixMismatch has no broken character`);
  console.log(`PASS  ${label}: hub read mixMismatch ${JSON.stringify(status.mixMismatch)}`);
}
try {
  const quoted = 'Music "Lo-fi" \\ VB-Cable';
  check(await stream("status_quoted", quoted), quoted, "quotes and a backslash");
  // 62 ASCII bytes, then 2-byte characters: the plugin's 63-byte copy ends inside one.
  const long = "Music from Voicemeeter Aux B2 via the VB-Audio Virtual Cable: " + "éééééééé";
  check(await stream("status_long", long), long, "a long unicode name");
  console.log("status-name: the hub could read every mismatch report");
} finally {
  await Promise.all(children.map(stop));
  await rm(temp, { recursive: true, force: true });
}
