#!/bin/sh
# VipleStream §SELF-UPDATE (4) — package install helper for the host's self-update.
#
# Installed root-owned at a fixed path (<prefix>/libexec/viplestream-server/self-update) and run
# through pkexec (or passwordless sudo) by the server. The fixed path is what lets the polkit
# action app.viplestream.server.self-update (see packaging/linux/*.policy.in) match this program,
# so the authentication dialog shows a readable message, and root no longer executes a script
# stored in a user's directory.
#
# Keep in sync with kLinuxScript in src/self_update.cpp (the fallback used when this helper is
# not installed, e.g. the first update from a version that did not ship it).
#
# Usage: self-update <package.deb> <sha256 hex | -> <reinstall: 0 | 1>
DEB="$1"; SHA="$2"; REINSTALL="$3"
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH
export DEBIAN_FRONTEND=noninteractive
[ -n "$DEB" ] && [ -f "$DEB" ] || { echo "package not found: $DEB"; exit 4; }
if [ "$SHA" != "-" ]; then
  echo "$SHA  $DEB" | sha256sum -c - >/dev/null 2>&1 || { echo "sha256 mismatch"; exit 4; }
fi
PKG=$(dpkg-deb -f "$DEB" Package 2>/dev/null)
[ "$PKG" = "viplestream-server" ] || { echo "unexpected package name: $PKG"; exit 4; }
if command -v apt-get >/dev/null 2>&1; then
  set -- -y --allow-downgrades --no-remove -o DPkg::Lock::Timeout=120
  [ "$REINSTALL" = "1" ] && set -- "$@" --reinstall
  apt-get install "$@" "$DEB" && exit 0
  echo "apt-get install failed; trying dpkg -i + apt-get -f install"
  dpkg -i "$DEB" && apt-get -f install -y --no-remove && exit 0
  exit 2
fi
dpkg -i "$DEB" || exit 2
exit 0
