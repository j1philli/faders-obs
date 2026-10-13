#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Builds build/faders.plugin as a universal binary (Apple Silicon + Intel) plus the arm64
# headless test host. OBS ships separate Apple and Intel releases, so each architecture links
# against its own libobs and the two binaries are merged with lipo. FADERS_INTEL=0: arm64 only.
# Run scripts/fetch-deps.sh first.
set -euo pipefail
cd "$(dirname "$0")/.."

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build --config Release

if [ "${FADERS_INTEL:-1}" = 1 ]; then
  cmake -S . -B build-x86_64 -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=x86_64 \
    -DOBS_APP="$PWD/.deps/OBS-x86_64.app" -DOPUS_ROOT="$PWD/.deps/opus-x86_64" >/dev/null
  cmake --build build-x86_64 --config Release --target faders
  bin=build/faders.plugin/Contents/MacOS/faders
  lipo -create "$bin" build-x86_64/faders.plugin/Contents/MacOS/faders -output "$bin.universal"
  mv "$bin.universal" "$bin"
  # Merging invalidates the linker's ad-hoc signature; re-sign (release builds re-sign with a
  # Developer ID in package-macos.sh).
  codesign --force --sign - build/faders.plugin
fi
echo "built build/faders.plugin ($(lipo -archs build/faders.plugin/Contents/MacOS/faders))"
