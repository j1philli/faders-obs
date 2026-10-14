# SPDX-License-Identifier: GPL-2.0-or-later
# Windows (MSVC) dependencies into .deps\ — run from a "Developer PowerShell for VS":
#   OBS 32.2.2 release (DLLs) → import libs via dumpbin/lib · obs-studio headers · static opus.
$ErrorActionPreference = "Stop"
$ObsVersion = if ($env:OBS_VERSION) { $env:OBS_VERSION } else { "32.2.2" }
# Every download is pinned: release files by SHA-256 (GitHub's published asset digests), source
# checkouts by commit. Overriding OBS_VERSION means overriding the matching pins too.
$ObsWinSha256 = if ($env:OBS_WIN_SHA256) { $env:OBS_WIN_SHA256 } else { "4d6e40e3ab155f56b30de517380566a206d74b63cdf5ad49aa596924768f97e1" }
$ObsCommit = if ($env:OBS_COMMIT) { $env:OBS_COMMIT } else { "ba2f32bdf791005443988a4955e963663e16b1ed" }
$SimdeCommit = "71fd833d9666141edcd1d3c109a80e228303d8d7"
$OpusVersion = "1.5.2"
$OpusSha256 = "65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1"
Set-Location (Join-Path $PSScriptRoot "..")
New-Item -ItemType Directory -Force .deps\win\lib | Out-Null
Set-Location .deps

if (-not (Test-Path win\obs\bin\64bit\obs.dll)) {
  Invoke-WebRequest "https://github.com/obsproject/obs-studio/releases/download/$ObsVersion/OBS-Studio-$ObsVersion-Windows-x64.zip" -OutFile obs-win.zip
  if ((Get-FileHash obs-win.zip -Algorithm SHA256).Hash.ToLower() -ne $ObsWinSha256) { throw "OBS checksum mismatch" }
  Expand-Archive obs-win.zip -DestinationPath win\obs -Force   # full OBS: also used by the headless test
  Remove-Item obs-win.zip
}

# OBS ships DLLs but no import libraries: generate them from each DLL's export table.
foreach ($dll in "obs", "obs-frontend-api", "w32-pthreads") {
  $exports = dumpbin /exports "win\obs\bin\64bit\$dll.dll" | Select-String '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]{8}\s+(\S+)' | ForEach-Object { $_.Matches[0].Groups[1].Value }
  @("LIBRARY $dll.dll", "EXPORTS") + ($exports | ForEach-Object { "  $_" }) | Set-Content "win\lib\$dll.def"
  lib /nologo /machine:x64 /def:"win\lib\$dll.def" /out:"win\lib\$dll.lib" | Out-Null
  Write-Host "$dll.lib: $($exports.Count) exports"
}

if (-not (Test-Path obs-studio-src)) { git clone --depth 1 --branch $ObsVersion https://github.com/obsproject/obs-studio.git obs-studio-src }
if (-not (Test-Path simde)) { git clone --depth 1 --branch v0.8.2 https://github.com/simd-everywhere/simde.git simde }
foreach ($pin in @(@("obs-studio-src", $ObsCommit), @("simde", $SimdeCommit))) {
  if ((git -c safe.directory="*" -C $pin[0] rev-parse HEAD) -ne $pin[1]) { throw "$($pin[0]) is not at pinned commit $($pin[1])" }
}

if (-not (Test-Path win\opus\lib\opus.lib)) {
  Invoke-WebRequest "https://downloads.xiph.org/releases/opus/opus-$OpusVersion.tar.gz" -OutFile opus.tar.gz
  if ((Get-FileHash opus.tar.gz -Algorithm SHA256).Hash.ToLower() -ne $OpusSha256) { throw "opus checksum mismatch" }
  tar xzf opus.tar.gz
  cmake -S "opus-$OpusVersion" -B win\opus-build -A x64 -DOPUS_BUILD_SHARED_LIBRARY=OFF -DOPUS_BUILD_TESTING=OFF `
    -DOPUS_BUILD_PROGRAMS=OFF -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL -DCMAKE_INSTALL_PREFIX="$PWD\win\opus"
  cmake --build win\opus-build --config Release
  cmake --install win\opus-build --config Release
}
Write-Host "deps ready"
