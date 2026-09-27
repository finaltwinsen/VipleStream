#!/bin/bash
# VipleStream §SF-POC（M2a）— Steam Frame 實機證據收集（Day 0／Day 1）
#
#   bash frame-poc-collect.sh [--out DIR] [--app-id ID] [--iface IF] [--rtt-target HOST]
#                             [--with-app-probes] [--samples DIR] [--no-redact]
#
# 在 Frame 本機執行（Developer Mode 的 SSH，或桌面模式的終端機），只在本機產生
# <out>.tar.gz 與 .sha256，由 <dev-client> 用 scp 拉回。同一套流程也是 G-β／G-rc 的 log 回收 SOP
# （docs/steam_frame_client.md）。
#
# 原則：
#   - 不碰網路。唯一例外：明確給了 --rtt-target 時，對該主機跑 ip route get 與 ping（PoC-6）。
#   - 不改系統狀態：只讀檔、列目錄、跑查詢命令。--samples 會把樣本複製進
#     ~/.var/app/<app-id>/data/samples/（沙箱只看得到那裡）；--with-app-probes 會以 flatpak run
#     跑 VipleStream 的探測動作（v4l2-probe 的 header-test 會短暫佔用 VPU）。
#   - 預設遮蔽 MAC、SSID、IP、SteamID、序號；主機名稱在收集時就不產生（--no-redact 關閉遮蔽）。
#     遮蔽只作用在收集出來的副本上；遮不了的二進位檔搬到 <out>.unredacted/，不打包。
#   - 不用 set -e：單一命令失敗照樣繼續，每條命令的 rc 與耗時記進 index.tsv。
#   - 先複製既有的 app log，再跑探測（探測會寫新的 log；舊的串流證據不能被擠掉）。
#
# 收集項目對應的 PoC（docs/vr_architecture.md §7）：
#   PoC-0  V4L2 裝置、驅動、kernel 訊息；PoC-1 flatpak 安裝範圍與沙箱可見性；
#   PoC-6  Wi-Fi 連線、省電模式、（opt-in）RTT 分佈；PoC-7 gamescope 參數與環境、WSI layer；
#   PoC-F  OpenXR active runtime 與 SteamVR runtime 的函式庫依賴；
#   Day 1（--with-app-probes）：v4l2-probe、xr-probe、decode-bench（PoC-3／3b／3c／4、PoC-2、PoC-F）。
#
# run_sh 的片段刻意用單引號（由內層 bash 展開）；這個 directive 在第一個命令之前，作用於整個檔案。
# shellcheck disable=SC2016
set -u

readonly SCRIPT_VERSION="m2a-2"
APP_ID="io.github.finaltwinsen.VipleStream"
OUT=""
IFACE=""
RTT_TARGET=""
WITH_APP_PROBES=0
SAMPLES=""
REDACT=1

RUN_TIMEOUT=60
PROBE_TIMEOUT=600

usage()
{
	cat <<'EOF'
VipleStream Steam Frame 實機證據收集（Day 0／Day 1）

用法：
  bash frame-poc-collect.sh [選項]

選項：
  --out DIR           輸出目錄（預設 ~/viplestream-poc-<時間>）；結束時產生 DIR.tar.gz 與 DIR.tar.gz.sha256
  --app-id ID         VipleStream 的 Flatpak app-id（預設 io.github.finaltwinsen.VipleStream）
  --iface IF          Wi-Fi 介面（預設自動偵測第一個無線介面）
  --rtt-target HOST   對 HOST 量 RTT（PoC-6）：基準 ping，再以 180 pps、232 B 量上行負載下的分佈。
                      沒給就完全不碰網路。
  --with-app-probes   Day 1：以 flatpak run 跑 v4l2-probe（含 header-test）、xr-probe、decode-bench
  --samples DIR       把 DIR 內的樣本複製到 ~/.var/app/<app-id>/data/samples/（沙箱看得到的位置）；
                      搭配 --with-app-probes 時對每個 .h264/.264/.hevc/.h265/.265/.ivf 跑 decode-bench：
                      連發（auto）、--fps 90（依串流節拍送，量每幀延遲）、--decoder sw 各一次
  --no-redact         不遮蔽 MAC、SSID、IP、SteamID、序號、主機名稱（預設遮蔽；遮蔽是規則式，
                      貼進公開文件前仍要人工檢查）
  -h, --help          顯示這段說明

拉回（在 <dev-client> 上）：
  scp <frame-user>@<frame>:<輸出目錄>.tar.gz <frame-user>@<frame>:<輸出目錄>.tar.gz.sha256 .
  sha256sum -c <輸出目錄名>.tar.gz.sha256
EOF
}

