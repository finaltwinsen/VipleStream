#!/bin/bash
# VipleStream §S3（M2a，PoC-F-pre）— 在 x86_64 dev Flatpak 沙箱內，用 SteamVR 的 OpenXR runtime 跑 xr-probe
#
#   bash moonlight-qt/scripts/steamframe/s3/run-xr-probe-steamvr.sh --steam-root DIR
#        [--runtime-json PATH] [--app-id ID] [--branch dev] [--out DIR]
#
# 回答 R2 的前半：KDE runtime 沙箱內能不能載入 SteamVR 的 OpenXR runtime（Frame 上的 PoC-F 之前，
# 先在 linux-builder 上驗證機制）。跑三件事，全部記錄下來：
#   A. auto：只靠 manifest 的 finish-args 與 app 自己的 runtime 探索（XrRuntimeJson::resolveActive）
#      ——驗證 finish-args 對「原生 Steam 預設佈局」是否足夠；
#   B. explicit：flatpak run 臨時加 --filesystem=<SteamVR>:ro，並以 --xr-runtime-json 指定 runtime JSON
#      ——排除路徑可見性後，runtime 的 .so 在沙箱內能不能載入（PoC-F-pre 的主要答案）；
#   C. 沙箱內 ldd runtime 的 .so（KDE runtime 缺哪些函式庫），對照 host 上的 ldd。
#
# 前提（見 s3/README.md）：x86_64 dev Flatpak 已用 build-steamframe.sh --smoke 或
# flatpak install --user --bundle 裝好；隔離的 Steam 已由使用者登入並裝好 SteamVR；null driver
# 已設定（write-null-driver-vrsettings.sh）；SteamVR 最好已啟動（vrserver 在跑）。
# 本腳本不登入 Steam、不啟動 Steam／SteamVR、不改任何設定、不連網。
#
# 結束碼：B 的 xr-probe 結束碼（0 載入成功；13 找不到或載不起 runtime；10 本建置沒有 OpenXR）；
#         1 參數錯誤；2 app 沒裝；4 找不到 SteamVR 或 runtime JSON。
set -u

APP_ID="io.github.finaltwinsen.VipleStream"
BRANCH="dev"
ARCH="x86_64"
STEAM_ROOT=""
RUNTIME_JSON=""
OUT=""
PROBE_TIMEOUT=300

usage()
{
	cat <<'EOF'
用法：
  bash run-xr-probe-steamvr.sh --steam-root DIR [選項]

  --steam-root DIR      Steam 根目錄（含 steamapps/）；SteamVR 在 DIR/steamapps/common/SteamVR
  --steamvr-dir DIR     SteamVR 不在上述位置時（另一個 Steam library）直接指定
  --runtime-json PATH   SteamVR 的 OpenXR runtime JSON（預設在 SteamVR 目錄找 steamxr_linux64.json，
                        找不到時找含 "runtime" 的 *.json）
  --app-id ID           預設 io.github.finaltwinsen.VipleStream
  --branch B            預設 dev
  --out DIR             輸出目錄（預設 ~/viple-s3-xrprobe-<時間>）
EOF
}

STEAMVR=""
while [ $# -gt 0 ]; do
	case "$1" in
		--steam-root) [ $# -ge 2 ] || { usage 1>&2; exit 1; }; STEAM_ROOT=$2; shift 2 ;;
		--steamvr-dir) [ $# -ge 2 ] || { usage 1>&2; exit 1; }; STEAMVR=$2; shift 2 ;;
		--runtime-json) [ $# -ge 2 ] || { usage 1>&2; exit 1; }; RUNTIME_JSON=$2; shift 2 ;;
		--app-id) [ $# -ge 2 ] || { usage 1>&2; exit 1; }; APP_ID=$2; shift 2 ;;
		--branch) [ $# -ge 2 ] || { usage 1>&2; exit 1; }; BRANCH=$2; shift 2 ;;
		--out) [ $# -ge 2 ] || { usage 1>&2; exit 1; }; OUT=$2; shift 2 ;;
		-h|--help) usage; exit 0 ;;
		*) echo "不認得的參數：$1" 1>&2; usage 1>&2; exit 1 ;;
	esac
done

say()
{
	printf '[s3-xrprobe] %s\n' "$*"
	[ -n "${OUT:-}" ] && [ -d "$OUT" ] && printf '%s\n' "$*" >> "$OUT/summary.txt"
	return 0
}

if [ -z "$STEAMVR" ]; then
	[ -n "$STEAM_ROOT" ] || { usage 1>&2; exit 1; }
	STEAMVR="$STEAM_ROOT/steamapps/common/SteamVR"
fi
if [ ! -d "$STEAMVR" ]; then
	say "找不到 SteamVR 目錄：$STEAMVR（SteamVR 裝了嗎？在別的 library 就用 --steamvr-dir）"
	exit 4
fi
STEAMVR=$(readlink -f "$STEAMVR")

