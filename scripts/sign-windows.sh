#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Authenticode-signs Windows files with Azure Trusted Signing, from Linux or macOS, using Jsign:
#   scripts/sign-windows.sh file.dll [file.exe ...]
# Needs java plus these (a Microsoft Entra app with the "Trusted Signing Certificate Profile
# Signer" role on the certificate profile):
#   AZURE_TENANT_ID, AZURE_CLIENT_ID, AZURE_CLIENT_SECRET
#   AZURE_SIGNING_ENDPOINT   region host, e.g. eus.codesigning.azure.net
#   AZURE_SIGNING_ACCOUNT    Trusted Signing account name
#   AZURE_SIGNING_PROFILE    certificate profile name
set -euo pipefail
JSIGN_VERSION="7.5"
JSIGN_SHA256="602a51c3545a6dc4fb99bd2ea7152b26d1345916d0c93ddfbd5936cb735af91c"
here="$(cd "$(dirname "$0")/.." && pwd)"
jar="$here/.deps/jsign-$JSIGN_VERSION.jar"
if [ ! -f "$jar" ]; then
  mkdir -p "$here/.deps"
  curl -fsSL -o "$jar.tmp" "https://github.com/ebourg/jsign/releases/download/$JSIGN_VERSION/jsign-$JSIGN_VERSION.jar"
  if command -v sha256sum >/dev/null; then echo "$JSIGN_SHA256  $jar.tmp" | sha256sum -c - >/dev/null
  else echo "$JSIGN_SHA256  $jar.tmp" | shasum -a 256 -c - >/dev/null; fi
  mv "$jar.tmp" "$jar"
fi

# Client-credentials token for the signing API, read by Jsign from the environment (never argv).
AZURE_SIGNING_TOKEN="$(curl -fsS "https://login.microsoftonline.com/$AZURE_TENANT_ID/oauth2/v2.0/token" \
  --data-urlencode grant_type=client_credentials \
  --data-urlencode "client_id=$AZURE_CLIENT_ID" \
  --data-urlencode "client_secret@-" \
  --data-urlencode "scope=https://codesigning.azure.net/.default" < <(printf %s "$AZURE_CLIENT_SECRET") \
  | python3 -c 'import json,sys; print(json.load(sys.stdin)["access_token"])')"
export AZURE_SIGNING_TOKEN

java -jar "$jar" --storetype TRUSTEDSIGNING --keystore "$AZURE_SIGNING_ENDPOINT" \
  --storepass env:AZURE_SIGNING_TOKEN --alias "$AZURE_SIGNING_ACCOUNT/$AZURE_SIGNING_PROFILE" \
  --tsaurl http://timestamp.acs.microsoft.com --tsmode RFC3161 \
  --name "Faders for OBS" --url https://faders.tv "$@"
