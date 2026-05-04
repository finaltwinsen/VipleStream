#!/bin/bash
echo "=== Web UI HTTPS probe ==="
curl -sk -o /dev/null -w "47990 HTTPS code=%{http_code} time=%{time_total}s size=%{size_download}\n" \
    --max-time 5 https://localhost:47990/ 2>&1

echo ""
echo "=== /serverinfo HTTP probe ==="
curl -s --max-time 5 http://localhost:47989/serverinfo 2>&1 | head -30

echo ""
echo "=== Encoder cascade ==="
journalctl --user -u app-app.viplestream.server --since "2 minutes ago" --no-pager 2>&1 \
    | grep -iE "encoder|nvenc|amf|vaapi|vulkan|libx264|fail" | head -25
