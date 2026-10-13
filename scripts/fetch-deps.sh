#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Fetches everything the macOS build links against into .deps/ (not committed):
#   OBS.app (libobs + frontend API, official release) · obs-studio headers (same tag)
#   SIMDe (header-only, needed by libobs headers on arm64) · libopus (static, macOS 13 target)
set -euo pipefail
OBS_VERSION="${OBS_VERSION:-32.2.2}"
OPUS_VERSION="1.5.2"
OPUS_SHA256="65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1"
cd "$(dirname "$0")/.."
mkdir -p .deps && cd .deps

if [ ! -d OBS.app ]; then
  curl -fSL -o obs.dmg "https://github.com/obsproject/obs-studio/releases/download/${OBS_VERSION}/OBS-Studio-${OBS_VERSION}-macOS-Apple.dmg"
  mnt=$(hdiutil attach -nobrowse -readonly obs.dmg | tail -1 | awk -F'\t' '{print $NF}')
  cp -R "$mnt/OBS.app" ./OBS.app
  hdiutil detach "$mnt" >/dev/null
  rm obs.dmg
fi
[ -d obs-studio-src ] || git clone --depth 1 --branch "$OBS_VERSION" https://github.com/obsproject/obs-studio.git obs-studio-src
[ -d simde ] || git clone --depth 1 --branch v0.8.2 https://github.com/simd-everywhere/simde.git simde

if [ ! -f opus/lib/libopus.a ]; then
  curl -fSL -o opus.tar.gz "https://downloads.xiph.org/releases/opus/opus-${OPUS_VERSION}.tar.gz"
  echo "${OPUS_SHA256}  opus.tar.gz" | shasum -a 256 -c -
  tar xzf opus.tar.gz
  cmake -S "opus-${OPUS_VERSION}" -B opus-build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
    -DCMAKE_OSX_ARCHITECTURES=arm64 -DOPUS_BUILD_SHARED_LIBRARY=OFF -DOPUS_BUILD_TESTING=OFF -DOPUS_BUILD_PROGRAMS=OFF \
    -DCMAKE_INSTALL_PREFIX="$PWD/opus" >/dev/null
  cmake --build opus-build -j 8 >/dev/null && cmake --install opus-build >/dev/null
fi
echo "deps ready in $(pwd)"
