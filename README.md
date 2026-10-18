# Faders

A plugin for [OBS Studio](https://obsproject.com). Faders isn't affiliated with or endorsed by the OBS Project.

Sends your **voice**, **game** and **music** as three separate layers to the Faders hub so
viewers can mix them. Your Twitch stream is not touched.

**Get it:** download the installer from [faders.tv/streamers](https://faders.tv/streamers/) or the
[latest release](https://github.com/j1philli/faders-obs/releases/latest), and get your key from the
[Faders portal](https://faders.tv/portal/). Setup guide: [faders.tv/docs/streamer-setup](https://faders.tv/docs/streamer-setup/).

- **Tools → Faders** opens the settings (OBS's own properties window, no custom UI code):
  key, which mixer track is voice / game / music, "only send while streaming". The server
  defaults to `wss://hub.faders.tv/ingest` (the key identifies the channel); it's editable under
  *Advanced* for testing against another hub.
- Taps OBS mixer tracks with `obs_add_raw_audio_callback` (default tracks 2/3/4). Assign
  sources in *Edit → Advanced Audio Properties*.
- Opus 64 kbps, 20 ms frames, 500 ms chunks (`PROTOCOL.md`). ~190 kbps total upload,
  and **0 while nobody is listening**: the hub tells the plugin its listener count, the
  plugin keeps a 60 s backlog and replays it when the first listener arrives.
- The key goes in an `Authorization: Bearer` header. Reconnects back off 1 s → 30 s, so a wrong
  key doesn't hammer the hub. Status shows in the settings window and the OBS log (`[faders]`).
- Heartbeat: pings the hub every 20 s and reconnects after 60 s without a reply, so a connection
  silently dropped by a router or proxy recovers on its own.
- When the hub reports a newer release, **Tools → Faders** shows "Update available" with a link
  to the download page. The plugin never updates itself.
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

## Install

Download the installer for your platform from the
[latest release](https://github.com/j1philli/faders-obs/releases/latest) and run it, then restart
OBS and open **Tools → Faders**:

- **macOS 13+** (Apple silicon and Intel): `faders-obs-<version>-macos.pkg`. Installs for your
  user into `~/Library/Application Support/obs-studio/plugins`. Signed with a Developer ID and
  notarized by Apple.
- **Windows 10/11 x64**: `faders-obs-<version>-windows.exe`. Installs into
  `%ProgramData%\obs-studio\plugins\faders`; uninstall from Settings → Apps. The DLL is placed
  both where OBS 33 looks (`faders\faders.dll`) and where OBS 32 looks (`faders\bin\64bit\`), so
  one installer works with both and OBS 33 doesn't flag it as a legacy plugin. The plugin DLL,
  installer and uninstaller are code-signed (Azure Trusted Signing).

## Build (macOS)

```sh
./scripts/fetch-deps.sh     # OBS 32.2.2 (Apple + Intel releases), matching headers, SIMDe, static libopus
./scripts/build-macos.sh    # universal build/faders.plugin (+ arm64 headless test host); FADERS_INTEL=0 for arm64 only
./scripts/install-macos.sh  # → ~/Library/Application Support/obs-studio/plugins
./scripts/package-macos.sh  # → faders-obs-<version>-macos.pkg (signs/notarizes when MACOS_* are set; see the script)
```

## Build (Windows x64, MSVC)

From a "Developer PowerShell for VS 2022":

```powershell
.\scripts\fetch-deps-windows.ps1              # OBS 32.2.2 DLLs → .lib, headers, static opus
cmake -S . -B build -A x64; cmake --build build --config Release
.\scripts\install-windows.ps1                 # (as admin) → %ProgramData%\obs-studio\plugins\faders
```

Cross-compiling from macOS or Linux (how releases are built): `./scripts/cross-windows.sh` (MinGW-w64)
→ `build-win/faders.dll`; `scripts/ci/check-windows-dll.py` verifies its exports and imports.

Installer: `makensis -DVERSION=0.1.0 -DDLL=<path>/faders.dll -DOUTFILE=<path>/faders-obs-0.1.0-windows.exe installer/windows/faders.nsi`
(NSIS 3, on Windows or Linux). The installer art in `installer/` is generated from the Faders
brand kit.

CI runs on TeamCity (`scripts/ci/`): `teamcity-macos.sh` builds natively, runs the end-to-end
test below against `test/mock-hub.mjs` and packages (zip + `.pkg`; on `main` also Developer ID
signed and notarized). `teamcity-linux-cross.sh` (in `ubuntu:24.04`) cross-builds the Windows
release with MinGW, checks the DLL statically and builds the `.exe` installer; on `main` it signs
the DLL, installer and uninstaller with Azure Trusted Signing (`scripts/sign-windows.sh`).
The signed `main` builds become a draft GitHub release, which is published by hand.

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

## Not verified yet

Released as 0.1.0. These parts haven't been tested by hand yet; bug reports are welcome
(GitHub issues, or the Faders Discord: discord.gg/TA7Xd8CEAz).

- **Windows in the OBS app:** the DLL cross-compiles, exports exactly the 8 functions
  libobs looks up, and every one of its 60 OBS imports exists in the real OBS 32.2.2 DLLs, but it
  has only run in libobs headlessly (CI), not yet clicked through in the OBS app on Windows.
- **Intel Macs:** the universal build links each architecture against its own OBS release, but
  the x86_64 half hasn't been run yet (no Intel Mac / Rosetta in CI).
- **GUI check in real OBS:** the Tools menu + properties window haven't been clicked through in
  the OBS app yet (the headless test bypasses the frontend).
- **Linux:** not supported yet. It needs a libcurl transport (`src/ws-curl.c`); everything else
  is shared.

## License

GPL-2.0-or-later (see `LICENSE`), the same as OBS Studio, which this plugin links against.
Bundled: Opus (BSD-3-Clause). The Faders hub, website and browser extension are separate
programs that talk to this plugin over the network protocol in `PROTOCOL.md`.