while [ $# -gt 0 ]; do
	case "$1" in
		--out) [ $# -ge 2 ] || { echo "--out 需要值" 1>&2; exit 1; }; OUT=$2; shift 2 ;;
		--out=*) OUT=${1#*=}; shift ;;
		--app-id) [ $# -ge 2 ] || { echo "--app-id 需要值" 1>&2; exit 1; }; APP_ID=$2; shift 2 ;;
		--app-id=*) APP_ID=${1#*=}; shift ;;
		--iface) [ $# -ge 2 ] || { echo "--iface 需要值" 1>&2; exit 1; }; IFACE=$2; shift 2 ;;
		--iface=*) IFACE=${1#*=}; shift ;;
		--rtt-target) [ $# -ge 2 ] || { echo "--rtt-target 需要值" 1>&2; exit 1; }; RTT_TARGET=$2; shift 2 ;;
		--rtt-target=*) RTT_TARGET=${1#*=}; shift ;;
		--with-app-probes) WITH_APP_PROBES=1; shift ;;
		--samples) [ $# -ge 2 ] || { echo "--samples 需要值" 1>&2; exit 1; }; SAMPLES=$2; shift 2 ;;
		--samples=*) SAMPLES=${1#*=}; shift ;;
		--redact) REDACT=1; shift ;;
		--no-redact) REDACT=0; shift ;;
		-h|--help) usage; exit 0 ;;
		*) echo "不認得的參數：$1（--help 看用法）" 1>&2; exit 1 ;;
	esac
done

case "$APP_ID" in
	*[!A-Za-z0-9._-]*|'') echo "--app-id 格式不對：$APP_ID" 1>&2; exit 1 ;;
esac
case "$RTT_TARGET" in
	*[!A-Za-z0-9.:_-]*) echo "--rtt-target 只接受主機名稱或 IP：$RTT_TARGET" 1>&2; exit 1 ;;
esac
if [ -n "$SAMPLES" ] && [ ! -d "$SAMPLES" ]; then
	echo "--samples 不是目錄：$SAMPLES" 1>&2
	exit 1
fi

