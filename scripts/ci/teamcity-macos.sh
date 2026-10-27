#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# TeamCity (macOS agent): build faders.plugin (universal), run the headless end-to-end test,
# package the .pkg installer and a zip of the plugin (signed when the MACOS_* identities are set).
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
./scripts/build-macos.sh # universal plugin (arm64 + x86_64) and the arm64 headless host

# libobs behaviour the virtual mixer relies on (PROTOCOL.md "Virtual mixer"): every check must pass.
build/faders-mixcheck > mixcheck.log 2>&1 || { grep -E "^(PASS|FAIL|SKIP)" mixcheck.log; echo "##teamcity[buildProblem description='faders-mixcheck: a libobs mixer assumption failed']"; exit 1; }
grep -E "^(PASS|FAIL|SKIP)" mixcheck.log

# The hub's update notice is kept in the settings (test/update-notice.mjs runs its own mock hub).
node test/update-notice.mjs

node test/mock-hub.mjs --port "$PORT" > hub.log 2>&1 &
HUB=$!
HOST=""
trap 'kill $HUB ${HOST:-} 2>/dev/null || true' EXIT
for _ in $(seq 1 30); do curl -s "localhost:$PORT/health" | grep -q ok && break; sleep 1; done

M="$PWD/test/media"
build/faders-headless "ws://127.0.0.1:$PORT/ingest/ci_channel" wrong-key "$M/voice.wav" "$M/game.wav" "$M/music.wav" 5 > host-badkey.log 2>&1 || true
grep -q "HTTP 401" host-badkey.log || { echo "##teamcity[buildProblem description='wrong key was not rejected with HTTP 401']"; exit 1; }

# The wire as a listener sees it: three named faders plus Everything else, which is silent while
# every source is in a fader (the subtraction is exact)...
build/faders-headless "ws://127.0.0.1:$PORT/ingest/ci_channel" dev-token "$M/voice.wav" "$M/game.wav" "$M/music.wav" 30 > host.log 2>&1 &
HOST=$!
sleep 6
node test/plugin-listener.mjs --server "ws://127.0.0.1:$PORT" --channel ci_channel --seconds 15 --stems 4 --names "Voice,Game,Music,rest"
wait $HOST || true

# ...and carries a source that is in no fader.
FADERS_EXTRA_WAV="$M/voice.wav" build/faders-headless "ws://127.0.0.1:$PORT/ingest/ci_channel" dev-token "$M/voice.wav" "$M/game.wav" "$M/music.wav" 30 > host-rest.log 2>&1 &
HOST=$!
sleep 6
node test/plugin-listener.mjs --server "ws://127.0.0.1:$PORT" --channel ci_channel --seconds 15 --stems 4 --names "Voice,Game,Music,rest" --rest-audible
wait $HOST || true
grep -q '"mixMismatch"' hub.log && { echo "##teamcity[buildProblem description='mixMismatch reported although every fader source is on the streamed track']"; exit 1; }

# Every case of the mixing matrix through the real plugin (test/mixtest.c), then the wire with
# five faders as a listener sees it.
build/faders-mixtest "ws://127.0.0.1:$PORT/ingest/mix_channel" dev-token > mixtest.log 2>&1 || { grep -E "^(PASS|FAIL|SKIP)" mixtest.log; echo "##teamcity[buildProblem description='faders-mixtest: a virtual-mixer case failed']"; exit 1; }
grep -E "^(PASS|FAIL|SKIP)" mixtest.log
grep -q '"mixMismatch":"injected"' hub.log || { echo "##teamcity[buildProblem description='the injected mismatch was not reported to the hub']"; exit 1; }
build/faders-mixtest "ws://127.0.0.1:$PORT/ingest/mix_wire" dev-token wire > mixtest-wire.log 2>&1 &
HOST=$!
sleep 5
node test/plugin-listener.mjs --server "ws://127.0.0.1:$PORT" --channel mix_wire --seconds 12 --stems 5 --names "Voice,Game,Music,Discord,rest" --rest-audible
wait $HOST || true

# The plan's allowance folds extra faders into Everything else (mock hub: maxStems 2).
F_PORT=$((PORT + 2))
node test/mock-hub.mjs --port "$F_PORT" --kbps 128 --max-stems 2 > hub-fold.log 2>&1 &
FHUB=$!
trap 'kill $HUB $FHUB ${HOST:-} 2>/dev/null || true' EXIT
for _ in $(seq 1 30); do curl -s "localhost:$F_PORT/health" | grep -q ok && break; sleep 1; done
build/faders-mixtest "ws://127.0.0.1:$F_PORT/ingest/mix_fold" dev-token fold > mixtest-fold.log 2>&1 || { grep -E "^(PASS|FAIL|SKIP)" mixtest-fold.log; echo "##teamcity[buildProblem description='faders-mixtest: the plan cap did not fold faders']"; exit 1; }
grep -E "^(PASS|FAIL|SKIP)" mixtest-fold.log
kill $FHUB 2>/dev/null || true

# The hub sets the channel's audio quality; the plugin follows it.
Q_PORT=$((PORT + 1))
node test/mock-hub.mjs --port "$Q_PORT" --kbps 256 > hub-quality.log 2>&1 &
QHUB=$!
trap 'kill $HUB $QHUB ${FHUB:-} ${HOST:-} ${QHOST:-} 2>/dev/null || true' EXIT
for _ in $(seq 1 30); do curl -s "localhost:$Q_PORT/health" | grep -q ok && break; sleep 1; done
build/faders-headless "ws://127.0.0.1:$Q_PORT/ingest/ci_channel" dev-token "$M/voice.wav" "$M/game.wav" "$M/music.wav" 30 > host-quality.log 2>&1 &
QHOST=$!
sleep 6
node test/plugin-listener.mjs --server "ws://127.0.0.1:$Q_PORT" --channel ci_channel --seconds 15 --kbps 256 --stems 4

rm -f faders-obs-*-macos.zip faders-obs-*-macos.pkg # earlier versions' installers stay in the checkout
./scripts/package-macos.sh
