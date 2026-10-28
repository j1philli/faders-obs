#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Packages build/faders.plugin as faders-obs-<version>-macos.pkg (current-user install into
# ~/Library/Application Support/obs-studio/plugins), and the same bundle as
# faders-obs-<version>-macos.zip for extracting by hand. Run scripts/build-macos.sh first.
#
# Unsigned by default: macOS then asks the user to allow it in System Settings → Privacy &
# Security. Release signing, when these are set (needs a paid Apple Developer account):
#   MACOS_APP_IDENTITY        "Developer ID Application: …"  signs the plugin (hardened runtime)
#   MACOS_INSTALLER_IDENTITY  "Developer ID Installer: …"    signs the .pkg
#   MACOS_NOTARY_PROFILE      notarytool keychain profile    notarizes and staples the .pkg (the
#                                                            ticket also covers the zip's bundle)
# or, instead of a profile, an App Store Connect API key (the same kind of key used for iOS uploads):
#   MACOS_NOTARY_KEY_PATH, MACOS_NOTARY_KEY_ID, MACOS_NOTARY_ISSUER
# On CI the identities can live in their own keychain, unlocked for the build:
#   MACOS_KEYCHAIN_PATH, MACOS_KEYCHAIN_PASSWORD_FILE
set -euo pipefail
cd "$(dirname "$0")/.."
VERSION="${FADERS_VERSION:-$(sed -n 's/^project(faders VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)}"
OUT="faders-obs-$VERSION-macos.pkg"
ZIP="faders-obs-$VERSION-macos.zip"
WORK=package/macos
keychain=()
rm -rf "$WORK" && mkdir -p "$WORK/root" "$WORK/resources"

if [ -n "${MACOS_KEYCHAIN_PATH:-}" ]; then
  security unlock-keychain -p "$(cat "$MACOS_KEYCHAIN_PASSWORD_FILE")" "$MACOS_KEYCHAIN_PATH"
  keychain=(--keychain "$MACOS_KEYCHAIN_PATH")
fi

# No extended attributes or resource forks: pkgbuild would ship them as ._ files.
ditto --norsrc --noextattr --noqtn --noacl build/faders.plugin "$WORK/root/faders.plugin"
if [ -n "${MACOS_APP_IDENTITY:-}" ]; then
  codesign --force --timestamp --options runtime ${keychain[@]+"${keychain[@]}"} --sign "$MACOS_APP_IDENTITY" "$WORK/root/faders.plugin"
fi

# Keep the bundle where we put it (no "relocation" to a copy found elsewhere on disk).
pkgbuild --analyze --root "$WORK/root" "$WORK/component.plist" >/dev/null
plutil -replace 0.BundleIsRelocatable -bool NO "$WORK/component.plist"
COPYFILE_DISABLE=1 pkgbuild --root "$WORK/root" --component-plist "$WORK/component.plist" --identifier tv.faders.obs-plugin \
  --version "$VERSION" --install-location "/Library/Application Support/obs-studio/plugins" \
  "$WORK/faders-component.pkg" >/dev/null

# Retina-ready background: a multi-resolution TIFF from the 1x/2x PNGs.
tiffutil -cathidpicheck installer/macos/bg.png installer/macos/bg@2x.png -out "$WORK/resources/bg.tiff" 2>/dev/null
cp installer/macos/welcome.html installer/macos/conclusion.html "$WORK/resources/"
sed "s/@VERSION@/$VERSION/" installer/macos/distribution.xml > "$WORK/distribution.xml"

sign=()
[ -n "${MACOS_INSTALLER_IDENTITY:-}" ] && sign=(--sign "$MACOS_INSTALLER_IDENTITY" --timestamp ${keychain[@]+"${keychain[@]}"})
productbuild --distribution "$WORK/distribution.xml" --resources "$WORK/resources" --package-path "$WORK" \
  ${sign[@]+"${sign[@]}"} "$OUT" >/dev/null

notary=()
[ -n "${MACOS_NOTARY_PROFILE:-}" ] && notary=(--keychain-profile "$MACOS_NOTARY_PROFILE")
[ -n "${MACOS_NOTARY_KEY_PATH:-}" ] && notary=(--key "$MACOS_NOTARY_KEY_PATH" --key-id "$MACOS_NOTARY_KEY_ID" --issuer "$MACOS_NOTARY_ISSUER")
if [ ${#notary[@]} -gt 0 ]; then
  xcrun notarytool submit "$OUT" "${notary[@]}" --wait
  xcrun stapler staple "$OUT"
fi
# The signed bundle that went into the .pkg; notarizing the .pkg notarized it too.
rm -f "$ZIP"
ditto -c -k --norsrc --noextattr --noqtn --keepParent "$WORK/root/faders.plugin" "$ZIP"
echo "built $OUT and $ZIP ($([ ${#sign[@]} -gt 0 ] && echo signed || echo unsigned))"
