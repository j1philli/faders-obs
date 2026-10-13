#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# TeamCity (macOS agent): build faders.plugin, run the headless end-to-end test, package.
# Agent needs: Xcode command line tools, cmake, git, node 22.
set -euo pipefail
cd "$(dirname "$0")/../.."
PORT="${FADERS_TEST_PORT:-18787}"

npm ci --no-audit --no-fund
./scripts/fetch-deps.sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release

node test/mock-hub.mjs --port "$PORT" > hub.log 2>&1 &
HUB=$!
HOST=""
trap 'kill $HUB ${HOST:-} 2>/dev/null || true' EXIT
for _ in $(seq 1 30); do curl -s "localhost:$PORT/health" | grep -q ok && break; sleep 1; done

M="$PWD/test/media"
build/faders-headless "ws://127.0.0.1:$PORT/ingest/ci_channel" wrong-key "$M/voice.wav" "$M/game.wav" "$M/music.wav" 5 > host-badkey.log 2>&1 || true
grep -q "HTTP 401" host-badkey.log || { echo "##teamcity[buildProblem description='wrong key was not rejected with HTTP 401']"; exit 1; }

build/faders-headless "ws://127.0.0.1:$PORT/ingest/ci_channel" dev-token "$M/voice.wav" "$M/game.wav" "$M/music.wav" 40 > host.log 2>&1 &
HOST=$!
sleep 6
node test/plugin-listener.mjs --server "ws://127.0.0.1:$PORT" --channel ci_channel --seconds 20

rm -f faders-obs-macos.zip
(cd build && zip -qry ../faders-obs-macos.zip faders.plugin)
