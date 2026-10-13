# SPDX-License-Identifier: GPL-2.0-or-later
# TeamCity (Windows agent): build faders.dll with MSVC, run the headless end-to-end test, package
# in OBS's user-plugin layout (%ProgramData%\obs-studio\plugins\faders\{bin\64bit,data}).
# Agent needs: Visual Studio 2022 or Build Tools with "Desktop development with C++" (includes
# CMake), git, node 22.
$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
Set-Location $root
$port = if ($env:FADERS_TEST_PORT) { $env:FADERS_TEST_PORT } else { "18787" }

# MSVC environment, wherever Visual Studio is installed.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio 2022 / Build Tools with the C++ workload is required on this agent" }
Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null

function Check($what) { if ($LASTEXITCODE -ne 0) { throw "$what failed ($LASTEXITCODE)" } }
npm ci --no-audit --no-fund; Check "npm ci"
& .\scripts\fetch-deps-windows.ps1
cmake -S . -B build -A x64; Check "cmake configure"
cmake --build build --config Release; Check "cmake build"

$hub = $null; $hostProc = $null
try {
  $hub = Start-Process node -ArgumentList "test/mock-hub.mjs", "--port", $port -PassThru -NoNewWindow -RedirectStandardOutput "$root\hub.log" -RedirectStandardError "$root\hub.err.log"
  for ($i = 0; $i -lt 30; $i++) { try { if ((Invoke-WebRequest "http://127.0.0.1:$port/health" -UseBasicParsing).Content -eq "ok") { break } } catch {}; Start-Sleep 1 }

  # libobs on Windows finds its data at ..\..\data\libobs and loads libobs-d3d11 from the
  # working directory, so the host runs from OBS's own bin\64bit.
  $bin = Join-Path $root ".deps\win\obs\bin\64bit"
  Copy-Item build\Release\faders-headless.exe $bin -Force
  $m = Join-Path $root "test\media"
  $media = @("$m\voice.wav", "$m\game.wav", "$m\music.wav")
  Push-Location $bin
  & .\faders-headless.exe "ws://127.0.0.1:$port/ingest/ci_channel" wrong-key @media 5 *> "$root\host-badkey.log"
  if (-not (Select-String -Path "$root\host-badkey.log" -Pattern "HTTP 401" -Quiet)) { throw "wrong key was not rejected with HTTP 401" }
  $hostProc = Start-Process .\faders-headless.exe -ArgumentList (@("ws://127.0.0.1:$port/ingest/ci_channel", "dev-token") + $media + @("40")) -PassThru -NoNewWindow -RedirectStandardOutput "$root\host.log" -RedirectStandardError "$root\host.err.log"
  Pop-Location
  Start-Sleep 6
  node test/plugin-listener.mjs --server "ws://127.0.0.1:$port" --channel ci_channel --seconds 20; Check "listener check"
} finally {
  foreach ($p in @($hostProc, $hub)) { if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force } }
  Get-Content "$root\host.log", "$root\host.err.log" -ErrorAction SilentlyContinue | Select-Object -Last 40
}

$stage = Join-Path $root "package\faders"
Remove-Item (Join-Path $root "package") -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$stage\bin\64bit", "$stage\data" | Out-Null
Copy-Item build\Release\faders.dll "$stage\bin\64bit\"
Copy-Item data\* "$stage\data" -Recurse
Compress-Archive -Path $stage -DestinationPath (Join-Path $root "faders-obs-windows.zip") -Force

# Installer from the DLL that just passed the runtime test, when NSIS is on the agent
# (https://nsis.sourceforge.io; the Linux cross-build also produces one from the MinGW DLL).
$nsis = @((Get-Command makensis -ErrorAction SilentlyContinue).Source, "${env:ProgramFiles(x86)}\NSIS\makensis.exe") | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
if ($nsis) {
  $version = (Select-String -Path CMakeLists.txt -Pattern 'project\(faders VERSION ([0-9.]+)').Matches[0].Groups[1].Value
  & $nsis -V2 "-DVERSION=$version" "-DDLL=$root\build\Release\faders.dll" "-DOUTFILE=$root\faders-obs-$version-windows.exe" installer\windows\faders.nsi; Check "makensis"
} else {
  Write-Host "##teamcity[message text='NSIS not installed on this agent: skipped the .exe installer' status='WARNING']"
}
