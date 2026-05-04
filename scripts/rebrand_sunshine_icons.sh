#!/bin/bash
# VipleStream §K.1: regenerate Sunshine Web UI logo PNGs from
# assets/icon/viplestream_icon.svg so the served Web UI top-bar +
# browser favicon match the rest of the rebrand.  State-specific
# tray icons (locked / pausing / playing) NOT replaced — they need
# state-overlay design work, deferred to §K.4.
set -e
SRC="$REPO/assets/icon/viplestream_icon.svg"
OUT="$REPO/Sunshine/src_assets/common/assets/web/public/images"
[ -f "$SRC" ] || { echo "FAIL: $SRC missing"; exit 1; }

rsvg-convert -w 16 -h 16 "$SRC" -o "$OUT/logo-sunshine-16.png"
rsvg-convert -w 45 -h 45 "$SRC" -o "$OUT/logo-sunshine-45.png"
echo "Replaced:"
ls -lh "$OUT/logo-sunshine-16.png" "$OUT/logo-sunshine-45.png"
