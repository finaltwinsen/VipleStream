#!/bin/bash
echo "=== Active session sunshine.log tail (200 lines) ==="
tail -200 ~/.config/sunshine/sunshine.log 2>/dev/null

echo ""
echo "=== systemd journal (last 5 minutes, errors + frame rate hints) ==="
journalctl --user -u app-app.viplestream.server --since "5 minutes ago" --no-pager 2>&1 \
    | grep -iE "encode|frame|fps|client|connect|session|reqested|deny|drop|error|warn" \
    | tail -50

echo ""
echo "=== full last 80 journal lines ==="
journalctl --user -u app-app.viplestream.server -n 80 --no-pager 2>&1 | tail -80

echo ""
echo "=== display capture state ==="
journalctl --user -u app-app.viplestream.server --since "5 minutes ago" --no-pager 2>&1 \
    | grep -iE "wayland|pipewire|xdg|portal|capture|grab" | tail -20

echo ""
echo "=== input device state ==="
journalctl --user -u app-app.viplestream.server --since "5 minutes ago" --no-pager 2>&1 \
    | grep -iE "uinput|virtual|gamepad|mouse|keyboard|click|cap_sys" | tail -20
