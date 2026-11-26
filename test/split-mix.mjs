// SPDX-License-Identifier: GPL-2.0-or-later
// The split fader through the real plugin and libobs, as the hub sees it (PROTOCOL-SPLIT.md §2–3).
// Runs test/mock-hub.mjs (Pro, Twitch and YouTube) and build/faders-split-mix (which checks the
// audio itself and prints PASS/FAIL lines), answers its "STEP" lines (the hub's hello changes
// for a downgrade and an upgrade) and checks the stems and status messages the hub got:
//   split    the split stems message, byte for byte; the status's split warnings; after the
//            downgrade, today's stems message exactly; after the upgrade, the split's again
//   cap      maxStems 2 counts the split fader once (and splitMaxFaders 2 allows it)
//   quality  splitMaxFaders 2 with 3 faders: today's stems message, byte for byte
//   nosplit  no split fader: today's stems message, byte for byte, and no split in the status
// Run after building: node test/split-mix.mjs [split|cap|nosplit ...]
import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { once } from "node:events";
import { mkdtemp, readFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { resolve } from "node:path";

const root = resolve(import.meta.dirname, "..");
const temp = await mkdtemp(`${tmpdir()}/faders-split-mix-`);
const basePort = Number(process.env.FADERS_SPLIT_MIX_PORT || 18795);
const sleep = (ms) => new Promise((done) => setTimeout(done, ms));
const golden = async (name) => (await readFile(resolve(root, "test/split", name), "utf8")).trim();
let failures = 0;
const check = (ok, name, detail) => {
  console.log(`${ok ? "PASS" : "FAIL"}  ${name.padEnd(56)} ${detail ?? ""}`);
  if (!ok) failures++;
};

function start(command, args) {
  const child = spawn(command, args, { cwd: root, env: { ...process.env, TMPDIR: temp, TEMP: temp }, stdio: ["ignore", "pipe", "pipe"] });
  child.out = "";
  child.err = "";
  child.stdout.on("data", (d) => (child.out += d));
  child.stderr.on("data", (d) => (child.err += d));
  return child;
}
async function waitFor(child, text, ms = 20000) {
  for (let t = 0; t < ms; t += 50) {
    if (child.out.includes(text)) return true;
    if (child.exitCode !== null) return child.out.includes(text);
    await sleep(50);
  }
  return false;
}
// The plugin's text messages to the hub so far (parsed), in order.
const messages = (hub, channel) =>
  hub.out
    .split("\n")
    .filter((l) => l.includes(`plugin ${channel}: {`))
    .map((l) => l.slice(l.indexOf(`plugin ${channel}: `) + `plugin ${channel}: `.length));
const lastOf = (hub, channel, type) => messages(hub, channel).filter((t) => t.startsWith(`{"type":"${type}"`)).at(-1);
async function hello(port, channel, plan, youtube) {
  const res = await fetch(`http://127.0.0.1:${port}/hello`, { method: "POST", body: JSON.stringify({ channel, plan, twitch: true, youtube }) });
  assert.equal(res.status, 200);
}

async function run(mode, port, hubArgs, onStep) {
  const channel = `split_${mode}`;
  const hub = start(process.execPath, ["test/mock-hub.mjs", "--port", String(port), ...hubArgs]);
  assert.ok(await waitFor(hub, "mock hub on"), hub.err);
  const host = start(resolve(root, "build/faders-split-mix"), [`ws://127.0.0.1:${port}/ingest/${channel}`, "dev-token", mode]);
  let seen = 0;
  while (host.exitCode === null) {
    const steps = [...host.out.matchAll(/^STEP (\S+)$/gm)].map((m) => m[1]);
    for (; seen < steps.length; seen++) await onStep(steps[seen], { hub, channel, port });
    await sleep(50);
  }
  for (const line of host.out.split("\n")) if (/^(PASS|FAIL)/.test(line)) console.log(line);
  check(host.exitCode === 0, `${mode}: the plugin's own checks`, host.exitCode === 0 ? "" : `exit ${host.exitCode}\n${host.err.slice(-2000)}`);
  hub.kill("SIGTERM");
  await once(hub, "exit");
}

const modes = process.argv.slice(2).length ? process.argv.slice(2) : ["split", "cap", "quality", "nosplit"];
const pro = ["--plan", "pro", "--youtube", "--kbps", "128"];
const splitOn = '{"type":"stems","stems":[{"id":0,"name":"Voice"},{"id":1,"name":"Music","fader":"f1","platform":"twitch"},{"id":2,"name":"Music","fader":"f1","platform":"youtube"},{"id":3,"name":"Everything else","rest":true,"platform":"twitch"},{"id":4,"name":"Everything else","rest":true,"platform":"youtube"}]}';
const splitOff = '{"type":"stems","stems":[{"id":0,"name":"Voice"},{"id":1,"name":"Music"},{"id":2,"name":"Everything else","rest":true}]}';

if (modes.includes("split"))
  await run("split", basePort, pro, async (step, { hub, channel, port }) => {
    if (step === "check-stems-on") {
      await sleep(600);
      const stems = lastOf(hub, channel, "stems");
      check(stems === splitOn, "split: stems message (split on), byte for byte", stems);
    } else if (step === "check-status-doubling") {
      await sleep(1200);
      const status = lastOf(hub, channel, "status");
      let parsed = {};
      try {
        parsed = JSON.parse(status);
      } catch {}
      check(parsed.split?.youtubeTrack === 3 && parsed.split?.warnings?.includes("twitch-source-on-youtube-track") && parsed.streamAudio === "ok" && parsed.track === 1,
        "split: status carries split.youtubeTrack and the warning", status);
    } else if (step === "hello-free") {
      await hello(port, channel, "free", true);
    } else if (step === "check-stems-off") {
      await sleep(600);
      const stems = lastOf(hub, channel, "stems");
      check(stems === splitOff, "split: after the downgrade, today's stems message exactly", stems);
      const status = lastOf(hub, channel, "status");
      check(status !== undefined && !status.includes('"split"'), "split: after the downgrade, no split in the status", status);
    } else if (step === "hello-pro") {
      await hello(port, channel, "pro", true);
    } else if (step === "done") {
      check(lastOf(hub, channel, "stems") === splitOn, "split: the upgrade announces the split again", lastOf(hub, channel, "stems"));
    }
  });

if (modes.includes("cap"))
  await run("cap", basePort + 1, [...pro, "--max-stems", "2", "--split-max-faders", "2"], async (step, { hub, channel }) => {
    if (step === "done") check(lastOf(hub, channel, "stems") === splitOn, "cap: maxStems 2 counts the split fader once; splitMaxFaders 2 allows it", lastOf(hub, channel, "stems"));
  });

if (modes.includes("quality"))
  await run("quality", basePort + 3, [...pro, "--split-max-faders", "2"], async (step, { hub, channel }) => {
    if (step === "done") {
      const want = '{"type":"stems","stems":[{"id":0,"name":"Voice"},{"id":1,"name":"Music"},{"id":2,"name":"Game"},{"id":3,"name":"Everything else","rest":true}]}';
      const stems = messages(hub, channel).filter((t) => t.startsWith('{"type":"stems"'));
      check(stems.length > 0 && stems.every((s) => s === want), "quality: over splitMaxFaders, no split stems are ever sent", stems.join(" | "));
      const status = messages(hub, channel).filter((t) => t.startsWith('{"type":"status"'));
      check(status.every((s) => !s.includes('"split"')), "quality: no split in the status", status.join(" | "));
    }
  });

if (modes.includes("nosplit"))
  await run("nosplit", basePort + 2, pro, async (step, { hub, channel }) => {
    if (step === "check-stems-nosplit") {
      await sleep(300);
      const want = await golden("stems-off.json");
      const stems = messages(hub, channel).filter((t) => t.startsWith('{"type":"stems"'));
      check(stems.length > 0 && stems.every((s) => s === want), "nosplit: today's stems message, byte for byte (golden)", stems.join(" | "));
      const status = messages(hub, channel).filter((t) => t.startsWith('{"type":"status"'));
      check(status.length > 0 && status.every((s) => !s.includes('"split"')), "nosplit: no split object in the status", status.join(" | "));
    }
  });

console.log(failures ? "FAILED" : "ALL PASSED");
process.exit(failures ? 1 : 0);
