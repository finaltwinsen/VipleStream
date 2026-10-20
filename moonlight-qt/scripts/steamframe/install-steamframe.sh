#!/bin/bash
# VipleStream for Steam Frame：安裝／更新／移除（在頭盔上以一般使用者執行，不需要 root）。
#
# 為什麼不能只裝 .flatpak：Flatpak 一定會替 app 開獨立的 PID namespace（沒有 finish-arg 能關），Frame 的 SteamVR
# 用 PID 辨識 OpenXR client，兩邊對不上會讓 SteamVR 出錯甚至重啟（finish-args.md §6）。程式偵測到這種沙盒就停用
# VR 模式與影片模式（vrlauncher.cpp 的 inPidNamespacedSandbox），只剩桌面模式。這支腳本另外裝一個啟動器：
#   - 以 FLATPAK_BWRAP 換上只拿掉 --unshare-pid 的 bwrap 包裝（Flatpak 的其他隔離都還在）；
#   - 執行期加上 OpenXR 連 SteamVR 需要的 /opt/steamvr（唯讀）、/tmp、/dev/shm（這三條不進 manifest，理由同上）；
#   - 把啟動器加進 Steam 收藏庫，並蓋掉選單裡 Flatpak 匯出的捷徑，兩個入口開的都是同一個啟動器；
#   - 收藏庫（與 SteamVR）的封面圖與圖示：從安裝好的 app 複製（/app/share/viplestream/steam/）。Flatpak 匯出的圖示只有
#     SVG 主題名稱，Steam 捷徑認不得，沒有這一步捷徑就沒有圖。
#
# 用法：
#   bash install-steamframe.sh [VipleStream-Client-X.Y.Z-linux-arm64.flatpak]   安裝或更新
#   bash install-steamframe.sh --no-steam [bundle]                               不加 Steam 捷徑
#   bash install-steamframe.sh --uninstall                                       移除（Steam 捷徑要自己刪）
# 沒給 bundle 時，在這支腳本所在的資料夾與目前資料夾找版號最新的 VipleStream-Client-*-linux-arm64.flatpak。
set -euo pipefail

APP_ID=io.github.finaltwinsen.VipleStream
BRANCH=stable
DEST="$HOME/.local/share/viplestream-frame"
LAUNCHER="$DEST/VipleStream"
SHIM="$DEST/bwrap-nopid"
DESKTOP="$HOME/.local/share/applications/$APP_ID.desktop"

die() { echo "error: $*" >&2; exit 1; }
say() { echo "[viplestream] $*"; }

ADD_STEAM=1
UNINSTALL=0
BUNDLE=""
for a in "$@"; do
    case "$a" in
        --no-steam) ADD_STEAM=0 ;;
        --uninstall) UNINSTALL=1 ;;
        -h|--help) sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*) die "unknown option: $a (see --help)" ;;
        *) BUNDLE="$a" ;;
    esac
done

[ "$(id -u)" != 0 ] || die "run this as your normal user, not root"
command -v flatpak >/dev/null || die "flatpak not found"
[ -x /usr/bin/bwrap ] || die "/usr/bin/bwrap not found"

if [ "$UNINSTALL" = 1 ]; then
    rm -f "$LAUNCHER" "$SHIM" "$DESKTOP"
    rmdir "$DEST" 2>/dev/null || true
    flatpak --user uninstall -y "$APP_ID//$BRANCH" 2>/dev/null || true
    say "removed. Delete the VipleStream shortcut from your Steam library by hand."
    exit 0
fi

[ "$(uname -m)" = aarch64 ] || die "this is for the Steam Frame (aarch64); this machine is $(uname -m)"

# 找 bundle：腳本所在資料夾優先，再找目前資料夾；同時有多個版本時取版號最大的
if [ -z "$BUNDLE" ]; then
    here="$(dirname "$(readlink -f "$0")")"
    BUNDLE="$( { ls "$here"/VipleStream-Client-*-linux-arm64.flatpak "$PWD"/VipleStream-Client-*-linux-arm64.flatpak 2>/dev/null || true; } \
               | sort -uV | tail -n 1)"
    [ -n "$BUNDLE" ] || die "no VipleStream-Client-*-linux-arm64.flatpak next to this script or in $PWD; pass its path"
fi
[ -f "$BUNDLE" ] || die "not found: $BUNDLE"

say "installing $(basename "$BUNDLE") (the first install also downloads the KDE runtime from Flathub)"
flatpak --user install -y --noninteractive --reinstall --bundle "$BUNDLE"

mkdir -p "$DEST" "$(dirname "$DESKTOP")"

# bwrap 包裝：只拿掉 --unshare-pid。flatpak 把大部分 bwrap 參數放在 --args <fd>（NUL 分隔），少數直接放在命令列，兩種都處理
cat > "$SHIM" <<'EOF'
#!/bin/bash
# VipleStream（install-steamframe.sh 安裝）：轉呼叫 /usr/bin/bwrap，只拿掉 --unshare-pid，其他參數原樣保留
out=()
while [ $# -gt 0 ]; do
    case "$1" in
        --args)
            mapfile -d '' -t inner < "/proc/self/fd/$2"
            filt=()
            for a in "${inner[@]}"; do [ "$a" = --unshare-pid ] || filt+=("$a"); done
            exec {nfd}< <(printf '%s\0' "${filt[@]}")
            out+=(--args "$nfd")
            shift 2
            ;;
        --unshare-pid) shift ;;
        *) out+=("$1"); shift ;;
    esac
