#!/bin/bash
# VipleStream 2.0 §VR M3a X6 — S1：Monado（無頭）上的 XR 驗測（dev-only）
#
#   bash xr-s1-monado.sh [--app-id ID] [--duration SEC] [--stream HOST] [--app NAME]
#                        [--stream-sec SEC] [--stream-args "..."] [--out DIR] [--keep-monado]
#
# 在 Linux 驗測機（S1，docs/vr_architecture.md §7）執行，對已安裝的 VipleStream Flatpak：
#   1. 起一個無頭 Monado（null compositor 90 Hz＋模擬 HMD；已有 monado-service 在跑就沿用）。
#      XRT_COMPOSITOR_NULL／SIMULATED_ENABLE 只用在這支測試腳本，不是產品設定。
#   2. 把主機的 Monado OpenXR client（libopenxr_monado.so＋libcjson.so.1）與一份指向它的 runtime
#      JSON 放到 ~/.var/app/<app-id>/data/xr-s1/（沙箱只看得到那裡）。KDE runtime 6.11 是 glibc 2.42，
#      Monado 25 的 client 最高需要 GLIBC_2.38。
#   3. flatpak run 只在這次加上 --filesystem=xdg-run/monado_comp_ipc 與 LD_LIBRARY_PATH（不改
#      manifest 的 finish-args），跑 xr-probe --session；給了 --stream 再跑一次 xr-desktop 串流
#      （有 --xr-dump-frame 讀回），結束後對 host 送 quit。
#   4. 收集 stdout、xr-probe JSON、Monado log 到 --out，印摘要；最後停掉自己起的 Monado。
#
# 需求：monado-service（apt：monado-service）、已安裝的 VipleStream Flatpak（x86_64 dev 可用
#       build-steamframe.sh --arch x86_64 --flavor dev --bundle 產出後 flatpak install --user --bundle）。
# 不用 set -e：單一步驟失敗照樣收集證據。
set -u

APP_ID="io.github.finaltwinsen.VipleStream"
DURATION=30
STREAM_HOST=""
STREAM_APP="Desktop"
STREAM_SEC=45
STREAM_ARGS="--resolution 1920x1080 --fps 60 --bitrate 10000 --no-quic"
OUT=""
KEEP_MONADO=0
ARCH=$(uname -m)
BRANCH=""

usage()
{
	cat <<'EOF'
VipleStream S1（Monado 無頭）XR 驗測（dev-only）

用法：
  bash xr-s1-monado.sh [選項]

選項：
  --app-id ID         Flatpak app-id（預設 io.github.finaltwinsen.VipleStream）
  --duration SEC      xr-probe --session 的 frame loop 秒數（預設 30）
  --stream HOST       另外對 HOST 跑一次 --display-target xr-desktop 串流（沒給就只跑 xr-probe）
  --app NAME          串流的 app（預設 Desktop）
  --stream-sec SEC    串流秒數（預設 45）
  --stream-args "..." 串流參數（預設 "--resolution 1920x1080 --fps 60 --bitrate 10000 --no-quic"）
  --out DIR           輸出目錄（預設 ~/viplestream-xr-s1-<時間>）
  --keep-monado       結束時不停掉這支腳本起的 monado-service
  --branch BRANCH     Flatpak branch（預設：該 arch 有裝 dev 就用 dev，否則 stable）
  --arch ARCH         flatpak run 的 arch（預設本機 uname -m；兩種 arch 都裝時不寫可能挑到 qemu 的 aarch64）
  -h, --help          顯示這段說明

結束碼：0 xr-probe 成功（與串流結果無關，串流看摘要）；1 參數錯誤；2 前置條件不足；3 xr-probe 失敗。
EOF
}

while [ $# -gt 0 ]; do
	case "$1" in
		--app-id) APP_ID=${2:?}; shift 2 ;;
		--duration) DURATION=${2:?}; shift 2 ;;
		--stream) STREAM_HOST=${2:?}; shift 2 ;;
		--app) STREAM_APP=${2:?}; shift 2 ;;
		--stream-sec) STREAM_SEC=${2:?}; shift 2 ;;
		--stream-args) STREAM_ARGS=${2:?}; shift 2 ;;
		--out) OUT=${2:?}; shift 2 ;;
		--keep-monado) KEEP_MONADO=1; shift ;;
		--arch) ARCH=${2:?}; shift 2 ;;
		--branch) BRANCH=${2:?}; shift 2 ;;
		-h|--help) usage; exit 0 ;;
		*) echo "未知參數：$1（--help 看用法）" 1>&2; exit 1 ;;
	esac
done

[ -n "$OUT" ] || OUT="$HOME/viplestream-xr-s1-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT" || exit 2
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
log() { echo "[xr-s1] $(date +%T) $*" | tee -a "$OUT/xr-s1.log"; }

command -v monado-service >/dev/null || { log "缺 monado-service（sudo apt install monado-service）"; exit 2; }
if [ -z "$BRANCH" ]; then
	if flatpak info --arch="$ARCH" "$APP_ID//dev" >/dev/null 2>&1; then BRANCH=dev; else BRANCH=stable; fi
fi
flatpak info --arch="$ARCH" "$APP_ID//$BRANCH" >/dev/null 2>&1 || { log "沒有安裝 $APP_ID//$BRANCH（$ARCH）"; exit 2; }

