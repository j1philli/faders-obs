// SPDX-License-Identifier: GPL-2.0-or-later
// Minimal stand-in for the Faders hub, for testing the plugin (CI or local). Speaks PROTOCOL.md:
//   /ingest/<channel>  plugin uplink (Bearer or ?token= must equal --token), receives
//                      {"type":"listeners","n":N} whenever the listener count changes, and
//                      {"type":"quality","kbps":N,"maxStems":M} on connect when --kbps is given,
//                      {"type":"update","latest":V} on connect when --update is given
//   /listen/<channel>  viewers: the plugin's {"type":"stems"} (if any), {"type":"backlog"} + up to
//                      60 s of chunks, {"type":"live"}, then live
// Every text message from the plugin is logged ("plugin <channel>: ..."), for tests to grep.
//
//   node test/mock-hub.mjs [--port 8787] [--token dev-token] [--kbps 256] [--max-stems 4] [--update 0.9.0]
import { createServer } from "node:http";
import { parseArgs } from "node:util";
import { WebSocketServer } from "ws";

const { values: args } = parseArgs({ options: { port: { type: "string", default: "8787" }, token: { type: "string", default: "dev-token" }, kbps: { type: "string" }, "max-stems": { type: "string" }, update: { type: "string" } } });
const RING_MS = 60_000;
const t0 = Date.now();
const channels = new Map(); // name -> { ingest, listeners:Set, ring:[], lastSeq }
const chan = (name) => {
  if (!channels.has(name)) channels.set(name, { ingest: null, listeners: new Set(), ring: [], lastSeq: -1, stems: null });
  return channels.get(name);
};
const notify = (c) => c.ingest?.send(JSON.stringify({ type: "listeners", n: c.listeners.size }));

const server = createServer(async (req, res) => {
  if (req.url === "/health") return res.end("ok");
  res.writeHead(404).end();
});
const wss = new WebSocketServer({ noServer: true });

server.on("upgrade", (req, socket, head) => {
  const url = new URL(req.url, "http://localhost");
  const [, role, name] = url.pathname.split("/");
  if (!["ingest", "listen"].includes(role) || !/^[a-z0-9_]{1,25}$/.test(name ?? "")) return socket.destroy();
  if (role === "ingest") {
    const token = req.headers.authorization?.replace(/^Bearer\s+/i, "") ?? url.searchParams.get("token");
    if (token !== args.token) {
      socket.end("HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n\r\n");
      return;
    }
  }
  wss.handleUpgrade(req, socket, head, (ws) => {
    const c = chan(name);
    if (role === "ingest") {
      c.ingest?.close(4000, "replaced by new ingest connection");
      c.ingest = ws;
      if (args.kbps) ws.send(JSON.stringify({ type: "quality", kbps: Number(args.kbps), ...(args["max-stems"] ? { maxStems: Number(args["max-stems"]) } : {}) }));
      if (args.update) ws.send(JSON.stringify({ type: "update", latest: args.update }));
      notify(c);
      ws.on("message", (data, isBinary) => {
        if (!isBinary) {
          const text = data.toString();
          if (text === "ping") return ws.send("pong");
          console.log(`[${((Date.now() - t0) / 1000).toFixed(1)}s] plugin ${name}: ${text}`);
          let msg;
          try {
            msg = JSON.parse(text);
          } catch {
            return;
          }
          if (msg.type === "stems") {
            c.stems = text;
            for (const l of c.listeners) l.send(text);
          }
          return;
        }
        if (data.length < 16) return;
        const seq = data.readUInt32LE(2);
        const ts = data.readDoubleLE(6);
        if (seq <= c.lastSeq) return; // backlog replays after a pause
        c.lastSeq = seq;
        c.ring.push(data);
        while (c.ring.length && ts - c.ring[0].readDoubleLE(6) > RING_MS) c.ring.shift();
        for (const l of c.listeners) l.send(data);
      });
      ws.on("close", () => c.ingest === ws && (c.ingest = null));
    } else {
      if (c.stems) ws.send(c.stems);
      ws.send(JSON.stringify({ type: "backlog", count: c.ring.length }));
      for (const chunk of c.ring) ws.send(chunk);
      ws.send(JSON.stringify({ type: "live" }));
      c.listeners.add(ws);
      notify(c);
      ws.on("close", () => {
        c.listeners.delete(ws);
        notify(c);
      });
    }
  });
});

server.listen(Number(args.port), "127.0.0.1", () => console.log(`mock hub on ws://127.0.0.1:${args.port} (token "${args.token}")`));