TS=$(date +%Y%m%d-%H%M%S)
[ -n "$OUT" ] || OUT="$HOME/viplestream-poc-$TS"
OUT=${OUT%/}
case "$OUT" in
	/*) ;;
	*) OUT="$PWD/$OUT" ;;
esac
if [ -d "$OUT" ] && [ -n "$(find "$OUT" -mindepth 1 -maxdepth 1 -print -quit 2>/dev/null)" ]; then
	echo "輸出目錄已存在且不是空的：$OUT（換一個 --out）" 1>&2
	exit 1
fi
mkdir -p "$OUT" || { echo "無法建立 $OUT" 1>&2; exit 1; }
INDEX="$OUT/index.tsv"
printf 'name\trc\tseconds\tcommand\n' > "$INDEX"

have()
{
	command -v "$1" >/dev/null 2>&1
}

note()
{
	printf '[frame-poc-collect] %(%H:%M:%S)T %s\n' -1 "$*"
	printf '%(%Y-%m-%dT%H:%M:%S%z)T %s\n' -1 "$*" >> "$OUT/collector.log"
}

# run <name> <cmd…>：輸出寫到 <out>/<name>.txt（name 可含子目錄），rc 與耗時記進 index.tsv。
# 失敗不中斷；有 timeout 時加上逾時保護。
run_timeout()
{
	local limit=$1 name=$2
	shift 2
	local file="$OUT/$name.txt" start rc cmdline
	mkdir -p "$(dirname "$file")"
	# 給人看的命令列（index.tsv 一行一筆；不做 shell 跳脫）
	cmdline="$*"
	printf '$ %s\n\n' "$cmdline" > "$file"
	start=$SECONDS
	if have timeout; then
		timeout --kill-after=10 "$limit" "$@" >> "$file" 2>&1 < /dev/null
		rc=$?
	else
		"$@" >> "$file" 2>&1 < /dev/null
		rc=$?
	fi
	printf '%s\t%d\t%d\t%s\n' "$name" "$rc" "$((SECONDS - start))" "$cmdline" >> "$INDEX"
	return 0
}

run()
{
	run_timeout "$RUN_TIMEOUT" "$@"
}

# run_sh <name> <shell 片段>：需要 glob、管線、迴圈時用。
run_sh()
{
	run "$1" bash -c "$2"
}

# run_fn <name> <函式> [參數…]：本腳本內的函式（快速的本機讀檔，不加逾時）。
run_fn()
{
	local name=$1
	shift
	local file="$OUT/$name.txt" start rc
	mkdir -p "$(dirname "$file")"
	printf '$ (function) %s\n\n' "$*" > "$file"
	start=$SECONDS
	( "$@" ) >> "$file" 2>&1 < /dev/null
	rc=$?
	printf '%s\t%d\t%d\t%s\n' "$name" "$rc" "$((SECONDS - start))" "function $*" >> "$INDEX"
	return 0
}

# 環境變數白名單：STEAM_* 只記有無（可能含 token），其餘只記和顯示／XR／Vulkan 有關的。
env_whitelist()
{
	local kv k
	while IFS= read -r -d '' kv; do
		k=${kv%%=*}
		case "$k" in
			STEAM_*|SteamAppId|SteamGameId|SteamOverlayGameId) printf '%s=<set>\n' "$k" ;;
			XDG_SESSION_TYPE|XDG_CURRENT_DESKTOP|XDG_SESSION_DESKTOP|XDG_RUNTIME_DIR|XDG_CONFIG_HOME|XDG_DATA_HOME|XDG_CACHE_HOME) printf '%s\n' "$kv" ;;
			WAYLAND_DISPLAY|DISPLAY|GAMESCOPE_*|ENABLE_GAMESCOPE_WSI|DISABLE_GAMESCOPE_WSI) printf '%s\n' "$kv" ;;
			SDL_*|QT_QPA_PLATFORM|QT_SCALE_FACTOR|XR_*|VK_*|MESA_*|LIBVA_*|DBUS_SESSION_BUS_ADDRESS|LANG|LC_ALL) printf '%s\n' "$kv" ;;
			FLATPAK_ID|container) printf '%s\n' "$kv" ;;
		esac
	done < "$1"
}

self_env()
{
	env_whitelist /proc/$$/environ
	echo
	echo "# 本腳本自動補上的值（SSH 下 flatpak run 需要）："
	echo "auto_XDG_RUNTIME_DIR=${AUTO_XDG_RUNTIME_DIR:-no}"
	echo "auto_DBUS_SESSION_BUS_ADDRESS=${AUTO_DBUS:-no}"
}

gamescope_report()
{
	local pid found=0
	local -a pids=()
	mapfile -t pids < <(pgrep -f gamescope 2>/dev/null)
	for pid in "${pids[@]}"; do
		[ -r "/proc/$pid/cmdline" ] || continue
		found=1
		echo "== pid $pid"
		tr '\0' ' ' < "/proc/$pid/cmdline" | cut -c1-600
		echo
		echo "-- environ（白名單）"
		if [ -r "/proc/$pid/environ" ]; then
			env_whitelist "/proc/$pid/environ"
		else
			echo "（讀不到 /proc/$pid/environ）"
		fi
		echo
	done
	[ "$found" -eq 1 ] || echo "沒有 gamescope 行程"
	echo "== XDG_RUNTIME_DIR（${XDG_RUNTIME_DIR:-unset}）內的 socket"
	[ -n "${XDG_RUNTIME_DIR:-}" ] && ls -la "$XDG_RUNTIME_DIR" 2>&1
	return 0
}

# OpenXR active runtime（使用者層級、系統層級）與 SteamVR runtime JSON：內容、library_path、ldd。
openxr_report()
{
	local f dir lib steamvr
	local -a jsons=()
	for dir in "${XDG_CONFIG_HOME:-$HOME/.config}/openxr/1" /etc/xdg/openxr/1 /etc/openxr/1 /usr/share/openxr/1; do
		echo "== ls $dir"
		ls -la "$dir" 2>&1
		for f in "$dir"/active_runtime*.json; do
			[ -e "$f" ] && jsons+=("$f")
		done
	done
	for steamvr in "$HOME/.local/share/Steam/steamapps/common/SteamVR" "$HOME/.steam/steam/steamapps/common/SteamVR"; do
		[ -d "$steamvr" ] || continue
		echo "== ls $steamvr"
		ls -la "$steamvr" 2>&1
		echo "== ls $steamvr/bin"
		ls -la "$steamvr/bin" 2>&1
		for f in "$steamvr"/*.json; do
			[ -e "$f" ] && jsons+=("$f")
		done
	done
	echo "== ~/.config/openvr/openvrpaths.vrpath"
	cat "$HOME/.config/openvr/openvrpaths.vrpath" 2>&1
	echo
	for f in "${jsons[@]}"; do
		echo "== $f -> $(readlink -f "$f" 2>/dev/null)"
		cat "$f" 2>&1
		echo
		lib=$(sed -n 's/.*"library_path"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$f" | head -n 1)
		[ -n "$lib" ] || continue
		case "$lib" in
			/*) ;;
			*) lib="$(dirname "$(readlink -f "$f")")/$lib" ;;
		esac
		echo "-- library_path 解析後：$lib"
		ls -l "$lib" 2>&1
		have file && file -L "$lib" 2>&1
		have ldd && ldd "$lib" 2>&1
		echo
	done
	return 0
}

# ---------------------------------------------------------------------------
# 0. SSH 下的 flatpak run 需要 XDG_RUNTIME_DIR 與 session bus；有就沿用，沒有才補。
# ---------------------------------------------------------------------------
AUTO_XDG_RUNTIME_DIR=""
AUTO_DBUS=""
if [ -z "${XDG_RUNTIME_DIR:-}" ] && [ -d "/run/user/$(id -u)" ]; then
	XDG_RUNTIME_DIR="/run/user/$(id -u)"
	export XDG_RUNTIME_DIR
	AUTO_XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR
fi
if [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ] && [ -n "${XDG_RUNTIME_DIR:-}" ] && [ -S "$XDG_RUNTIME_DIR/bus" ]; then
	export DBUS_SESSION_BUS_ADDRESS="unix:path=$XDG_RUNTIME_DIR/bus"
	AUTO_DBUS=$DBUS_SESSION_BUS_ADDRESS
fi

note "輸出：$OUT（app-id=$APP_ID redact=$REDACT app-probes=$WITH_APP_PROBES rtt-target=${RTT_TARGET:+yes}）"

APP_INSTALLED=0
if have flatpak && flatpak info "$APP_ID" >/dev/null 2>&1; then
	APP_INSTALLED=1
fi

FLATPAK_LOG_DIR="$HOME/.var/app/$APP_ID/cache/VipleStream/VipleStream/logs"
NATIVE_LOG_DIR="${XDG_CACHE_HOME:-$HOME/.cache}/VipleStream/VipleStream/logs"

# ---------------------------------------------------------------------------
# 1. meta
# ---------------------------------------------------------------------------
{
	echo "script_version=$SCRIPT_VERSION"
	echo "started=$(date -Iseconds)"
	echo "app_id=$APP_ID"
	echo "app_installed=$APP_INSTALLED"
	echo "redact=$REDACT"
	echo "with_app_probes=$WITH_APP_PROBES"
	echo "rtt_target_given=$([ -n "$RTT_TARGET" ] && echo 1 || echo 0)"
	echo "samples_given=$([ -n "$SAMPLES" ] && echo 1 || echo 0)"
} > "$OUT/meta.txt"

# 不帶 -n：主機名稱可能是使用者自訂（含真名），從收集端就不產生。
run meta/uname uname -srvmpio
run_sh meta/os-release 'for f in /etc/os-release /usr/lib/os-release /etc/lsb-release /etc/steamos-release /etc/buildinfo; do [ -r "$f" ] && { echo "== $f"; cat "$f"; echo; }; done; true'
run meta/id id
run meta/uptime uptime
run_sh meta/hw 'nproc; echo; free -m; echo; df -h "$HOME" /tmp 2>&1; echo; grep -m 8 -E "model name|Hardware|CPU part|CPU implementer|Features" /proc/cpuinfo'
run meta/cmdline cat /proc/cmdline
run_fn meta/env self_env

# ---------------------------------------------------------------------------
# 2. 既有 app log（在任何探測之前複製）
# ---------------------------------------------------------------------------
MARKER="$OUT/.probe-start-marker"
: > "$MARKER"
for pair in "flatpak:$FLATPAK_LOG_DIR" "native:$NATIVE_LOG_DIR"; do
	kind=${pair%%:*}
	dir=${pair#*:}
	if [ -d "$dir" ]; then
		run "app-logs/before-$kind-copy" cp -a "$dir/." "$OUT/app-logs/before-$kind/"
	else
		printf '%s\t%s\t%s\t%s\n' "app-logs/before-$kind-copy" "-" "0" "skip: $dir 不存在" >> "$INDEX"
	fi
done

# ---------------------------------------------------------------------------
# 3. PoC-0：V4L2／DRM／kernel
# ---------------------------------------------------------------------------
run_sh video/devices 'ls -l /dev/video* /dev/media* /dev/v4l 2>&1; true'
run_sh video/sysfs-names 'for f in /sys/class/video4linux/*/name; do [ -r "$f" ] && echo "$f: $(cat "$f")"; done; true'
run_sh video/kmod 'lsmod 2>&1 | grep -i -E "iris|venus|qcom|v4l2|videobuf|msm|drm|adreno"; true'
run_sh video/dmesg 'dmesg 2>&1 | grep -i -E "iris|venus|video|v4l2|vpu|drm|msm|adreno|firmware|denied" | tail -n 400; true'
if have journalctl; then
	# --no-hostname：預設的 short 格式每一行都帶主機名稱。
	run_sh video/journal-kernel 'journalctl -k -b --no-hostname --no-pager 2>&1 | grep -i -E "iris|venus|video|v4l2|vpu|drm|msm|adreno|firmware" | tail -n 400; true'