MONADO_LIB=""
for d in /usr/lib/x86_64-linux-gnu /usr/lib/aarch64-linux-gnu /usr/lib64 /usr/lib; do
	[ -f "$d/libopenxr_monado.so" ] && { MONADO_LIB="$d/libopenxr_monado.so"; break; }
done
[ -n "$MONADO_LIB" ] || { log "找不到 libopenxr_monado.so"; exit 2; }
CJSON=$(ldd "$MONADO_LIB" | awk '/libcjson/ {print $3}')

# ── 1. Monado ──
STARTED_MONADO=0
MONADO_PGID=""
if pgrep -x monado-service >/dev/null; then
	log "沿用已在執行的 monado-service（pid $(pgrep -x monado-service | head -1)）"
else
	rm -f "$XDG_RUNTIME_DIR/monado.pid"
	# monado-service 會 epoll 監看 stdin，/dev/null 會讓它起不來；給一條不會結束的 pipe
	nohup setsid bash -c "tail -f /dev/null | XRT_COMPOSITOR_NULL=1 XRT_COMPOSITOR_DEFAULT_FRAMERATE=90 SIMULATED_ENABLE=1 exec monado-service" \
		> "$OUT/monado.log" 2>&1 < /dev/null &
	# 非互動腳本的背景工作不是行程群組 leader，setsid 會直接 exec：$! 即新 session 的 leader，
	# 也就是這組 bash／tail／monado-service 的行程群組 id（清理時只停這一組）
	MONADO_PGID=$!
	disown
	for _ in $(seq 1 20); do [ -S "$XDG_RUNTIME_DIR/monado_comp_ipc" ] && pgrep -x monado-service >/dev/null && break; sleep 0.5; done
	if ! pgrep -x monado-service >/dev/null; then
		log "monado-service 起不來，見 $OUT/monado.log"; exit 2
	fi
	STARTED_MONADO=1
	log "monado-service 已啟動（null compositor＋模擬 HMD）"
fi

# ── 2. 沙箱內可見的 runtime ──
STAGE="$HOME/.var/app/$APP_ID/data/xr-s1"
mkdir -p "$STAGE"
cp -f "$MONADO_LIB" "$STAGE/"
[ -n "$CJSON" ] && cp -fL "$CJSON" "$STAGE/libcjson.so.1"
cat > "$STAGE/openxr_monado_s1.json" <<EOF
{ "file_format_version": "1.0.0", "runtime": { "name": "Monado (S1)", "library_path": "$STAGE/libopenxr_monado.so" } }
EOF
FRUN=(flatpak run --arch="$ARCH" --filesystem=xdg-run/monado_comp_ipc --env=LD_LIBRARY_PATH="$STAGE" "$APP_ID//$BRANCH")

# ── 3a. xr-probe --session ──
log "xr-probe --session --duration $DURATION"
timeout $((DURATION + 60)) "${FRUN[@]}" xr-probe --session --duration "$DURATION" \
	--xr-runtime-json "$STAGE/openxr_monado_s1.json" --json "$STAGE/xrprobe.json" > "$OUT/xr-probe.out" 2>&1
PROBE_RC=$?
cp -f "$STAGE/xrprobe.json" "$OUT/" 2>/dev/null
log "xr-probe rc=$PROBE_RC"
grep -E "session|FOCUSED|frames|miss|refresh|vulkan" "$OUT/xr-probe.out" | grep -v "^ext:" | tail -12 | tee -a "$OUT/xr-s1.log"

# ── 3b. 串流（可選）──
if [ -n "$STREAM_HOST" ]; then
	rm -f "$STAGE/dump.png"
	log "stream $STREAM_HOST $STREAM_APP --display-target xr-desktop（$STREAM_SEC s）"
	# shellcheck disable=SC2086
	timeout "$STREAM_SEC" "${FRUN[@]}" stream "$STREAM_HOST" "$STREAM_APP" $STREAM_ARGS \
		--display-target xr-desktop --xr-runtime-json "$STAGE/openxr_monado_s1.json" \
		--xr-dump-frame "$STAGE/dump.png" > "$OUT/stream.out" 2>&1
	log "stream rc=$?（124＝到時結束）"
	timeout 20 "${FRUN[@]}" quit "$STREAM_HOST" > /dev/null 2>&1
	cp -f "$STAGE/dump.png" "$OUT/" 2>/dev/null && log "讀回畫面：$OUT/dump.png"
	# Flatpak 版把 log 寫到沙箱的 cache 目錄（stdout 只有一行「Redirecting log output to …」）
	APPLOG=$(sed -n 's/^Redirecting log output to //p' "$OUT/stream.out" | head -1)
	if [ -n "$APPLOG" ] && [ -f "$APPLOG" ]; then
		cp -f "$APPLOG" "$OUT/stream-app.log"
	else
		cp -f "$OUT/stream.out" "$OUT/stream-app.log"
	fi
	grep -E "\[VIPLE-XR\] (bring-up|xr-desktop|10s|session ended|no Wayland|XrRenderer)|VAAPI: offscreen|VIPLE-NET10" \
		"$OUT/stream-app.log" | tail -14 | tee -a "$OUT/xr-s1.log"
fi

# ── 4. 清理 ──
if [ $STARTED_MONADO -eq 1 ] && [ $KEEP_MONADO -eq 0 ]; then
	kill -- -"$MONADO_PGID" 2>/dev/null
	log "已停止 monado-service"
fi
log "輸出：$OUT"
[ $PROBE_RC -eq 0 ] || exit 3
exit 0
