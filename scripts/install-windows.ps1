# SPDX-License-Identifier: GPL-2.0-or-later
# Installs the built plugin where OBS 32 looks for user plugins (frontend/widgets/OBSBasic.cpp):
#   %ProgramData%\obs-studio\plugins\faders\bin\64bit\faders.dll
#   %ProgramData%\obs-studio\plugins\faders\data\locale\en-US.ini
# Run as Administrator (ProgramData is machine-wide), then restart OBS → Tools → Faders.
param([string]$Build = (Join-Path $PSScriptRoot "..\build"))
$ErrorActionPreference = "Stop"
$dll = @("$Build\Release\faders.dll", "$Build\faders.dll") | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $dll) { throw "faders.dll not found under $Build — build first" }
$root = Join-Path $env:ProgramData "obs-studio\plugins\faders"
New-Item -ItemType Directory -Force "$root\bin\64bit", "$root\data" | Out-Null
Copy-Item $dll "$root\bin\64bit\faders.dll" -Force
Copy-Item (Join-Path $PSScriptRoot "..\data\*") "$root\data" -Recurse -Force
Write-Host "Installed to $root. Restart OBS, then open Tools → Faders."