fi
if have v4l2-ctl; then
	run video/v4l2-ctl-list-devices v4l2-ctl --list-devices
	for dev in /dev/video*; do
		[ -e "$dev" ] || continue
		n=$(basename "$dev")
		run "video/v4l2-ctl-$n-all" v4l2-ctl -d "$dev" --all
		run "video/v4l2-ctl-$n-formats-out" v4l2-ctl -d "$dev" --list-formats-out-ext
		run "video/v4l2-ctl-$n-formats-cap" v4l2-ctl -d "$dev" --list-formats-ext
		run "video/v4l2-ctl-$n-ctrls" v4l2-ctl -d "$dev" --list-ctrls-menus
	done
else
	printf '%s\t%s\t%s\t%s\n' "video/v4l2-ctl" "-" "0" "skip: 沒有 v4l2-ctl（改看 Day 1 的 v4l2-probe）" >> "$INDEX"
fi

run_sh gpu/drm 'ls -l /dev/dri 2>&1; for d in /sys/class/drm/card*/device; do [ -r "$d/uevent" ] && { echo "== $d"; cat "$d/uevent"; }; done; true'
if have vulkaninfo; then
	run gpu/vulkaninfo-summary vulkaninfo --summary
fi
# gamescope WSI layer：aarch64 版是否存在、檔名、放在哪（決定 finish-args 的 host-os:ro 要不要留）。
run_sh gpu/vulkan-layers 'for d in /usr/share/vulkan/implicit_layer.d /usr/share/vulkan/explicit_layer.d /etc/vulkan/implicit_layer.d "$HOME/.local/share/vulkan/implicit_layer.d"; do echo "== $d"; ls -l "$d" 2>&1; done; echo "== gamescope WSI .so"; ls -l /usr/lib/libVkLayer_FROG_gamescope_wsi* /usr/lib/*/libVkLayer_FROG_gamescope_wsi* /usr/lib64/libVkLayer_FROG_gamescope_wsi* 2>&1; for f in /usr/share/vulkan/implicit_layer.d/*gamescope*; do [ -r "$f" ] && { echo "== $f"; cat "$f"; }; done; true'

