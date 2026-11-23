#!/usr/bin/env node
// SPDX-License-Identifier: GPL-2.0-or-later
// A WebSocket server that sends every client a text message as fast as it can take them, so a
// client always has a message callback in flight (test/wstest.c).
//   node test/ws-flood.mjs --port 18797
import { createServer } from "node:http";
import { WebSocketServer } from "ws";

const port = Number(process.argv[process.argv.indexOf("--port") + 1]) || 18797;
const http = createServer((req, res) => res.end(req.url === "/health" ? "ok" : ""));
const wss = new WebSocketServer({ server: http });
wss.on("connection", (ws) => {
  let n = 0;
  // A bounded batch per turn, so one slow or vanished client never holds up the others.
  const pump = () => {
    if (ws.readyState !== ws.OPEN) return;
    for (let i = 0; i < 20 && ws.bufferedAmount < 1 << 16; i++) ws.send(`{"type":"listeners","n":${n++ % 100}}`);
    setTimeout(pump, 2);
  };
  pump();
  ws.on("error", () => {});
});
http.listen(port, "127.0.0.1", () => console.log(`ws-flood on ${port}`));
