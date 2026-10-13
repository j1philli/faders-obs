# Faders OBS plugin

Sends your **voice**, **game** and **music** as three separate layers to the Faders hub so
viewers can mix them. Your Twitch stream is not touched.

- **Tools → Faders** opens the settings (OBS's own properties window, no custom UI code):
  server, key, which mixer track is voice / game / music, "only send while streaming".
- Taps OBS mixer tracks with `obs_add_raw_audio_callback` (default tracks 2/3/4). Assign
  sources in *Edit → Advanced Audio Properties*.
- Opus 64 kbps, 20 ms frames, 500 ms chunks (`PROTOCOL.md`). ~190 kbps total upload,
  and **0 while nobody is listening**: the hub tells the plugin its listener count, the
  plugin keeps a 60 s backlog and replays it when the first listener arrives.
- The key goes in an `Authorization: Bearer` header. Reconnects back off 1 s → 30 s, so a wrong
  key doesn't hammer the hub. Status shows in the settings window and the OBS log (`[faders]`).
- Scriptable: procedures `faders_configure(server, key, voice, game, music, only_live)` and
  `faders_status(out status)` on the global proc handler.

## One plugin, two platforms

Everything is shared C except the WebSocket transport (`src/ws.h`):

| | macOS | Windows |
|---|---|---|
| Transport | `ws-apple.m` (NSURLSessionWebSocketTask) | `ws-win.c` (WinHTTP WebSocket API) |
| Shared | `plugin-main.c` (settings, Tools menu, events), `uplink.c` (capture, Opus, chunks, backlog, reconnect) | same |
| Output | `faders.plugin` bundle | `faders.dll` + `data/` |
| libobs | links OBS.app frameworks | import libs generated from the release DLLs |

## Build (macOS, Apple silicon)

```sh
./scripts/fetch-deps.sh                       # OBS.app 32.2.2, matching headers, SIMDe, static libopus
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
./scripts/install-macos.sh                    # → ~/Library/Application Support/obs-studio/plugins
```

## Build (Windows x64, MSVC)

From a "Developer PowerShell for VS 2022":

```powershell
.\scripts\fetch-deps-windows.ps1              # OBS 32.2.2 DLLs → .lib, headers, static opus
cmake -S . -B build -A x64; cmake --build build --config Release
.\scripts\install-windows.ps1                 # (as admin) → %ProgramData%\obs-studio\plugins\faders
```

Cross-compiling from macOS or Linux for a quick check: `./scripts/cross-windows.sh` (MinGW-w64)
→ `build-win/faders.dll`; `scripts/ci/check-windows-dll.py` verifies its exports and imports.

TeamCity runs `scripts/ci/teamcity-windows.ps1` (MSVC build + end-to-end test + packaging),
`scripts/ci/teamcity-macos.sh` and `scripts/ci/teamcity-linux-cross.sh` (in `ubuntu:24.04`). CI (`.github/workflows/ci.yml`) builds both platforms with their native
toolchains and runs the end-to-end test below on each, against `test/mock-hub.mjs`.

## Test without the OBS GUI

`test/mock-hub.mjs` is a small stand-in for the Faders hub (same protocol, see `PROTOCOL.md`):

```sh
npm ci && node test/mock-hub.mjs &                       # ws://127.0.0.1:8787, key "dev-token"
build/faders-headless ws://127.0.0.1:8787/ingest/ci_channel dev-token \
  test/media/voice.wav test/media/game.wav test/media/music.wav 40 &
node test/plugin-listener.mjs --channel ci_channel --seconds 20   # PASS/FAIL
```

### Headless host

`build/faders-headless` runs real libobs, plays three WAVs into tracks 2/3/4, loads the plugin
and configures it through its procedures:

```sh
build/faders-headless ws://localhost:8787/ingest/<channel> <fdr_live_key> \
  test/media/voice.wav test/media/game.wav test/media/music.wav 60
```

Verified: wrong key → "server rejected connection (HTTP 401)" with backoff; correct key →
pauses at 0 listeners, resumes and replays backlog on the first listener; Chrome decodes every
frame and each decoded layer correlates 0.97–0.997 with its own source and ≤0.37 with the others.

## Not done yet

- **Windows runtime is unverified:** the DLL cross-compiles, exports exactly the 8 functions
  libobs looks up, and every one of its 60 OBS imports exists in the real OBS 32.2.2 DLLs, but it
  hasn't been loaded into OBS on a Windows machine yet. The CI workflow does that on a
  windows-2025 runner once the repo is on GitHub.
- **Linux:** needs a libcurl transport (`src/ws-curl.c`); everything else is shared.
- **Intel Macs / universal build:** set `CMAKE_OSX_ARCHITECTURES="arm64;x86_64"` and build opus
  universal.
- **Distribution signing:** the build is ad-hoc signed. Shipping to other Macs needs a Developer
  ID signature + notarization (Apple Developer Program, $99/yr) and an installer `.pkg`.
- **GUI check in real OBS:** the Tools menu + properties window haven't been clicked through in
  the OBS app yet (the headless test bypasses the frontend).

## License

GPL-2.0-or-later (see `LICENSE`), the same as OBS Studio, which this plugin links against.
Bundled: Opus (BSD-3-Clause). The Faders hub, website and browser extension are separate
programs that talk to this plugin over the network protocol in `PROTOCOL.md`.
