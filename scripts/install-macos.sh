#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Installs the built plugin for the current user (OBS must be restarted).
set -euo pipefail
cd "$(dirname "$0")/.."
dest="$HOME/Library/Application Support/obs-studio/plugins"
mkdir -p "$dest"
rm -rf "$dest/faders.plugin"
cp -R build/faders.plugin "$dest/"
echo "installed to $dest/faders.plugin. Restart OBS, then open Tools → Faders."
