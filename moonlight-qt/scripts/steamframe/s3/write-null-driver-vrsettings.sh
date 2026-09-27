#!/bin/bash
# VipleStream §S3（M2a，PoC-F-pre 前置）— 在「隔離的」Steam 使用者目錄寫入 SteamVR null driver 設定
#
#   bash moonlight-qt/scripts/steamframe/s3/write-null-driver-vrsettings.sh --steam-root DIR [--apply]
#        [--render WxH] [--hz N] [--window WxH]
#
# 目的：linux-builder 沒有頭盔，S3 用 SteamVR 的 null driver 假裝有一台 HMD，讓 SteamVR 的
# OpenXR runtime 與 vrserver 能起來（PoC-F-pre、α overlay、PoC-2b 的前置）。
#
# 寫入 <steam-root>/config/steamvr.vrsettings（使用者層級設定，會覆寫 driver 預設值）：
#   steamvr.forcedDriver=null、steamvr.activateMultipleDrivers=true、steamvr.enableHomeApp=false、
#   driver_null.enable=true 與顯示幾何（預設每眼 1728×1728、90 Hz，對應 VR emulate 樣本 3456×1728@90）。
# 這組鍵值是社群慣用的 null driver 設定，不同 SteamVR 版本是否都認 **UNVERIFIED**；SteamVR 不認
# 使用者層級的 driver_null.enable 時，要改 SteamVR 安裝目錄的
# drivers/null/resources/settings/default.vrsettings——本腳本刻意不動 SteamVR 安裝目錄。
#
# 安全規則：
#   - 預設 dry-run：只印出目標路徑與要寫入的內容；加 --apply 才寫。
#   - 只寫「隔離」的 Steam，兩種都要操作者刻意建立的標記檔：
#       a. 專為 S3 新裝、平常沒在用的 Flatpak 版 Steam（~/.var/app/com.valvesoftware.Steam/…），而且
#          ~/.var/app/com.valvesoftware.Steam/.viple-s3-isolated 存在。只看路徑分不出「為 S3 新裝的」與
#          「日常就在用的 Flatpak Steam」（Ubuntu 上 Flathub 版是常見的日常安裝方式），所以一樣要標記。
#       b. 這個帳號是 S3 專用帳號：$HOME/.viple-s3-isolated 存在。
#     其他情況一律拒絕——不改使用者日常用的 Steam 設定。
#   - SteamVR（vrserver）在跑時拒絕寫入（它結束時會把記憶體內的設定寫回、蓋掉我們的改動）。
#   - 寫之前備份成 steamvr.vrsettings.bak-<時間>，並印出還原指令。
#   - 不登入 Steam、不啟動 Steam、不連網。
set -u

STEAM_ROOT=""
APPLY=0
RENDER="1728x1728"
HZ="90"
WINDOW="1920x1080"

usage()
{
	cat <<'EOF'
用法：
  bash write-null-driver-vrsettings.sh --steam-root DIR [--apply] [--render WxH] [--hz N] [--window WxH]

  --steam-root DIR   隔離 Steam 的根目錄（含 steamapps/、config/ 的那一層），例如
                     ~/.var/app/com.valvesoftware.Steam/.local/share/Steam（專為 S3 新裝的 Flatpak 版
                     Steam；要先 touch ~/.var/app/com.valvesoftware.Steam/.viple-s3-isolated），或
                     S3 專用帳號的 ~/.local/share/Steam（該帳號要先 touch ~/.viple-s3-isolated）
  --apply            真的寫入（預設只印出來）
  --render WxH       null HMD 每眼渲染解析度（預設 1728x1728）
  --hz N             顯示頻率（預設 90）
  --window WxH       null driver 的桌面視窗大小（預設 1920x1080）

結束碼：0 成功（或 dry-run）；1 參數錯誤；2 隔離檢查未通過／SteamVR 正在跑；3 寫入失敗
EOF
}

