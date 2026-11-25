#!/bin/sh
# §CLIENT-DEB — Linux x64 client .deb 的安裝 helper。
#
# 裝在 /usr/libexec/viplestream-client/self-update，擁有者是 root。client 只透過
# pkexec 叫這支固定路徑：polkit 的 exec.path 認的是路徑，不是「使用者目錄裡的某支 shell」。
#
# 為什麼只有一個參數、不收呼叫端給的 sha256：下載端已經對過 release digest。這裡若再信任
# 呼叫端傳來的雜湊，那串字可以跟檔案一起被換掉。改成先複製到 root 目錄，之後只驗那份副本的
# 套件名與版號；驗證之後才被換掉的檔不會被裝上去。
# 為什麼仍要管理者密碼：這支 helper 不驗套件簽章。通過授權等於以 root 執行套件的安裝腳本。
# 政策是 auth_admin_keep（同一個工作階段不必重打），不是 yes，也沒有 sudoers NOPASSWD。
#
# Usage: self-update <package.deb>
# Exit: 0 installed; 2 install failed; 3 interrupted; 4 rejected.
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH
export DEBIAN_FRONTEND=noninteractive
umask 077

[ "$(id -u)" -eq 0 ] || { echo "must run as root"; exit 4; }
[ "$#" -eq 1 ] || { echo "usage: self-update <package.deb>"; exit 4; }
SRC=$1

case "$SRC" in
  /*) ;;
  *) echo "package path must be absolute: $SRC"; exit 4 ;;
esac
case "$SRC" in
  *"
"*) echo "package path must not contain a newline"; exit 4 ;;
esac
if [ -L "$SRC" ] || [ ! -f "$SRC" ]; then
  echo "package not found or not a regular file: $SRC"
  exit 4
fi

# 檔名必須是 VipleStream-Client-X.Y.Z-linux-x64.deb。不用 case 的 * 去切版號：* 會把中間的字一起吃掉。
BASE=$(basename -- "$SRC")
PREFIX=VipleStream-Client-
SUFFIX=-linux-x64.deb
case "$BASE" in
  "$PREFIX"*"$SUFFIX") ;;
  *) echo "unexpected package filename: $BASE"; exit 4 ;;
esac
NAMEVER=${BASE#"$PREFIX"}
NAMEVER=${NAMEVER%"$SUFFIX"}
case "$NAMEVER" in
  *[!0-9.]*) echo "unexpected package filename: $BASE"; exit 4 ;;
esac
dots=$(printf '%s' "$NAMEVER" | tr -cd '.')
[ "$dots" = ".." ] || { echo "unexpected package filename: $BASE"; exit 4; }
case "$NAMEVER" in
  .*|*.|*..*) echo "unexpected package filename: $BASE"; exit 4 ;;
esac

# 複製到 root 專用目錄（之後只用副本）。呼叫端在我們讀完之後換檔，影響不到這份。
CACHE_BASE=/var/cache/viplestream-client
CACHE=$CACHE_BASE/update
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
# apt-get 只把結尾是 .deb、而且路徑裡有 / 的參數當成本機檔。副本固定叫 package.deb。
DEB=$WORK/package.deb
# cp 預設會跟隨符號連結。上面的檢查與這裡的複製之間，來源可能被換成連結。-P 原樣複製，下一行擋下。
cp -P -- "$SRC" "$DEB" || { echo "cannot copy package"; exit 4; }
if [ -L "$DEB" ] || [ ! -f "$DEB" ]; then
  echo "copied package is not a regular file"
  exit 4
fi

PKG=$(dpkg-deb -f "$DEB" Package 2>/dev/null) || { echo "dpkg-deb failed"; exit 4; }
[ "$PKG" = "viplestream-client" ] || { echo "unexpected package name: $PKG"; exit 4; }
# 檔名寫 x64。套件內容若是別的架構，不能單憑檔名放行。
ARCH=$(dpkg-deb -f "$DEB" Architecture 2>/dev/null) || { echo "dpkg-deb failed"; exit 4; }
[ "$ARCH" = "amd64" ] || { echo "unexpected architecture: $ARCH"; exit 4; }
NEWV=$(dpkg-deb -f "$DEB" Version 2>/dev/null) || { echo "dpkg-deb failed"; exit 4; }
# 檔名上的版號必須等於控制檔。只驗檔名的話，內容可以換成另一個版號。
[ "$NEWV" = "$NAMEVER" ] || { echo "filename version $NAMEVER != package version $NEWV"; exit 4; }
dpkg --validate-version "$NEWV" || { echo "invalid package version: $NEWV"; exit 4; }

# shellcheck disable=SC2016  # ${...} 是 dpkg-query 的欄位格式，不是 shell 展開
INST=$(dpkg-query -W -f='${db:Status-Abbrev}|${Version}' viplestream-client 2>/dev/null) || INST=""
OLDV=${INST#*|}
case "$INST" in
  ''|?n*|?c*) OLDV="" ;;
esac
if [ -n "$OLDV" ]; then
  # compare-versions 遇到不合法的版號回 2。不是「比較新」就拒絕（1 與 2 都算）。
  if ! dpkg --compare-versions "$NEWV" gt "$OLDV"; then
    echo "refusing: package $NEWV is not newer than installed $OLDV"
    exit 4
  fi
fi
echo "installing viplestream-client $NEWV (installed: ${OLDV:-none})"

if command -v apt-get >/dev/null 2>&1; then
  # --no-remove：求解相依時不把已經裝好的套件拆掉。鎖逾時避免另一個 apt 佔著時一直空等。
  apt-get install -y --no-remove -o DPkg::Lock::Timeout=120 "$DEB" && exit 0
  echo "apt-get install failed; trying dpkg -i"
  dpkg -i "$DEB" && apt-get -f install -y --no-remove && exit 0
  exit 2
fi
dpkg -i "$DEB" || exit 2
exit 0