# ---------------------------------------------------------------------------
# 4. PoC-1：flatpak 安裝範圍與沙箱可見性
# ---------------------------------------------------------------------------
if have flatpak; then
	run flatpak/version flatpak --version
	run flatpak/installations flatpak --installations
	run flatpak/remotes flatpak remotes --show-details
	run flatpak/list-app flatpak list --app --columns=application,version,branch,arch,origin,installation
	run flatpak/list-runtime flatpak list --runtime --columns=application,version,branch,arch,origin,installation
	if [ "$APP_INSTALLED" -eq 1 ]; then
		run flatpak/app-info flatpak info "$APP_ID"
		run flatpak/app-permissions flatpak info --show-permissions "$APP_ID"
		# 沙箱內實際看得到什麼（不啟動 app，只跑 sh）。
		run flatpak/sandbox-view flatpak run --command=sh "$APP_ID" -c 'id; echo; ls -l /dev/video* /dev/media* /dev/dri /dev/hidraw* 2>&1; echo; cat /.flatpak-info; echo; echo "HOME=$HOME XDG_CONFIG_HOME=$XDG_CONFIG_HOME XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR"; ls -la "$HOME/.config/openxr/1" 2>&1; ls -la "$XDG_RUNTIME_DIR" 2>&1; ls -l /var/run/host/usr/lib/libVkLayer_FROG_gamescope_wsi* 2>&1; true'
	else
		printf '%s\t%s\t%s\t%s\n' "flatpak/app" "-" "0" "skip: $APP_ID 沒有安裝" >> "$INDEX"
	fi
else
	printf '%s\t%s\t%s\t%s\n' "flatpak" "-" "0" "skip: 沒有 flatpak 指令" >> "$INDEX"
fi

