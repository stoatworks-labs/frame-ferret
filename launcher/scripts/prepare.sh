#!/usr/bin/env bash
# Stage the Frame Ferret binary into the desktop bundle.
#
# Frame Ferret is a single native executable, so this is far simpler than the
# Node-based launchers in the fleet: build it and copy it in. No runtime to
# embed, no node_modules, no platform-specific prebuilds.
#
# Produces src-tauri/bin/frame-ferret[.exe], which is git-ignored — it ships
# inside the bundle, not in the repo. Run before `npm run tauri build`.
#
# Every transport is loaded at run time rather than linked, so the bundled
# binary starts on a machine with none of them installed and reports each as
# unavailable. That is why there is nothing else to stage.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
BIN_DIR="$HERE/../src-tauri/bin"

echo "building frame-ferret from $REPO"
# On macOS the launcher ships one universal .app, so the engine inside it has
# to be universal too. Must be set at configure time, or "universal" ships
# arm64 only — the same flag the release workflow's build job uses.
EXTRA=""
if [[ "$(uname -s)" == Darwin ]]; then
  EXTRA='-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64'
fi
cmake -S "$REPO" -B "$REPO/build-launcher" -DCMAKE_BUILD_TYPE=Release $EXTRA >/dev/null
cmake --build "$REPO/build-launcher" --config Release -j 4 >/dev/null

mkdir -p "$BIN_DIR"
if [[ -f "$REPO/build-launcher/Release/frame-ferret.exe" ]]; then
  cp "$REPO/build-launcher/Release/frame-ferret.exe" "$BIN_DIR/"
  echo "staged $BIN_DIR/frame-ferret.exe"
else
  cp "$REPO/build-launcher/frame-ferret" "$BIN_DIR/"
  chmod +x "$BIN_DIR/frame-ferret"
  if [[ "$(uname -s)" == Darwin ]]; then
    __archs="$( lipo -archs "$BIN_DIR/frame-ferret" )"
    case "$__archs" in
      *arm64*x86_64*|*x86_64*arm64*) ;;
      *) echo "staged frame-ferret is not universal: $__archs" >&2; exit 1 ;;
    esac
    echo "staged $BIN_DIR/frame-ferret ($__archs)"
  else
    echo "staged $BIN_DIR/frame-ferret"
  fi
fi
