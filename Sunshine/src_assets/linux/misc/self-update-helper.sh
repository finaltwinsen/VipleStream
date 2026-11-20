#!/bin/sh
# VipleStream §SELF-UPDATE (4) — package install helper for the host's self-update.
#
# Installed root-owned at a fixed path (<prefix>/libexec/viplestream-server/self-update) and run
# through pkexec (or passwordless sudo) by the server. The fixed path is what lets the polkit
# action app.viplestream.server.self-update (see packaging/linux/*.policy.in) match this program,
# so the authentication dialog shows a readable message, and root no longer executes a script
# stored in a user's directory.
#
# 這份檔案同時是 src/self_update.cpp 的內嵌備援腳本 kLinuxScript 的唯一來源：建置時由
# cmake/compile_definitions/common.cmake 讀進產生的標頭（self_update_helper_script.h），
# 所以只改這裡就好，不要在 self_update.cpp 另外維護一份。
# 這支腳本會被 sh 直接讀（.deb 安裝的 helper），也會被包進 C++ raw string（分隔字 VIPLESH），
# 因此內容裡不可出現「右括號＋VIPLESH＋雙引號」這個序列。
#
# ⚠ 安全前提（2026-11-20 審查 F1）：這支 helper **不驗套件來源**。sha256 由呼叫端給，
#   Package／Version 檢查擋不住自製的 viplestream-server 套件，而套件的 maintainer script 以 root 執行——
#   也就是「通過授權＝以 root 執行任意程式碼」。安全性完全建立在每次都要管理者密碼上：
#   不可把 polkit 政策的 auth_admin 放寬成 yes 或 auth_admin_keep，不可替這支 helper 加 sudoers NOPASSWD
#   或 polkit rules 放行。要做無人值守更新，先做 root 側的套件簽章驗證。
#
# 這裡做的是縱深防禦（審查 F2／F5／F6）：
#   - 參數格式：套件路徑必須是絕對路徑、一般檔案（不是 symlink）；sha256 必須剛好 64 個小寫十六進位字元
#     （不再接受 "-" 略過驗證）；reinstall 只接受 0 或 1。
#   - 先把套件複製到 root 擁有、其他人不可寫的目錄，之後驗證與安裝只用那份副本，
#     呼叫端在驗證之後換檔也影響不到安裝（TOCTOU）。
#   - 拒絕降版：套件版本低於已安裝版本就不裝；同版只在 reinstall=1 時加 --reinstall。
#
# Usage: self-update <package.deb> <sha256 hex> <reinstall: 0 | 1>
# Exit: 0 installed; 2 install failed; 3 interrupted; 4 rejected (bad arguments / verification failed).
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH
export DEBIAN_FRONTEND=noninteractive
umask 077

[ "$#" -eq 3 ] || { echo "usage: self-update <package.deb> <sha256> <0|1>"; exit 4; }
SRC="$1"; SHA="$2"; REINSTALL="$3"

# --- 參數格式 ---
case "$SRC" in
  /*) ;;
  *) echo "package path must be absolute: $SRC"; exit 4 ;;
esac
if [ -L "$SRC" ] || [ ! -f "$SRC" ]; then
  echo "package not found or not a regular file: $SRC"
  exit 4
fi
case "$SHA" in
  '' | *[!0-9a-f]*) echo "invalid sha256 (need 64 lowercase hex digits)"; exit 4 ;;
esac
[ "${#SHA}" -eq 64 ] || { echo "invalid sha256 (need 64 lowercase hex digits)"; exit 4; }
case "$REINSTALL" in
  0 | 1) ;;
  *) echo "invalid reinstall flag: $REINSTALL"; exit 4 ;;
esac

# --- 複製到 root 專用目錄（之後只用副本）---
CACHE_BASE=/var/cache/viplestream-server
CACHE="$CACHE_BASE/update"
if [ -L "$CACHE_BASE" ] || [ -L "$CACHE" ]; then
  echo "refusing symlinked cache directory"
  exit 4
fi
install -d -m 0755 -o root -g root "$CACHE_BASE" || { echo "cannot create $CACHE_BASE"; exit 4; }
install -d -m 0700 -o root -g root "$CACHE" || { echo "cannot create $CACHE"; exit 4; }
[ "$(stat -c '%u %a' "$CACHE" 2>/dev/null)" = "0 700" ] || { echo "unsafe cache directory: $CACHE"; exit 4; }
WORK=$(mktemp -d "$CACHE/pkg.XXXXXXXXXX") || { echo "mktemp failed"; exit 4; }
trap 'rm -rf "$WORK"' EXIT
trap 'exit 3' HUP INT TERM
# apt-get 只把結尾是 .deb、含 / 的參數當成本機檔案，所以副本固定叫 package.deb
DEB="$WORK/package.deb"
cp -- "$SRC" "$DEB" || { echo "cannot copy package"; exit 4; }
if [ -L "$DEB" ] || [ ! -f "$DEB" ]; then
  echo "copied package is not a regular file"
  exit 4
fi

# --- 驗證副本 ---
GOT=$(sha256sum "$DEB" 2>/dev/null) || { echo "sha256sum failed"; exit 4; }
GOT="${GOT%% *}"
[ "$GOT" = "$SHA" ] || { echo "sha256 mismatch"; exit 4; }
PKG=$(dpkg-deb -f "$DEB" Package 2>/dev/null)
[ "$PKG" = "viplestream-server" ] || { echo "unexpected package name: $PKG"; exit 4; }
NEWV=$(dpkg-deb -f "$DEB" Version 2>/dev/null)
case "$NEWV" in
  [0-9]*) ;;
  *) echo "invalid package version: $NEWV"; exit 4 ;;
esac
case "$NEWV" in
  *[!0-9A-Za-z.+~:-]*) echo "invalid package version: $NEWV"; exit 4 ;;
esac

# --- 拒絕降版 ---
# shellcheck disable=SC2016  # ${...} 是 dpkg-query 的欄位格式，不是 shell 展開
INST=$(dpkg-query -W -f='${db:Status-Abbrev}|${Version}' viplestream-server 2>/dev/null) || INST=""
OLDV="${INST#*|}"
case "$INST" in
  '' | ?n* | ?c*) OLDV="" ;;  # 沒裝過、或只剩設定檔（rc）：不比
esac
SAME=0
if [ -n "$OLDV" ]; then
  if dpkg --compare-versions "$NEWV" lt "$OLDV"; then
    echo "refusing downgrade: package $NEWV is older than installed $OLDV"
    exit 4
  fi
  dpkg --compare-versions "$NEWV" eq "$OLDV" && SAME=1
fi
echo "installing viplestream-server $NEWV (installed: ${OLDV:-none})"

# --- 安裝（只用副本）---
if command -v apt-get >/dev/null 2>&1; then
  set -- -y --no-remove -o DPkg::Lock::Timeout=120
  if [ "$SAME" = "1" ] && [ "$REINSTALL" = "1" ]; then
    set -- "$@" --reinstall
  fi
  apt-get install "$@" "$DEB" && exit 0
  echo "apt-get install failed; trying dpkg -i + apt-get -f install"
  dpkg -i "$DEB" && apt-get -f install -y --no-remove && exit 0
  exit 2
fi
dpkg -i "$DEB" || exit 2
exit 0
