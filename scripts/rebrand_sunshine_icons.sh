#!/bin/bash
# VipleStream §K.1: regenerate ALL upstream-Sunshine / upstream-Moonlight
# icons from assets/icon/viplestream_icon.svg so:
#   1. Sunshine Web UI top-bar logo + favicon match the rebrand
#   2. Sunshine sunshine.exe icon (Windows taskbar / Start menu / Task Manager)
#   3. Moonlight VipleStream.exe icon (Windows taskbar / Start menu / Task Manager)
#   4. Moonlight Linux install icon (used by hicolor → AppImage)
#
# State-specific tray icons (sunshine-locked / pausing / playing .ico/.png)
# NOT replaced — they need state-overlay design work, deferred to §K.4.
#
# Requires: rsvg-convert (librsvg2-bin) + convert (imagemagick).
# REPO defaults to the parent of this script.
set -e
REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"
SRC="$REPO/assets/icon/viplestream_icon.svg"
[ -f "$SRC" ] || { echo "FAIL: $SRC missing"; exit 1; }

WEB_OUT="$REPO/Sunshine/src_assets/common/assets/web/public/images"
SUNSHINE_TOP="$REPO/Sunshine"
MOONLIGHT_APP="$REPO/moonlight-qt/app"
MOONLIGHT_RES="$REPO/moonlight-qt/app/res"

mkico() {
    # Build a multi-res ICO at $1 from SVG ($SRC).  16/32/48 sizes cover
    # taskbar + tab + alt-tab + Task Manager.  256x256 also added for
    # high-DPI Win11 contexts (Start menu tile, Settings → Apps).
    local out="$1"
    local tmp
    tmp=$(mktemp -d)
    rsvg-convert -w 16  -h 16  "$SRC" -o "$tmp/16.png"
    rsvg-convert -w 32  -h 32  "$SRC" -o "$tmp/32.png"
    rsvg-convert -w 48  -h 48  "$SRC" -o "$tmp/48.png"
    rsvg-convert -w 256 -h 256 "$SRC" -o "$tmp/256.png"
    convert "$tmp/16.png" "$tmp/32.png" "$tmp/48.png" "$tmp/256.png" "$out"
    rm -rf "$tmp"
}

echo "=== 1. Sunshine Web UI logos (top-bar) ==="
rsvg-convert -w 16 -h 16 "$SRC" -o "$WEB_OUT/logo-sunshine-16.png"
rsvg-convert -w 45 -h 45 "$SRC" -o "$WEB_OUT/logo-sunshine-45.png"

echo "=== 2. Sunshine Web UI favicon (browser tab) ==="
mkico "$WEB_OUT/sunshine.ico"

echo "=== 3. Sunshine .exe icon (Windows Task Manager / Start menu) ==="
mkico "$SUNSHINE_TOP/sunshine.ico"

echo "=== 4. Moonlight VipleStream.exe icon (Windows Task Manager / Start menu) ==="
mkico "$MOONLIGHT_APP/moonlight.ico"

echo "=== 5. Moonlight install svg (Linux hicolor + AppImage .DirIcon) ==="
cp "$SRC" "$MOONLIGHT_RES/moonlight.svg"

echo ""
echo "Replaced:"
ls -lh \
    "$WEB_OUT/logo-sunshine-16.png" "$WEB_OUT/logo-sunshine-45.png" \
    "$WEB_OUT/sunshine.ico" \
    "$SUNSHINE_TOP/sunshine.ico" \
    "$MOONLIGHT_APP/moonlight.ico" \
    "$MOONLIGHT_RES/moonlight.svg"