while [ $# -gt 0 ]; do
	case "$1" in
		--steam-root) [ $# -ge 2 ] || { usage 1>&2; exit 1; }; STEAM_ROOT=$2; shift 2 ;;
		--steam-root=*) STEAM_ROOT=${1#*=}; shift ;;
		--apply) APPLY=1; shift ;;
		--render) [ $# -ge 2 ] || { usage 1>&2; exit 1; }; RENDER=$2; shift 2 ;;
		--hz) [ $# -ge 2 ] || { usage 1>&2; exit 1; }; HZ=$2; shift 2 ;;
		--window) [ $# -ge 2 ] || { usage 1>&2; exit 1; }; WINDOW=$2; shift 2 ;;
		-h|--help) usage; exit 0 ;;
		*) echo "不認得的參數：$1" 1>&2; usage 1>&2; exit 1 ;;
	esac
done

say()
{
	printf '[s3-vrsettings] %s\n' "$*"
}

wxh_ok()
{
	case "$1" in
		[1-9]*x[1-9]*) ;;
		*) return 1 ;;
	esac
	case "$1" in
		*[!0-9x]*|*x*x*) return 1 ;;
	esac
	return 0
}

[ -n "$STEAM_ROOT" ] || { usage 1>&2; exit 1; }
wxh_ok "$RENDER" || { say "--render 格式要 WxH（收到 $RENDER）"; exit 1; }
wxh_ok "$WINDOW" || { say "--window 格式要 WxH（收到 $WINDOW）"; exit 1; }
case "$HZ" in
	''|*[!0-9]*) say "--hz 要正整數（收到 $HZ）"; exit 1 ;;
esac

[ -d "$STEAM_ROOT" ] || { say "不是目錄：$STEAM_ROOT"; exit 1; }
ROOT=$(readlink -f "$STEAM_ROOT")
if [ ! -d "$ROOT/steamapps" ] && [ ! -d "$ROOT/config" ] && [ ! -e "$ROOT/steam.sh" ]; then
	say "$ROOT 看起來不是 Steam 根目錄（沒有 steamapps/、config/、steam.sh）"
	exit 1
fi

# ---------------------------------------------------------------------------
# 隔離檢查
# ---------------------------------------------------------------------------
REAL_HOME=$(readlink -f "$HOME")
FLATPAK_STEAM_DIR="$REAL_HOME/.var/app/com.valvesoftware.Steam"
ISOLATION=""
IS_FLATPAK_STEAM=0
case "$ROOT/" in
	"$FLATPAK_STEAM_DIR/"*) IS_FLATPAK_STEAM=1 ;;
esac
# 標記放在 app 的私有資料根目錄（Steam 根目錄之外），Steam 自己的更新與清理碰不到。
if [ "$IS_FLATPAK_STEAM" -eq 1 ] && [ -f "$FLATPAK_STEAM_DIR/.viple-s3-isolated" ]; then
	ISOLATION="專為 S3 裝的 Flatpak 版 Steam（$FLATPAK_STEAM_DIR/.viple-s3-isolated 存在）"
fi
if [ -z "$ISOLATION" ] && [ -f "$HOME/.viple-s3-isolated" ]; then
	case "$ROOT/" in
		"$REAL_HOME/"*) ISOLATION="S3 專用帳號（$HOME/.viple-s3-isolated 存在）" ;;
	esac
fi
if [ -z "$ISOLATION" ]; then
	say "拒絕：$ROOT 不是隔離的 Steam。只接受（兩種都要操作者刻意建立的標記檔）："
	say "  (a) 專為 S3 新裝、平常沒在用的 Flatpak 版 Steam（~/.var/app/com.valvesoftware.Steam/… 底下），"
	say "      而且 ~/.var/app/com.valvesoftware.Steam/.viple-s3-isolated 存在；"
	say "  (b) S3 專用帳號自己家目錄底下的 Steam，而且該帳號已 touch ~/.viple-s3-isolated。"
	if [ "$IS_FLATPAK_STEAM" -eq 1 ]; then
		say "這是 Flatpak 版 Steam，但沒有 $FLATPAK_STEAM_DIR/.viple-s3-isolated。"
		say "如果它是你日常用的 Steam，不要建立標記，改用 s3/README.md 的做法 A（S3 專用帳號）。"
	fi
	say "不要對日常使用的 Steam 跑這支（見 s3/README.md 的隔離說明）。"
	exit 2