# ---------------------------------------------------------------------------
# 5. PoC-6：Wi-Fi 與路由（WARP／VPN 之類的隧道介面也看這裡）
# ---------------------------------------------------------------------------
if [ -z "$IFACE" ]; then
	for d in /sys/class/net/*; do
		if [ -d "$d/wireless" ] || [ -d "$d/phy80211" ]; then
			IFACE=$(basename "$d")
			break
		fi
	done
fi
echo "iface=${IFACE:-none}" >> "$OUT/meta.txt"
run net/ip-br-link ip -br link
run net/ip-br-addr ip -br addr
run net/ip-route ip route
run net/ip6-route ip -6 route
run net/proc-wireless cat /proc/net/wireless
if [ -n "$IFACE" ]; then
	run net/ip-link-detail ip -d link show "$IFACE"
	if have iw; then
		run net/iw-dev iw dev
		run net/iw-link iw dev "$IFACE" link
		run net/iw-info iw dev "$IFACE" info
		run net/iw-power-save iw dev "$IFACE" get power_save
		run net/iw-station-dump iw dev "$IFACE" station dump
	else
		printf '%s\t%s\t%s\t%s\n' "net/iw" "-" "0" "skip: 沒有 iw" >> "$INDEX"
	fi
fi
if [ -n "$RTT_TARGET" ]; then
	note "RTT：對 --rtt-target 跑 ping（這是本腳本唯一的網路動作）"
	run net/rtt-route ip route get "$RTT_TARGET"
	run_timeout 120 net/rtt-baseline ping -D -c 50 -i 0.2 "$RTT_TARGET"
	# 180 pps、232 B（對應 0x5506 pose 封包大小）的上行負載下量 RTT 分佈。非 root 的最小間隔
	# 依 iputils 版本不同（UNVERIFIED）；失敗時 rc 記在 index.tsv。
	run_timeout 120 net/rtt-180pps ping -D -c 1800 -i 0.0055 -s 232 "$RTT_TARGET"
fi

# ---------------------------------------------------------------------------
# 6. PoC-7：gamescope、Steam／SteamVR 行程
# ---------------------------------------------------------------------------
run_fn gamescope/processes gamescope_report
run_sh gamescope/steam-processes 'ps -eo pid,ppid,etimes,args 2>&1 | grep -i -E "[s]teamvr|[v]rserver|[v]rcompositor|[v]rmonitor|[s]team( |$)|[s]teamwebhelper" | cut -c1-300; true'
if have gamescope; then
	run gamescope/version gamescope --version
fi

# ---------------------------------------------------------------------------
# 7. PoC-F：OpenXR／SteamVR runtime
# ---------------------------------------------------------------------------
run_fn openxr/runtimes openxr_report

# ---------------------------------------------------------------------------
# 8. 樣本（沙箱只看得到 ~/.var/app/<app-id>）
# ---------------------------------------------------------------------------
SAMPLE_DST="$HOME/.var/app/$APP_ID/data/samples"
if [ -n "$SAMPLES" ]; then
	mkdir -p "$SAMPLE_DST"
	run samples/copy cp -a "$SAMPLES/." "$SAMPLE_DST/"
	run_sh samples/list "cd \"$SAMPLE_DST\" && ls -l && sha256sum -- * 2>&1; true"
fi

# ---------------------------------------------------------------------------
# 9. Day 1：VipleStream 探測（沙箱內）
# ---------------------------------------------------------------------------
if [ "$WITH_APP_PROBES" -eq 1 ]; then
	if [ "$APP_INSTALLED" -eq 1 ]; then
		# JSON 寫在沙箱看得到的位置，最後再複製進輸出目錄。
		PROBE_DIR="$HOME/.var/app/$APP_ID/data/poc-$TS"
		mkdir -p "$PROBE_DIR"
		note "Day 1 探測：JSON 暫存 $PROBE_DIR"
		run_timeout "$PROBE_TIMEOUT" probes/v4l2-probe \
			flatpak run "$APP_ID" v4l2-probe --json "$PROBE_DIR/v4l2-probe.json"
		run_timeout "$PROBE_TIMEOUT" probes/v4l2-probe-header-test \
			flatpak run "$APP_ID" v4l2-probe --header-test all --expbuf --json "$PROBE_DIR/v4l2-probe-header-test.json"
		run_timeout "$PROBE_TIMEOUT" probes/xr-probe \
			flatpak run "$APP_ID" xr-probe --json "$PROBE_DIR/xr-probe.json"
		# loader 的除錯輸出在 stderr，一併收進 .txt（PoC-F 失敗時看 runtime 搜尋與 dlopen）。
		run_timeout "$PROBE_TIMEOUT" probes/xr-probe-loader-debug \
			flatpak run "$APP_ID" xr-probe --loader-debug --json "$PROBE_DIR/xr-probe-loader-debug.json"
		if [ -d "$SAMPLE_DST" ]; then
			for s in "$SAMPLE_DST"/*; do
				[ -f "$s" ] || continue
				case "$s" in
					*.h264|*.264|*.hevc|*.h265|*.265|*.ivf) ;;
					*) continue ;;
				esac
				b=$(basename "$s")
				run_timeout "$PROBE_TIMEOUT" "probes/decode-bench-$b-auto" \
					flatpak run "$APP_ID" decode-bench "$s" --json "$PROBE_DIR/decode-bench-$b-auto.json"
				# --fps 90：依串流節拍每 11.1 ms 送一個 AU。連發模式下一個 AU 立刻就到，「要等下一個 AU
				# 才出幀」的 decoder 多出來的一幀延遲看不出來；每幀延遲以這一輪的 lat p99／eagainMax 為準
				# （v4l2-probe header-test 只判斷出幀模式）。樣本多長這一輪就跑多久（N 幀約 N/90 秒）。
				run_timeout "$PROBE_TIMEOUT" "probes/decode-bench-$b-fps90" \
					flatpak run "$APP_ID" decode-bench "$s" --fps 90 --json "$PROBE_DIR/decode-bench-$b-fps90.json"
				run_timeout "$PROBE_TIMEOUT" "probes/decode-bench-$b-sw" \
					flatpak run "$APP_ID" decode-bench "$s" --decoder sw --json "$PROBE_DIR/decode-bench-$b-sw.json"
			done
		fi
		run probes/json-copy cp -a "$PROBE_DIR/." "$OUT/probes/json/"
	else
		printf '%s\t%s\t%s\t%s\n' "probes" "-" "0" "skip: $APP_ID 沒有安裝，無法跑 Day 1 探測" >> "$INDEX"
		note "WARN: --with-app-probes 但 $APP_ID 沒有安裝，略過 Day 1 探測"
	fi
fi

# 探測之後新產生的 log（probe-*.log 與 probe JSON 預設也寫在 log 目錄）
for pair in "flatpak:$FLATPAK_LOG_DIR" "native:$NATIVE_LOG_DIR"; do
	kind=${pair%%:*}
	dir=${pair#*:}
	[ -d "$dir" ] || continue
	run_sh "app-logs/after-$kind-copy" "mkdir -p \"$OUT/app-logs/after-$kind\" && find \"$dir\" -maxdepth 1 -type f -newer \"$MARKER\" -exec cp -a {} \"$OUT/app-logs/after-$kind/\" \\; ; true"
done
rm -f -- "$MARKER"

# ---------------------------------------------------------------------------
# 10. 遮蔽（只作用在輸出目錄內的副本）
# ---------------------------------------------------------------------------
# 主機名稱的全域替換只是後備（uname／journalctl 已經不產生主機名稱）：通用名稱（steamdeck、steamos…）
# 或太短時不做，否則 ps 參數 -steamdeck、os-release 的 VARIANT_ID=steamdeck 這類證據會被換掉。
REDACT_HOST=""
redact_host_init()
{
	local h lc
	h=$(cat /proc/sys/kernel/hostname 2>/dev/null || hostname 2>/dev/null || true)
	case "$h" in
		''|*[!A-Za-z0-9.-]*) return 0 ;;
	esac
	lc=$(printf '%s' "$h" | tr '[:upper:]' '[:lower:]')
	case "$lc" in
		steamdeck|steamos|steamframe|frame|deck|steam|linux|localhost|localhost.localdomain) return 0 ;;
	esac
	[ ${#h} -ge 4 ] || return 0
	REDACT_HOST=$(printf '%s' "$h" | sed 's/[.]/\\./g')
}

redact_file()
{
	local f=$1
	local oct='(25[0-5]|2[0-4][0-9]|1[0-9][0-9]|[1-9]?[0-9])'
	local hex='[0-9A-Fa-f]'
	local -a host_rule=()
	[ -n "$REDACT_HOST" ] && host_rule=(-e "s/\\b$REDACT_HOST\\b/<hostname>/gI")
	# SteamID：steamwebhelper 命令列的 -steamid=<SteamID64> 等；-steamid=0 刻意保留（代表還沒登入）。
	# 序號：Qualcomm 開機鏈常見 androidboot.serialno=（/proc/cmdline）。
	LC_ALL=C sed -E -i \
		${host_rule[@]+"${host_rule[@]}"} \
		-e "s/\\b7656119[0-9]{10}\\b/<steamid64>/g" \
		-e "s/(-steamid=)[1-9][0-9]*/\\1<steamid>/g" \
		-e "s/((androidboot\\.)?serialno=)[^ ]*/\\1<serial>/g" \
		-e "s/\\b($hex{2}[:-]){5}$hex{2}\\b/<mac>/g" \
		-e "s/^([[:space:]]*)(SSID:|ssid)[[:space:]].*/\\1\\2 <ssid>/" \
		-e "s/\\b127(\\.$oct){3}\\b/<ipv4-loopback>/g" \
		-e "s/\\b10(\\.$oct){3}\\b/<ipv4-private>/g" \
		-e "s/\\b192\\.168(\\.$oct){2}\\b/<ipv4-private>/g" \
		-e "s/\\b172\\.(1[6-9]|2[0-9]|3[01])(\\.$oct){2}\\b/<ipv4-private>/g" \
		-e "s/\\b100\\.(6[4-9]|[7-9][0-9]|1[01][0-9]|12[0-7])(\\.$oct){2}\\b/<ipv4-cgnat>/g" \
		-e "s/\\b169\\.254(\\.$oct){2}\\b/<ipv4-linklocal>/g" \
		-e "s/\\b0\\.0\\.0\\.0\\b/<ipv4-any>/g" \
		-e "s/\\b$oct(\\.$oct){3}\\b/<ipv4>/g" \
		-e "s/\\b[Ff][Ee]80(:$hex{0,4}){1,7}/<ipv6-linklocal>/g" \
		-e "s/\\b[Ff][CcDd]$hex{2}(:$hex{0,4}){1,7}/<ipv6-ula>/g" \
		-e "s/\\b$hex{1,4}(:$hex{1,4}){7}\\b/<ipv6>/g" \
		-e "s/\\b$hex{1,4}(:$hex{1,4})*::($hex{1,4}(:$hex{1,4})*)?/<ipv6>/g" \
		"$f"
}

