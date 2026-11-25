#!/usr/bin/env bash
# §CLIENT-DEB：把 Linux x64 client 的 AppImage 包成 .deb 安裝包。
#
# 為什麼不直接給 AppImage：Ubuntu 24.04 起預設沒有 libfuse2，AppImage 雙擊常常跑不起來；一般使用者也
# 習慣「裝起來、選單裡有圖示」。這裡把 AppImage 解開（--appimage-extract 不需要 FUSE），裝到
# /opt/viplestream，加上 /usr/bin/viplestream、應用程式選單捷徑與圖示。
#
# 更新：從 /opt/viplestream 執行時，client 下載 VipleStream-Client-X.Y.Z-linux-x64.deb，
# 驗過 sha256 後用 pkexec 叫 /usr/libexec/viplestream-client/self-update 安裝
# （同目錄的 helper 與 polkit 政策）。AppImage 照舊發佈給想用免安裝版的人。
#
# 用法：build-client-deb.sh <AppImage> <版號 X.Y.Z> <輸出目錄>
#   產出 <輸出目錄>/VipleStream-Client-X.Y.Z-linux-x64.deb
# 需要：dpkg-deb（任何 Debian／Ubuntu，或 WSL）。不需要 root（用 --root-owner-group）。
set -euo pipefail

APPIMAGE=${1:?usage: build-client-deb.sh <AppImage> <version> <out dir>}
VERSION=${2:?usage: build-client-deb.sh <AppImage> <version> <out dir>}
OUTDIR=${3:?usage: build-client-deb.sh <AppImage> <version> <out dir>}

if ! [[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "[ERROR] version must be X.Y.Z, got '$VERSION'" >&2
    exit 1
fi
command -v dpkg-deb >/dev/null || { echo "[ERROR] dpkg-deb not found" >&2; exit 1; }

# §CLIENT-DEB：helper 與政策跟這支腳本放在一起，路徑寫進套件。pkexec 只認裝好的那支，不能改放使用者目錄。
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
HELPER_SRC="$SCRIPT_DIR/client-self-update.sh"
POLICY_SRC="$SCRIPT_DIR/app.viplestream.client.self-update.policy"
if [ ! -f "$HELPER_SRC" ] || [ ! -f "$POLICY_SRC" ]; then
    echo "[ERROR] missing self-update helper or policy next to this script" >&2
    exit 1
fi

APPIMAGE=$(readlink -f "$APPIMAGE")
mkdir -p "$OUTDIR"
OUTDIR=$(readlink -f "$OUTDIR")
OUT="$OUTDIR/VipleStream-Client-$VERSION-linux-x64.deb"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# 解開 AppImage：在暫存目錄裡執行，產出 squashfs-root/
cp "$APPIMAGE" "$WORK/app.AppImage"
chmod +x "$WORK/app.AppImage"
( cd "$WORK" && ./app.AppImage --appimage-extract >/dev/null )
SRC="$WORK/squashfs-root"
[ -x "$SRC/AppRun" ] || { echo "[ERROR] AppRun not found after extraction" >&2; exit 1; }

PKG="$WORK/pkg"
mkdir -p "$PKG/DEBIAN" "$PKG/opt/viplestream" "$PKG/usr/bin" \
         "$PKG/usr/libexec/viplestream-client" \
         "$PKG/usr/share/polkit-1/actions" \
         "$PKG/usr/share/applications" "$PKG/usr/share/icons/hicolor/256x256/apps"
cp "$HELPER_SRC" "$PKG/usr/libexec/viplestream-client/self-update"
chmod 0755 "$PKG/usr/libexec/viplestream-client/self-update"
cp "$POLICY_SRC" "$PKG/usr/share/polkit-1/actions/app.viplestream.client.self-update.policy"
chmod 0644 "$PKG/usr/share/polkit-1/actions/app.viplestream.client.self-update.policy"
cp -a "$SRC/." "$PKG/opt/viplestream/"
# AppImage 的權限是唯讀 squashfs 的；裝到系統目錄要能被 root 以外的人讀與執行
chmod -R u+rwX,go+rX,go-w "$PKG/opt/viplestream"

cat > "$PKG/usr/bin/viplestream" <<'EOF'
#!/bin/sh
# AppRun 以自己的實際路徑找 APPDIR，所以從這裡轉呼叫即可
exec /opt/viplestream/AppRun "$@"
EOF
chmod 0755 "$PKG/usr/bin/viplestream"

# 圖示：AppImage 根目錄的 .DirIcon（或任何 png）
ICON=""
for c in "$SRC/.DirIcon" "$SRC"/*.png "$SRC"/usr/share/icons/hicolor/256x256/apps/*.png; do
    if [ -f "$c" ]; then ICON="$c"; break; fi
done
if [ -n "$ICON" ]; then
    cp -L "$ICON" "$PKG/usr/share/icons/hicolor/256x256/apps/viplestream.png"
fi

cat > "$PKG/usr/share/applications/viplestream.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=VipleStream
Comment=Stream games and VR from your PC
Comment[zh_TW]=從電腦串流遊戲與 VR
Exec=viplestream
Icon=viplestream
Terminal=false
Categories=Game;Network;
StartupWMClass=VipleStream
EOF

INSTALLED_KB=$(du -sk "$PKG/opt" | cut -f1)

# 最低 glibc：從包內所有檔案引用的 GLIBC_x.y 取最大值。AppImage 不打包 glibc，在比建置機舊的系統上
# 會在執行時才報 `GLIBC_2.xx not found`；寫進 Depends 讓 apt 在安裝時就拒絕。
GLIBC_MIN=$(grep -raoh 'GLIBC_2\.[0-9]\+' "$PKG/opt/viplestream" 2>/dev/null \
    | sed 's/GLIBC_//' | sort -t. -k1,1n -k2,2n -u | tail -1)
GLIBC_MIN=${GLIBC_MIN:-2.35}
echo "[INFO] minimum glibc $GLIBC_MIN" >&2
# 只列 AppImage 依慣例不打包、一定要系統提供的函式庫；其餘都在 /opt/viplestream 裡
cat > "$PKG/DEBIAN/control" <<EOF
Package: viplestream-client
Version: $VERSION
Section: games
Priority: optional
Architecture: amd64
Installed-Size: $INSTALLED_KB
Depends: libc6 (>= $GLIBC_MIN), libgl1, libegl1
Recommends: libvulkan1, libva2, libpulse0
Maintainer: VipleStream <noreply@users.noreply.github.com>
Homepage: https://github.com/finaltwinsen/VipleStream
Description: VipleStream game and VR streaming client
 Streams games and SteamVR from a PC running VipleStream Server
 (Sunshine compatible) to this computer.
EOF

cat > "$PKG/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if command -v update-desktop-database >/dev/null 2>&1; then update-desktop-database -q /usr/share/applications || true; fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then gtk-update-icon-cache -q -t /usr/share/icons/hicolor || true; fi
exit 0
EOF
cp "$PKG/DEBIAN/postinst" "$PKG/DEBIAN/postrm"
chmod 0755 "$PKG/DEBIAN/postinst" "$PKG/DEBIAN/postrm"

dpkg-deb --root-owner-group -Zxz --build "$PKG" "$OUT" >/dev/null
echo "$OUT"