if [ -z "$RUNTIME_JSON" ]; then
	if [ -f "$STEAMVR/steamxr_linux64.json" ]; then
		RUNTIME_JSON="$STEAMVR/steamxr_linux64.json"
	else
		for f in "$STEAMVR"/*.json; do
			[ -f "$f" ] || continue
			if grep -q '"runtime"' "$f" 2>/dev/null; then
				RUNTIME_JSON=$f
				break
			fi
		done
	fi
fi
if [ -z "$RUNTIME_JSON" ] || [ ! -f "$RUNTIME_JSON" ]; then
	say "找不到 SteamVR 的 OpenXR runtime JSON（檔名 UNVERIFIED）。SteamVR 目錄下的 JSON："
	ls -l "$STEAMVR"/*.json 2>&1 | sed 's/^/    /'
	say "用 --runtime-json 指定後重跑"
	exit 4
fi
RUNTIME_JSON=$(readlink -f "$RUNTIME_JSON")

if ! command -v flatpak >/dev/null 2>&1; then
	say "沒有 flatpak 指令"
	exit 2
fi
if ! flatpak info --arch="$ARCH" "$APP_ID//$BRANCH" >/dev/null 2>&1; then
	say "$APP_ID//$BRANCH（$ARCH）沒有安裝。先跑："
	say "  bash moonlight-qt/scripts/build-steamframe.sh --arch x86_64 --flavor dev --smoke"
	say "或 flatpak install --user --bundle <VipleStream-Client-…-linux-x64-dev.flatpak>"
	exit 2
fi

TS=$(date +%Y%m%d-%H%M%S)
[ -n "$OUT" ] || OUT="$HOME/viple-s3-xrprobe-$TS"
mkdir -p "$OUT" || { echo "無法建立 $OUT" 1>&2; exit 1; }
: > "$OUT/summary.txt"

# SSH 下 flatpak run 需要的環境（有就沿用）
if [ -z "${XDG_RUNTIME_DIR:-}" ] && [ -d "/run/user/$(id -u)" ]; then
	XDG_RUNTIME_DIR="/run/user/$(id -u)"
	export XDG_RUNTIME_DIR
fi
if [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ] && [ -n "${XDG_RUNTIME_DIR:-}" ] && [ -S "$XDG_RUNTIME_DIR/bus" ]; then
	export DBUS_SESSION_BUS_ADDRESS="unix:path=$XDG_RUNTIME_DIR/bus"
fi

REF="$APP_ID//$BRANCH"
PROBE_DIR="$HOME/.var/app/$APP_ID/data/s3-$TS"
mkdir -p "$PROBE_DIR"

say "SteamVR：$STEAMVR"
say "runtime JSON：$RUNTIME_JSON"
say "app：$REF（$ARCH）"
say "輸出：$OUT"

# runtime JSON 的 library_path（相對路徑以 JSON 所在目錄解析）
LIB=$(sed -n 's/.*"library_path"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$RUNTIME_JSON" | head -n 1)
case "$LIB" in
	"") ;;
	/*) ;;
	*) LIB="$(dirname "$RUNTIME_JSON")/$LIB" ;;
esac

{
	echo "== runtime JSON"
	cat "$RUNTIME_JSON"
	echo
	echo "== library_path：${LIB:-（沒有）}"
	[ -n "$LIB" ] && ls -l "$LIB" 2>&1
	[ -n "$LIB" ] && command -v file >/dev/null 2>&1 && file -L "$LIB" 2>&1
	echo
	echo "== host ldd"
	[ -n "$LIB" ] && ldd "$LIB" 2>&1
	echo
	echo "== ~/.config/openxr/1"
	ls -la "$HOME/.config/openxr/1" 2>&1
	cat "$HOME/.config/openxr/1/active_runtime.json" 2>&1
	echo
	echo "== vrserver"
	pgrep -a vrserver 2>&1 || echo "vrserver 沒有在跑（xr-probe 可能要等 SteamVR 啟動，或失敗）"
} > "$OUT/host-side.txt" 2>&1

run_case()
{
	local name=$1
	shift
	local rc
	timeout --kill-after=10 "$PROBE_TIMEOUT" "$@" > "$OUT/$name.txt" 2>&1 < /dev/null
	rc=$?
	say "$name rc=$rc（$OUT/$name.txt）"
	return "$rc"
}

# A. auto：只靠 finish-args 與 app 自己的探索
run_case xr-probe-auto \
	flatpak run --arch="$ARCH" "$REF" xr-probe --loader-debug --json "$PROBE_DIR/xr-probe-auto.json"
RC_A=$?

# B. explicit：臨時開放 SteamVR 目錄，指定 runtime JSON
run_case xr-probe-explicit \
	flatpak run --arch="$ARCH" --filesystem="$STEAMVR:ro" "$REF" \
	xr-probe --xr-runtime-json "$RUNTIME_JSON" --loader-debug --json "$PROBE_DIR/xr-probe-explicit.json"
RC_B=$?

# C. 沙箱內 ldd（KDE runtime 缺哪些函式庫）
if [ -n "$LIB" ]; then
	run_case sandbox-ldd \
		flatpak run --arch="$ARCH" --filesystem="$STEAMVR:ro" --command=ldd "$REF" "$LIB"
fi

cp -a "$PROBE_DIR/." "$OUT/json/" 2>/dev/null

say "結果：auto rc=$RC_A，explicit rc=$RC_B（0＝載入成功；13＝找不到或載不起 runtime，JSON 內有 dlerror；10＝這個建置沒有 CONFIG+=openxr）"
if [ "$RC_B" -eq 0 ] && [ "$RC_A" -ne 0 ]; then
	say "判讀：runtime 本身能在沙箱內載入，但 finish-args 的預設路徑不夠（這個佈局需要額外的 --filesystem）"
fi
say "代表性限制：x86_64 SteamVR ≠ Frame 上的 arm64 SteamVR；null driver ≠ 真實 HMD；Flatpak 版 Steam 的佈局 ≠ Frame 原生佈局"
exit "$RC_B"
