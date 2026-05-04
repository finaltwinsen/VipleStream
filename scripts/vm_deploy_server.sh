#!/bin/bash
# Deploy a freshly-built VipleStream-Server-*.deb to a Linux test machine.
#
# Usage:  bash scripts/vm_deploy_server.sh user@host [version]
#
#   user@host   Mandatory.  e.g. ubuntu@192.0.2.10
#   version     Optional.   e.g. 1.3.337 (defaults to current version.json)
#
# Run from Windows host (or WSL) with the OpenSSH default key trusted on
# the remote machine's authorized_keys.  No internal hostnames / IPs are
# baked in — pass the target on every invocation.
set -e
REMOTE="${1:?Usage: bash scripts/vm_deploy_server.sh user@host [version]}"
VERSION="${2:-}"

REPO="$(cd "$(dirname "$0")/.." && pwd)"
RELDIR="$REPO/release"

if [ -z "$VERSION" ]; then
    VERSION=$(python3 -c "
import json
v = json.load(open('$REPO/version.json'))
print(f\"{v['major']}.{v['minor']}.{v['patch']}\")
")
fi

DEB="$RELDIR/VipleStream-Server-${VERSION}-linux-x64.deb"
[ -f "$DEB" ] || { echo "FAIL: $DEB not found"; exit 1; }

echo "=== Deploying $DEB → $REMOTE ==="
ls -lh "$DEB"

# Push .deb
scp -o BatchMode=yes "$DEB" "${REMOTE}:/tmp/"

# Stop service, install (--force-overwrite handles config-file collisions),
# restart, verify
ssh -o BatchMode=yes "$REMOTE" "
set -e
echo '--- stopping service ---'
systemctl --user stop app-app.viplestream.server.service || true
echo '--- installing new .deb ---'
sudo dpkg -i /tmp/$(basename $DEB) --force-overwrite || sudo apt-get -f install -y
echo '--- service file ---'
systemctl --user daemon-reload
echo '--- starting service ---'
systemctl --user reset-failed app-app.viplestream.server.service || true
systemctl --user start app-app.viplestream.server.service
sleep 2
systemctl --user is-active app-app.viplestream.server.service
echo '--- apps.json (should only have Desktop) ---'
cat ~/.config/sunshine/apps.json | head -20
echo '--- last 10 log lines ---'
journalctl --user -u app-app.viplestream.server.service -n 10 --no-pager 2>/dev/null || tail -10 ~/.config/sunshine/sunshine.log 2>/dev/null
"
echo "=== DEPLOY OK ==="
