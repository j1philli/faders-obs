// SPDX-License-Identifier: GPL-2.0-or-later
// Real plugin/libobs test: the hub's update notice is kept in the local settings, so Tools → Faders
// can show it after the connection ends; a malformed version is ignored.
// Run after building: node test/update-notice.mjs
import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { once } from "node:events";
import { mkdtemp, readFile, readdir, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { resolve } from "node:path";

const root = resolve(import.meta.dirname, "..");
const temp = await mkdtemp(`${tmpdir()}/faders-plugin-update-`);
const port = Number(process.env.FADERS_UPDATE_TEST_PORT || 18790);
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
// One stream against a hub that announces `latest`; returns the saved settings afterwards.
async function stream(latest) {
  output = "";
  const hub = start(process.execPath, ["test/mock-hub.mjs", "--port", String(port), "--update", latest]);
  await waitFor("mock hub on");
  const host = start(resolve(root, "build/faders-headless"), [`ws://127.0.0.1:${port}/ingest/update`, "dev-token", ...["voice", "game", "music"].map((name) => `${root}/test/media/${name}.wav`), "3"]);
  const [code] = await once(host, "exit");
  assert.equal(code, 0, errors);
  await stop(hub);
  const files = await readdir(temp, { recursive: true });
  const settingsPath = files.find((name) => name.endsWith("settings.json"));
  assert.ok(settingsPath, "plugin saved its settings");
  return JSON.parse(await readFile(resolve(temp, settingsPath), "utf8"));
}
try {
  assert.equal((await stream("1.2.x")).update_available, undefined, "malformed version ignored");
  assert.equal((await stream("99.0.0")).update_available, "99.0.0");
  assert.ok(output.includes("update available: 99.0.0"), output);
  console.log("update-notice: real plugin kept the hub's update notice after the stream and ignored a malformed version");
} finally {
  await Promise.all(children.map(stop));
  await rm(temp, { recursive: true, force: true });
}
