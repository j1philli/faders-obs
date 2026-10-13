#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Cross-compiles the Windows x64 plugin from macOS/Linux with MinGW-w64 (dev verification —
# releases are built with MSVC in CI). Needs: mingw-w64, llvm (llvm-readobj), cmake.
set -euo pipefail
OBS_VERSION="${OBS_VERSION:-32.2.2}"
OPUS_VERSION="1.5.2"
OPUS_SHA256="65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1"
READOBJ="${READOBJ:-$(command -v llvm-readobj || echo /opt/homebrew/opt/llvm/bin/llvm-readobj)}"
cd "$(dirname "$0")/.."
mkdir -p .deps
[ -d .deps/obs-studio-src ] || git clone -q --depth 1 --branch "$OBS_VERSION" https://github.com/obsproject/obs-studio.git .deps/obs-studio-src
[ -d .deps/simde ] || git clone -q --depth 1 --branch v0.8.2 https://github.com/simd-everywhere/simde.git .deps/simde
sha256() { if command -v sha256sum >/dev/null; then sha256sum -c -; else shasum -a 256 -c -; fi; }
mkdir -p .deps/win/lib && cd .deps/win

if [ ! -f obs/bin/64bit/obs.dll ]; then
  curl -fSL -o obs-win.zip "https://github.com/obsproject/obs-studio/releases/download/${OBS_VERSION}/OBS-Studio-${OBS_VERSION}-Windows-x64.zip"
  unzip -q -o obs-win.zip 'bin/64bit/obs.dll' 'bin/64bit/obs-frontend-api.dll' 'bin/64bit/w32-pthreads.dll' -d obs
  rm obs-win.zip
fi
for d in obs obs-frontend-api w32-pthreads; do
  { echo "LIBRARY $d.dll"; echo "EXPORTS"; "$READOBJ" --coff-exports "obs/bin/64bit/$d.dll" | awk '/Name: /{print "  "$2}'; } > "lib/$d.def"
  x86_64-w64-mingw32-dlltool -d "lib/$d.def" -l "lib/lib$d.dll.a" -D "$d.dll"
done

if [ ! -f opus/lib/libopus.a ]; then
  curl -fSL -o opus.tar.gz "https://downloads.xiph.org/releases/opus/opus-${OPUS_VERSION}.tar.gz"
  echo "${OPUS_SHA256}  opus.tar.gz" | sha256
  tar xzf opus.tar.gz
  cmake -S "opus-${OPUS_VERSION}" -B opus-build -DCMAKE_TOOLCHAIN_FILE="$PWD/../../cmake/mingw-w64-x86_64.cmake" -DCMAKE_BUILD_TYPE=Release \
    -DOPUS_BUILD_SHARED_LIBRARY=OFF -DOPUS_BUILD_TESTING=OFF -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_STACK_PROTECTOR=OFF -DOPUS_FORTIFY_SOURCE=OFF \
    -DCMAKE_INSTALL_PREFIX="$PWD/opus" >/dev/null
  cmake --build opus-build -j 8 >/dev/null && cmake --install opus-build >/dev/null
fi
cd ../..
cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build-win -j 8
echo "built build-win/faders.dll (+ build-win/data/obs-plugins/faders)"
