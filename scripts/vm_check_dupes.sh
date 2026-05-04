#!/bin/bash
echo "=== A: Ubuntu app list desktop entries ==="
ls /usr/share/applications/app.viplestream.server*.desktop 2>&1
for f in /usr/share/applications/app.viplestream.server*.desktop; do
    echo "--- $f ---"
    cat "$f" | head -10
done
echo ""
echo "=== B: server apps.json (what client sees) ==="
cat ~/.config/sunshine/apps.json 2>&1
echo ""
echo "=== C: any other VipleStream/sunshine .desktop residuals ==="
find / -name "*.desktop" 2>/dev/null | xargs grep -l -iE "VipleStream|sunshine" 2>/dev/null | head -10
