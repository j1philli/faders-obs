#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# TeamCity (Linux agent, Docker ubuntu:24.04): cross-compile faders.dll with MinGW-w64 and check
# it statically (exports + every OBS import exists in the release DLLs), and build the NSIS
# installer. This is the release build. With the AZURE_* signing variables set (main only, see
# scripts/sign-windows.sh) the DLL, installer and uninstaller are signed with Azure Trusted Signing.
set -euo pipefail
cd "$(dirname "$0")/../.."
# The checkout persists between builds: drop earlier versions' installers so only this one is an artifact.
rm -f faders-obs-*-windows.exe faders-obs-windows-cross.zip
export DEBIAN_FRONTEND=noninteractive
if ! command -v x86_64-w64-mingw32-gcc >/dev/null; then
  apt-get update -qq
  apt-get install -y -qq --no-install-recommends mingw-w64 cmake make llvm git curl unzip ca-certificates python3 nsis >/dev/null
fi
sign=()
if [ -n "${AZURE_SIGNING_ACCOUNT:-}" ]; then
  command -v java >/dev/null || apt-get install -y -qq --no-install-recommends default-jre-headless >/dev/null
  sign=(-DSIGN="$PWD/scripts/sign-windows.sh")
fi
./scripts/cross-windows.sh
python3 scripts/ci/check-windows-dll.py build-win/faders.dll .deps/win/lib
[ ${#sign[@]} -eq 0 ] || ./scripts/sign-windows.sh build-win/faders.dll
python3 - <<'PY'
import zipfile, pathlib
with zipfile.ZipFile("faders-obs-windows-cross.zip", "w", zipfile.ZIP_DEFLATED) as z:
    z.write("build-win/faders.dll", "faders/faders.dll")  # OBS 33+
    z.write("build-win/faders.dll", "faders/bin/64bit/faders.dll")  # OBS 32
    for p in pathlib.Path("data").rglob("*"):
        if p.is_file():
            z.write(p, f"faders/data/{p.relative_to('data')}")
PY

VERSION="${FADERS_VERSION:-$(sed -n 's/^project(faders VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)}"
makensis -V2 ${sign[@]+"${sign[@]}"} -DVERSION="$VERSION" -DDLL="$PWD/build-win/faders.dll" -DOUTFILE="$PWD/faders-obs-$VERSION-windows.exe" installer/windows/faders.nsi
echo "built faders-obs-$VERSION-windows.exe ($([ ${#sign[@]} -gt 0 ] && echo signed || echo unsigned))"
