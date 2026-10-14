#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Fetches everything the macOS build links against into .deps/ (not committed):
#   OBS.app (libobs + frontend API, official release) · obs-studio headers (same tag)
#   SIMDe (header-only, needed by libobs headers on arm64) · libopus (static, macOS 13 target)
# Apple Silicon deps go in OBS.app / opus; with FADERS_INTEL=1 (default) also the Intel release
# in OBS-x86_64.app / opus-x86_64, for the universal plugin (scripts/build-macos.sh).
set -euo pipefail
OBS_VERSION="${OBS_VERSION:-32.2.2}"
# Every download is pinned: release files by SHA-256 (GitHub's published asset digests), source
# checkouts by commit. Overriding OBS_VERSION means overriding the matching pins too.
OBS_COMMIT="${OBS_COMMIT:-ba2f32bdf791005443988a4955e963663e16b1ed}"
SIMDE_COMMIT="71fd833d9666141edcd1d3c109a80e228303d8d7"
OBS_APPLE_SHA256="${OBS_APPLE_SHA256:-920d6f26703d2df6e4085bd3c1cbed30488325084136c7a6e9e37021fbd6aaf7}"
OBS_INTEL_SHA256="${OBS_INTEL_SHA256:-f8d8afe3dffdc86efa0698c02ff0c997866bac3e6208ddaf56d37108baacf197}"
OPUS_VERSION="1.5.2"
OPUS_SHA256="65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1"
cd "$(dirname "$0")/.."
mkdir -p .deps && cd .deps

fetch_obs() { # <dmg flavour> <destination> <sha256>
  [ -d "$2" ] && return
  curl -fSL -o obs.dmg "https://github.com/obsproject/obs-studio/releases/download/${OBS_VERSION}/OBS-Studio-${OBS_VERSION}-macOS-$1.dmg"
  echo "$3  obs.dmg" | shasum -a 256 -c -
  mnt=$(hdiutil attach -nobrowse -readonly obs.dmg | tail -1 | awk -F'\t' '{print $NF}')
  cp -R "$mnt/OBS.app" "$2"
  hdiutil detach "$mnt" >/dev/null
  rm obs.dmg
}
fetch_obs Apple OBS.app "$OBS_APPLE_SHA256"
[ "${FADERS_INTEL:-1}" = 1 ] && fetch_obs Intel OBS-x86_64.app "$OBS_INTEL_SHA256"
[ -d obs-studio-src ] || git clone --depth 1 --branch "$OBS_VERSION" https://github.com/obsproject/obs-studio.git obs-studio-src
[ -d simde ] || git clone --depth 1 --branch v0.8.2 https://github.com/simd-everywhere/simde.git simde
pinned() { # <checkout> <commit>
  [ "$(git -c safe.directory="*" -C "$1" rev-parse HEAD)" = "$2" ] || { echo "$1 is not at pinned commit $2" >&2; exit 1; }
}
pinned obs-studio-src "$OBS_COMMIT"
pinned simde "$SIMDE_COMMIT"

build_opus() { # <arch> <prefix>
  [ -f "$2/lib/libopus.a" ] && return
  if [ ! -d "opus-${OPUS_VERSION}" ]; then
    curl -fSL -o opus.tar.gz "https://downloads.xiph.org/releases/opus/opus-${OPUS_VERSION}.tar.gz"
    echo "${OPUS_SHA256}  opus.tar.gz" | shasum -a 256 -c -
    tar xzf opus.tar.gz
  fi
  cmake -S "opus-${OPUS_VERSION}" -B "opus-build-$1" -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
    -DCMAKE_OSX_ARCHITECTURES="$1" -DOPUS_BUILD_SHARED_LIBRARY=OFF -DOPUS_BUILD_TESTING=OFF -DOPUS_BUILD_PROGRAMS=OFF \
    -DCMAKE_INSTALL_PREFIX="$PWD/$2" >/dev/null
  cmake --build "opus-build-$1" -j 8 >/dev/null && cmake --install "opus-build-$1" >/dev/null
}
build_opus arm64 opus
[ "${FADERS_INTEL:-1}" = 1 ] && build_opus x86_64 opus-x86_64
echo "deps ready in $(pwd)"