done
exec /usr/bin/bwrap "${out[@]}"
EOF

cat > "$LAUNCHER" <<EOF
#!/bin/bash
# VipleStream（install-steamframe.sh 安裝）：不開 PID namespace，並開放 OpenXR 連 SteamVR 需要的路徑
export FLATPAK_BWRAP="$SHIM"
exec /usr/bin/flatpak run --branch=$BRANCH --arch=aarch64 \\
    --filesystem=/opt/steamvr:ro --filesystem=/tmp --device=shm \\
    $APP_ID "\$@"
EOF
chmod 755 "$SHIM" "$LAUNCHER"

# Steam 用的封面圖與圖示（2.0.3 起的 bundle 才有）
ART="$(flatpak --user info --show-location "$APP_ID//$BRANCH" 2>/dev/null)/files/share/viplestream/steam"
ICON="$APP_ID"
if [ -f "$ART/icon.png" ]; then
    cp "$ART/icon.png" "$DEST/icon.png"
    ICON="$DEST/icon.png"
fi

# 和 Flatpak 匯出的 .desktop 同名：~/.local/share/applications 排在 exports 前面，選單裡的捷徑也改走啟動器
cat > "$DESKTOP" <<EOF
[Desktop Entry]
Name=VipleStream
Comment=Stream games, SteamVR and videos from your PC
Exec=$LAUNCHER
Icon=$ICON
StartupWMClass=viplestream
Terminal=false
Type=Application
Categories=Qt;Game;Network;RemoteAccess;
EOF

# 自我檢查：沙盒裡的 1 號行程要是 systemd（沒有 PID namespace），不是 bwrap
pid1="$(FLATPAK_BWRAP="$SHIM" flatpak run --branch=$BRANCH --arch=aarch64 --command=cat "$APP_ID" /proc/1/comm 2>/dev/null || true)"
if [ "$pid1" = bwrap ] || [ -z "$pid1" ]; then
    die "launcher check failed (sandbox PID 1 = '${pid1:-none}'); VR and video mode would stay disabled"
fi
say "launcher check OK (sandbox PID 1 = $pid1)"

# 捷徑的 appid（Steam 寫在 shortcuts.vdf，binary VDF）：印出「<userdata 的 config 資料夾> <appid>」，每個找到的一行
shortcut_ids() {
    python3 - "$LAUNCHER" "$HOME"/.local/share/Steam/userdata/*/config/shortcuts.vdf <<'PY' 2>/dev/null
import os, struct, sys

def parse(d, i):
    obj = {}
    while True:
        t = d[i]; i += 1
        if t == 0x08:
            return obj, i
        j = d.index(b"\0", i); key = d[i:j].decode("utf-8", "replace").lower(); i = j + 1
        if t == 0x00:
            v, i = parse(d, i)
        elif t == 0x01:
            j = d.index(b"\0", i); v = d[i:j]; i = j + 1
        elif t == 0x02:
            v = struct.unpack("<i", d[i:i + 4])[0]; i += 4
        elif t == 0x07:
            v = d[i:i + 8]; i += 8
        else:
            raise ValueError(t)
        obj[key] = v

launcher = sys.argv[1].encode()
for path in sys.argv[2:]:
    try:
        root, _ = parse(open(path, "rb").read(), 0)
    except Exception:
        continue
    for e in root.get("shortcuts", {}).values():
        if isinstance(e, dict) and launcher in e.get("exe", b"") and "appid" in e:
            print(os.path.dirname(path), e["appid"] & 0xFFFFFFFF)
PY
}

# 封面圖放進 Steam 的 grid 資料夾（<appid>.png 橫幅、p 直式、_hero、_logo）。Steam 開著時可能要重開才看得到
install_art() {
    [ -f "$ART/capsule.png" ] || return 0
    local ids="" i
    for i in $(seq 1 15); do  # steamos-add-to-steam 是請執行中的 Steam 加，寫進 shortcuts.vdf 要一點時間
        ids="$(shortcut_ids)"
        [ -n "$ids" ] && break
        sleep 1
    done
    if [ -z "$ids" ]; then
        say "the Steam shortcut is not saved yet: run this script again later to add its library artwork"
        return 0
    fi
    while read -r cfg appid; do
        mkdir -p "$cfg/grid"
        cp "$ART/capsule.png" "$cfg/grid/$appid.png"
        cp "$ART/portrait.png" "$cfg/grid/${appid}p.png"
        cp "$ART/hero.png" "$cfg/grid/${appid}_hero.png"
        cp "$ART/logo.png" "$cfg/grid/${appid}_logo.png"
        say "Steam library artwork set (shortcut $appid)"
    done <<< "$ids"
}

if [ "$ADD_STEAM" = 1 ]; then
    if grep -qs -- "$LAUNCHER" "$HOME"/.local/share/Steam/userdata/*/config/shortcuts.vdf; then
        say "Steam shortcut already present"
        install_art
    elif command -v steamos-add-to-steam >/dev/null && pgrep -x steam >/dev/null; then
        steamos-add-to-steam "$DESKTOP" && say "added VipleStream to your Steam library" && install_art
    else
        say "Steam is not running: add it later with  steamos-add-to-steam $DESKTOP  and run this script again for the artwork"
    fi
fi

say "done. Start VipleStream from your Steam library (not from 'flatpak run')."
