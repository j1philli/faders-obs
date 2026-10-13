#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# TeamCity (Linux agent, Docker ubuntu:24.04): cross-compile faders.dll with MinGW-w64 and check
# it statically (exports + every OBS import exists in the release DLLs). Can't run it here:
# the runtime test happens on the Windows agent.
set -euo pipefail
cd "$(dirname "$0")/../.."
export DEBIAN_FRONTEND=noninteractive
if ! command -v x86_64-w64-mingw32-gcc >/dev/null; then
  apt-get update -qq
  apt-get install -y -qq --no-install-recommends mingw-w64 cmake make llvm git curl unzip ca-certificates python3 >/dev/null
fi
./scripts/cross-windows.sh
python3 scripts/ci/check-windows-dll.py build-win/faders.dll .deps/win/lib
python3 - <<'PY'
import zipfile, pathlib
with zipfile.ZipFile("faders-obs-windows-cross.zip", "w", zipfile.ZIP_DEFLATED) as z:
    z.write("build-win/faders.dll", "faders/bin/64bit/faders.dll")
    for p in pathlib.Path("data").rglob("*"):
        if p.is_file():
            z.write(p, f"faders/data/{p.relative_to('data')}")
PY