fi
say "隔離：$ISOLATION"

if pgrep -x vrserver >/dev/null 2>&1; then
	say "拒絕：vrserver 正在執行。先關掉 SteamVR 再寫（它結束時會把設定寫回、蓋掉改動）。"
	exit 2
fi

TARGET="$ROOT/config/steamvr.vrsettings"
RW=${RENDER%x*}
RH=${RENDER#*x}
WW=${WINDOW%x*}
WH=${WINDOW#*x}

FRAGMENT=$(cat <<EOF
{
   "steamvr" : {
      "forcedDriver" : "null",
      "activateMultipleDrivers" : true,
      "enableHomeApp" : false
   },
   "driver_null" : {
      "enable" : true,
      "serialNumber" : "VIPLE-S3-NULL",
      "modelNumber" : "VIPLE-S3-NULL",
      "windowX" : 0,
      "windowY" : 0,
      "windowWidth" : $WW,
      "windowHeight" : $WH,
      "renderWidth" : $RW,
      "renderHeight" : $RH,
      "secondsFromVsyncToPhotons" : 0.011,
      "displayFrequency" : $HZ
   }
}
EOF
)

say "目標：$TARGET（$([ -f "$TARGET" ] && echo 已存在，會合併 || echo 不存在，會新建)）"
say "要寫入的鍵值："
printf '%s\n' "$FRAGMENT"

if [ "$APPLY" -eq 0 ]; then
	say "DRY-RUN：沒有寫入。確認無誤後加 --apply。"
	exit 0
fi

mkdir -p "$ROOT/config" || { say "無法建立 $ROOT/config"; exit 3; }

if [ -f "$TARGET" ]; then
	PY=$(command -v python3 2>/dev/null || true)
	if [ -z "$PY" ]; then
		say "$TARGET 已存在，合併需要 python3（沒有）。請手動把上面的鍵值併進該檔的 steamvr／driver_null 區段。"
		exit 3
	fi
	BACKUP="$TARGET.bak-$(date +%Y%m%d-%H%M%S)"
	cp -p -- "$TARGET" "$BACKUP" || { say "備份失敗"; exit 3; }
	say "備份：$BACKUP"
	# 只更新 steamvr／driver_null 兩個區段裡的這幾個鍵，其他設定原樣保留。
	if ! printf '%s' "$FRAGMENT" | "$PY" -c '
import json, sys
target = sys.argv[1]
frag = json.load(sys.stdin)
with open(target, "r", encoding="utf-8") as f:
    cur = json.load(f)
for section, values in frag.items():
    node = cur.get(section)
    if not isinstance(node, dict):
        node = {}
    node.update(values)
    cur[section] = node
tmp = target + ".viple-tmp"
with open(tmp, "w", encoding="utf-8") as f:
    json.dump(cur, f, indent=3, sort_keys=True)
    f.write("\n")
import os
os.replace(tmp, target)
' "$TARGET"; then
		say "合併失敗（$TARGET 不是合法 JSON？）；原檔未動，備份在 $BACKUP"
		exit 3
	fi
	say "已合併。還原：cp -p \"$BACKUP\" \"$TARGET\""
else
	printf '%s\n' "$FRAGMENT" > "$TARGET" || { say "寫入 $TARGET 失敗"; exit 3; }
	say "已新建。還原：rm \"$TARGET\""
fi
say "下一步：以這個隔離的 Steam 啟動 SteamVR（null driver 會出現一個 ${WW}x${WH} 的視窗），"
say "在 SteamVR 設定把它設成 OpenXR runtime，然後跑 run-xr-probe-steamvr.sh。"
exit 0