if [ "$REDACT" -eq 1 ]; then
	note "遮蔽 MAC／SSID／IP／SteamID／序號／主機名稱"
	redact_host_init
	# 不能用 grep -I 當篩選：它只看開頭一個 buffer，前段有 NUL（沒電或 kernel 當機後 log 尾端的 NUL 填充、
	# sparse 檔）就判成二進位而跳過，檔案卻照樣打包（fail-open）。GNU sed 在 LC_ALL=C 下可以安全處理含 NUL
	# 的檔案（大小與 NUL 都保留），所以已知的文字類一律遮蔽；其他真正的二進位檔遮不了，搬到
	# <out>.unredacted/（留在本機、不打包，也不刪：可能是證據），清單寫進 REDACTED.txt。
	UNREDACTED_DIR="$OUT.unredacted"
	excluded=()
	while IFS= read -r -d '' f; do
		rel=${f#"$OUT"/}
		[ -s "$f" ] || continue
		case "$rel" in
			*.log|*.txt|*.tsv|*.json|*.conf|*.vrpath|*.vrsettings|*.info|app-logs/*|probes/*)
				redact_file "$f" ;;
			*)
				if grep -Iq . "$f" 2>/dev/null; then
					redact_file "$f"
				elif mkdir -p "$UNREDACTED_DIR/$(dirname "$rel")" && mv -- "$f" "$UNREDACTED_DIR/$rel"; then
					excluded+=("$rel")
				else
					# 搬不走就刪掉：寧可少一份證據，也不要把沒遮蔽的內容打包出去。
					rm -f -- "$f"
					excluded+=("$rel（搬移失敗，已刪除）")
				fi ;;
		esac
	done < <(find "$OUT" -type f -print0)

	# 遮蔽之後才寫：說明文字本身（例如 1.2.3.4）不能被遮蔽規則改掉。
	{
		cat <<'EOF'
這份收集已遮蔽（frame-poc-collect.sh 預設）：
  MAC           → <mac>
  SSID          → <ssid>（iw 輸出的 SSID:／ssid 行）
  IPv4          → <ipv4-loopback> <ipv4-private> <ipv4-cgnat> <ipv4-linklocal> <ipv4-any> <ipv4>
  IPv6          → <ipv6-linklocal> <ipv6-ula> <ipv6>
  SteamID64     → <steamid64>；-steamid=<非 0 的值> → -steamid=<steamid>（-steamid=0 保留：代表還沒登入）
  序號          → serialno=<serial>（含 androidboot.serialno=）
  主機名稱      → 收集時就不產生（uname 不帶 -n、journalctl --no-hostname）
保留類別（私有／CGNAT／link-local）是為了判讀路由（例如 VPN／隧道介面吃掉私網路由）。
遮蔽是正規表示式替換：四段都 <= 255 的版本字串（如 1.2.3.4）也會被當成 IPv4。
這是規則式遮蔽，不保證完整（例如 app log 裡 host PC 的名稱、帳號名稱、/home/<帳號> 路徑都沒有處理）；
節錄貼進公開文件（docs/ 等）之前，要再人工檢查一次。
EOF
		if [ -n "$REDACT_HOST" ]; then
			echo "本機主機名稱不是通用名稱：另外把它全域換成 <hostname>（不分大小寫）。"
		else
			echo "本機主機名稱是通用名稱（steamdeck、steamos…）或太短：沒有做全域替換（避免改到證據）。"
		fi
		if [ ${#excluded[@]} -gt 0 ]; then
			echo
			echo "未遮蔽、沒有打包的二進位檔（搬到 <輸出目錄>.unredacted/，只留在本機）："
			for f in "${excluded[@]}"; do
				echo "  $f"
			done
		fi
	} > "$OUT/REDACTED.txt"
	if [ ${#excluded[@]} -gt 0 ]; then
		note "WARN: ${#excluded[@]} 個二進位檔遮不了，已搬到 $UNREDACTED_DIR（不打包；清單在 REDACTED.txt）"
	fi
fi

# ---------------------------------------------------------------------------
# 11. 打包
# ---------------------------------------------------------------------------
echo "finished=$(date -Iseconds)" >> "$OUT/meta.txt"
failed=$(awk -F'\t' 'NR > 1 && $2 != "0" && $2 != "-" { n++ } END { print n + 0 }' "$INDEX")
note "完成收集：$(($(wc -l < "$INDEX") - 1)) 項，rc 非 0 的 $failed 項（細節見 index.tsv；很多是「沒有這個裝置／權限不足」，本身就是證據）"

PARENT=$(dirname "$OUT")
BASE=$(basename "$OUT")
if tar -C "$PARENT" -czf "$OUT.tar.gz" "$BASE"; then
	( cd "$PARENT" && sha256sum "$BASE.tar.gz" > "$BASE.tar.gz.sha256" )
	note "tarball：$OUT.tar.gz"
	note "sha256：$(awk '{ print $1 }' "$OUT.tar.gz.sha256")"
	note "在 <dev-client> 拉回：scp <frame-user>@<frame>:$OUT.tar.gz <frame-user>@<frame>:$OUT.tar.gz.sha256 ."
	exit 0
fi
note "ERROR: 打包失敗（輸出目錄仍在 $OUT）"
exit 1
