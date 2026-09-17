#!/usr/bin/env bash
# Sign the embedded frame-ferret engine before Tauri bundles the .app, so every
# nested Mach-O in the bundle is Developer-ID-signed with the hardened runtime
# and the whole app passes notarization. Only for a copy you sign yourself:
# released builds are signed and notarised post-hoc (see ../SIGNING.md). No-op
# when no signing identity is configured.
set -euo pipefail

HERE="$(cd "$(dirname "$0")/.." && pwd)" # launcher/
NODE_BIN="$HERE/src-tauri/bin/frame-ferret"
ENTITLEMENTS="$HERE/src-tauri/entitlements.plist"
IDENTITY="${APPLE_SIGNING_IDENTITY:-}"

if [ -z "$IDENTITY" ]; then
  echo "sign-embedded: APPLE_SIGNING_IDENTITY not set — skipping (unsigned build)."
  exit 0
fi
if [ ! -f "$NODE_BIN" ]; then
  echo "sign-embedded: $NODE_BIN not found — run scripts/prepare.sh first." >&2
  exit 1
fi

echo "sign-embedded: signing $NODE_BIN"
codesign --force --timestamp --options runtime \
  --entitlements "$ENTITLEMENTS" \
  --sign "$IDENTITY" "$NODE_BIN"
codesign --verify --strict --verbose=2 "$NODE_BIN"
echo "sign-embedded: done."
