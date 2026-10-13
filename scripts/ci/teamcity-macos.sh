#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# TeamCity (macOS agent): build faders.plugin, run the headless end-to-end test, package.
# Agent needs: Xcode command line tools, git, node 22 (CMake is fetched if missing).
set -euo pipefail
cd "$(dirname "$0")/../.."
PORT="${FADERS_TEST_PORT:-18787}"

# Bring our own CMake if the agent has none (pinned version, checksum-verified), so the build
# doesn't depend on what's installed on a shared Mac.
if ! command -v cmake >/dev/null; then
  CMAKE_VERSION=4.4.4
  CMAKE_SHA256=4b7b73704b1db9b374e5c9ab8e17ac6148b817b6396ee75cd94a852cbac9d305
  CMAKE_DIR="$PWD/.deps/cmake-$CMAKE_VERSION"
  if [ ! -x "$CMAKE_DIR/CMake.app/Contents/bin/cmake" ]; then
    mkdir -p .deps
    curl -fsSL -o .deps/cmake.tar.gz "https://github.com/Kitware/CMake/releases/download/v$CMAKE_VERSION/cmake-$CMAKE_VERSION-macos-universal.tar.gz"
    echo "$CMAKE_SHA256  .deps/cmake.tar.gz" | shasum -a 256 -c -
    mkdir -p "$CMAKE_DIR" && tar xzf .deps/cmake.tar.gz -C "$CMAKE_DIR" --strip-components 1 && rm .deps/cmake.tar.gz
  fi
  export PATH="$CMAKE_DIR/CMake.app/Contents/bin:$PATH"
fi
cmake --version | head -1

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
