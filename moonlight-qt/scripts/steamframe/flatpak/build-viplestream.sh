#!/bin/bash
# VipleStream §SF-FLATPAK（M2a α）— viplestream 模組在 flatpak-builder 沙箱內的建置步驟
#
# 由 manifest（io.github.finaltwinsen.VipleStream.yml）的 viplestream 模組呼叫：
#   build-commands: [bash moonlight-qt/scripts/steamframe/flatpak/build-viplestream.sh]
# 工作目錄＝模組根目錄（buildsystem: simple 不做 builddir），內容是：
#   moonlight-qt/                      VipleStream client 原始碼
#   Sunshine/third-party/picoquic/     picoquic（dev＝快照；release＝pin 在 gitlink 的 git 來源）
#   picotls-src/                       picotls（manifest 的 git 來源，pin 在 VIPLE_PICOTLS_PIN）
#   flavor.txt、SOURCE_INFO            build-steamframe.sh 產生
#
# 不要在沙箱外手動跑這支（它假設 /app 前綴與 flatpak-builder 的環境變數）。
# 建置規範（CLAUDE.md）：一律透過 build-steamframe.sh → flatpak-builder 呼叫。
#
# 步驟：
#   1. picoquic＋picotls：離線 FetchContent（FETCHCONTENT_SOURCE_DIR_PICOTLS＋FULLY_DISCONNECTED），
#      只建 client 需要的靜態庫；picotls-fusion 只在 target 存在時建（x86_64 才有）。
#   2. qmake shadow build（PREFIX=/app，CONFIG+=openxr，VIPLE_MPQUIC，desktop id＝app-id）。
#   3. make qmake_all → moc 預先產生（qmake6 路徑 bug 的繞法，同 build-appimage-native.sh）
#      → make release → make install。
#   4. 授權檔、SOURCE_INFO、安裝後自檢。
set -u
set -o pipefail

log()
{
	printf '[build-viplestream] %(%H:%M:%S)T %s\n' -1 "$*"
}

die()
{
	printf '[build-viplestream] ERROR: %s\n' "$*" 1>&2
	exit 1
}

# 各步驟耗時（G-BUILD 要分得出 picoquic、qmake、編譯＋連結、安裝各佔多少）
STEP_NAME=""
STEP_START=0
step_begin()
{
	STEP_NAME=$1
	STEP_START=$SECONDS
	log "── $STEP_NAME"
}

step_end()
{
	log "── $STEP_NAME 完成（$((SECONDS - STEP_START)) 秒）"
	STEP_TIMES+=("$STEP_NAME=$((SECONDS - STEP_START))")
}
STEP_TIMES=()

# ---------------------------------------------------------------------------
# 0. 環境檢查（一律絕對路徑：shadow build 會 cd 進子目錄）
# ---------------------------------------------------------------------------
ROOT=$PWD
[ -f "$ROOT/moonlight-qt/moonlight-qt.pro" ] || die "找不到 $ROOT/moonlight-qt/moonlight-qt.pro；工作目錄必須是 viplestream 模組根目錄"

APP_ID=${FLATPAK_ID:-}
# FLATPAK_ID 空的話 qmake 會靜默退回 desktop id "viplestream"，Wayland app_id 就對不上 app-id。
[ -n "$APP_ID" ] || die "FLATPAK_ID 是空的（應由 flatpak build 設定）；不在 flatpak-builder 沙箱內？"

JOBS=${FLATPAK_BUILDER_N_JOBS:-}
if [ -z "$JOBS" ]; then
	JOBS=$(nproc 2>/dev/null || echo 2)
fi

FLAVOR=""
if [ -f "$ROOT/flavor.txt" ]; then
	FLAVOR=$(tr -d '[:space:]' < "$ROOT/flavor.txt")
fi
case "$FLAVOR" in
	dev|release) ;;
	*) die "flavor.txt 缺少或內容不對（'$FLAVOR'）；應由 build-steamframe.sh 產生 dev 或 release" ;;
esac

PQ_SRC="$ROOT/Sunshine/third-party/picoquic"
PQ_BUILD="$ROOT/_pq"
PTLS_SRC="$ROOT/picotls-src"
PTLS_LIBDIR="$PQ_BUILD/_deps/picotls-build"
QT_BUILD="$ROOT/_build"

[ -f "$PQ_SRC/CMakeLists.txt" ] || die "找不到 picoquic 原始碼：$PQ_SRC"
[ -f "$PTLS_SRC/CMakeLists.txt" ] || die "找不到 picotls 原始碼：$PTLS_SRC（manifest 的 picotls git 來源）"

log "app-id=$APP_ID flavor=$FLAVOR jobs=$JOBS arch=$(uname -m) root=$ROOT"

# ---------------------------------------------------------------------------
# 0b. picotls pin 比對（FETCHCONTENT_SOURCE_DIR 會忽略 GIT_TAG，不比對會靜默漂移）
#     manifest 的 pin（VIPLE_PICOTLS_PIN）必須等於 picoquic CMakeLists.txt 裡
#     非 AEGIS 分支的 PICOQUIC_FETCH_PTLS_DEFAULT_TAG。
# ---------------------------------------------------------------------------
PIN=${VIPLE_PICOTLS_PIN:-}
[ -n "$PIN" ] || die "VIPLE_PICOTLS_PIN 是空的（manifest 的 viplestream 模組 build-options.env）"

# 解析 `if(WITH_AEGIS) … else() set(PICOQUIC_FETCH_PTLS_DEFAULT_TAG "<sha>") … endif()`
# 的 else 分支（第一個 if(WITH_AEGIS) 區塊就是設定預設 tag 的那一段）。
PQ_DEFAULT_TAG=$(awk '
	/^[[:space:]]*if[[:space:]]*\([[:space:]]*WITH_AEGIS[[:space:]]*\)/ { inblk = 1; branch = "aegis"; next }
	inblk && /^[[:space:]]*else[[:space:]]*\([[:space:]]*\)/ { branch = "plain"; next }
	inblk && /^[[:space:]]*endif[[:space:]]*\(/ { inblk = 0; next }
	inblk && branch == "plain" && /PICOQUIC_FETCH_PTLS_DEFAULT_TAG/ {
		if (match($0, /"[0-9a-fA-F]+"/)) { print substr($0, RSTART + 1, RLENGTH - 2); exit }
	}' "$PQ_SRC/CMakeLists.txt")
[ -n "$PQ_DEFAULT_TAG" ] || die "無法從 $PQ_SRC/CMakeLists.txt 解析非 AEGIS 的 PICOQUIC_FETCH_PTLS_DEFAULT_TAG（格式改了？）"
if [ "$PQ_DEFAULT_TAG" != "$PIN" ]; then
	die "picotls pin 不一致：manifest=$PIN，picoquic 預設=$PQ_DEFAULT_TAG。picoquic 更新了 picotls 版本時，manifest 的 &picotls-pin 要一起改"
fi

# 實際取回的 picotls commit（flatpak-builder 的 git 來源會保留 .git；沒有時只能相信 manifest）
if command -v git >/dev/null 2>&1 && [ -e "$PTLS_SRC/.git" ]; then
	PTLS_HEAD=$(git -C "$PTLS_SRC" rev-parse HEAD 2>/dev/null || true)
	if [ -n "$PTLS_HEAD" ] && [ "$PTLS_HEAD" != "$PIN" ]; then
		die "picotls-src 的 HEAD=$PTLS_HEAD 和 pin=$PIN 不同"
	fi
	log "picotls pin 核對通過：$PIN（picoquic 預設相同${PTLS_HEAD:+，HEAD 相同}）"
else
	PTLS_HEAD=""
	log "WARN: picotls-src 沒有 .git（或沙箱沒有 git），只核對了 manifest pin 與 picoquic 預設"
fi

# ---------------------------------------------------------------------------
# 1. picoquic＋picotls（靜態庫，離線）
# ---------------------------------------------------------------------------
step_begin "picoquic/picotls"
rm -rf "$PQ_BUILD"
cmake -S "$PQ_SRC" -B "$PQ_BUILD" -G Ninja \
	-DCMAKE_BUILD_TYPE=Release \
	-DPICOQUIC_FETCH_PTLS=ON \
	-DFETCHCONTENT_SOURCE_DIR_PICOTLS="$PTLS_SRC" \
	-DFETCHCONTENT_FULLY_DISCONNECTED=ON \
	-DBUILD_DEMO=OFF \
	-DBUILD_LOGREADER=OFF \
	-DBUILD_TESTING=OFF \
	|| die "picoquic cmake configure 失敗"

PQ_TARGETS=(picoquic-core picotls-core picotls-openssl picotls-minicrypto)
# fusion 需要 AVX2/AES-NI，只有 x86_64 會產生這個 target；aarch64 上 picoquic 會自動定義
# PTLS_WITHOUT_FUSION，app.pro 也只在 libpicotls-fusion.a 存在時才連結。
# （先存成變數再 grep：pipefail 下 `ninja … | grep -q` 可能因 SIGPIPE 誤判成沒找到。）
PQ_ALL_TARGETS=$(ninja -C "$PQ_BUILD" -t targets all 2>/dev/null || true)
if grep -q '^picotls-fusion:' <<< "$PQ_ALL_TARGETS"; then
	PQ_TARGETS+=(picotls-fusion)
fi
log "picoquic targets: ${PQ_TARGETS[*]}"
ninja -C "$PQ_BUILD" -j "$JOBS" "${PQ_TARGETS[@]}" || die "picoquic/picotls 建置失敗"

for lib in "$PQ_BUILD/libpicoquic-core.a" \
           "$PTLS_LIBDIR/libpicotls-core.a" \
           "$PTLS_LIBDIR/libpicotls-openssl.a" \
           "$PTLS_LIBDIR/libpicotls-minicrypto.a"; do
	[ -f "$lib" ] || die "建完卻找不到 $lib（picoquic/picotls 的輸出位置改了？app.pro 的 PICOQUIC_BUILD/PICOTLS_LIBDIR 要跟著改）"
done
step_end

# ---------------------------------------------------------------------------
# 2. qmake（shadow build）
# ---------------------------------------------------------------------------
step_begin "qmake"
QMAKE_BIN=$(command -v qmake6 2>/dev/null || command -v qmake 2>/dev/null || true)
[ -n "$QMAKE_BIN" ] || die "找不到 qmake6/qmake（KDE SDK 應內建）"

rm -rf "$QT_BUILD"
mkdir -p "$QT_BUILD" || die "無法建立 $QT_BUILD"
cd "$QT_BUILD" || die "無法進入 $QT_BUILD"

# 刻意不帶：DEFINES+=APP_IMAGE（Flatpak 不是 AppImage，updater 會誤判）、
# CONFIG+=disable-libdrm（兩種 arch 都要 DRM_PRIME，見 finish-args.md）、
# CONFIG+=disable-wayland（Frame 在 gamescope 的 Wayland 下跑）。
QMAKE_ARGS=(
	"$ROOT/moonlight-qt/moonlight-qt.pro"
	"PREFIX=/app"
	"QMAKE_RPATHDIR=/app/lib"
	"NCNN_PREFIX=/app"
	"PICOQUIC_BUILD=$PQ_BUILD"
	"PICOTLS_LIBDIR=$PTLS_LIBDIR"
	"PICOQUIC_DIR=$PQ_SRC/picoquic"
	"VIPLE_DESKTOP_ID=$APP_ID"
	"CONFIG+=openxr"
	"DEFINES+=VIPLE_MPQUIC"
)
# buildsystem: simple 不會自動注入 SDK 的旗標（qmake buildsystem 才會），這裡補上，
# 讓 hardening 旗標（-fstack-protector-strong 等）與 -L/app/lib 生效。
[ -n "${CFLAGS:-}" ] && QMAKE_ARGS+=("QMAKE_CFLAGS+=$CFLAGS")
[ -n "${CXXFLAGS:-}" ] && QMAKE_ARGS+=("QMAKE_CXXFLAGS+=$CXXFLAGS")
[ -n "${LDFLAGS:-}" ] && QMAKE_ARGS+=("QMAKE_LFLAGS+=$LDFLAGS")
# dev 關掉 app 的 -flto（LTO link 不進 ccache，qemu 下會把增量建置放大很多）；
# release 保留，G-BUILD 以 release 設定判定。
if [ "$FLAVOR" = "dev" ]; then
	QMAKE_ARGS+=("CONFIG+=disable-lto")
fi

log "qmake: $QMAKE_BIN ${QMAKE_ARGS[*]}"
"$QMAKE_BIN" "${QMAKE_ARGS[@]}" || die "qmake 失敗"
step_end

# ---------------------------------------------------------------------------
# 3. 編譯、安裝
# ---------------------------------------------------------------------------
step_begin "make qmake_all + moc"
make -j"$JOBS" qmake_all || die "qmake 遞迴（make qmake_all）失敗"
# qmake6 的 moc 路徑 bug 繞法（同 build-appimage-native.sh）：先把 moc 原始檔全部產生好。
make -C app -f Makefile.Release -j"$JOBS" compiler_moc_source_make_all || die "moc 預先產生失敗"
step_end

step_begin "make release（編譯＋連結）"
make -j"$JOBS" release || die "make release 失敗"
step_end

# 連結時間估計：最後一個 .o 到執行檔的時間差（LTO 時就是 LTO link；G-BUILD 參考用）。
APP_BIN_BUILT=$(find "$QT_BUILD/app" -maxdepth 2 -type f -name viplestream -perm -u+x 2>/dev/null | head -n 1)
if [ -n "$APP_BIN_BUILT" ]; then
	LAST_OBJ=$(find "$QT_BUILD/app" -type f -name '*.o' -printf '%T@\n' 2>/dev/null | sort -n | tail -n 1)
	BIN_T=$(stat -c '%Y' "$APP_BIN_BUILT" 2>/dev/null || echo "")
	if [ -n "$LAST_OBJ" ] && [ -n "$BIN_T" ]; then
		log "連結估計：最後一個 .o → 執行檔 約 $((BIN_T - ${LAST_OBJ%.*})) 秒（flavor=$FLAVOR）"
	fi
fi

step_begin "make install"
make install || die "make install 失敗"
step_end

cd "$ROOT" || die "無法回到 $ROOT"

# ---------------------------------------------------------------------------
# 4. 授權、SOURCE_INFO
#    依賴模組與 repo 根目錄 LICENSE 由 flatpak-builder 自動收（/app/share/licenses/<app-id>/<module>/）；
#    這裡補 moonlight-qt 與兩個靜態連結的函式庫。目錄名刻意和模組名不同，避免和自動收的撞名。
# ---------------------------------------------------------------------------
LIC_ROOT="/app/share/licenses/$APP_ID"
install -Dm644 "$ROOT/moonlight-qt/LICENSE" "$LIC_ROOT/moonlight-qt/LICENSE" || die "安裝 moonlight-qt 授權失敗"
install -Dm644 "$PQ_SRC/LICENSE" "$LIC_ROOT/picoquic/LICENSE" || die "安裝 picoquic 授權失敗"
# picotls 沒有獨立的 LICENSE 檔：授權寫在 README 與每個原始檔開頭（MIT）。
install -Dm644 "$PTLS_SRC/README.md" "$LIC_ROOT/picotls/README.md" || die "安裝 picotls README 失敗"
sed -n '1,/\*\//p' "$PTLS_SRC/include/picotls.h" > "$ROOT/picotls-LICENSE.txt" \
	&& install -Dm644 "$ROOT/picotls-LICENSE.txt" "$LIC_ROOT/picotls/LICENSE" \
	|| die "擷取 picotls 授權失敗"
# minicrypto 內含 cifra 與 micro-ecc（各自的授權檔若存在就一起帶）。
for dep in cifra micro-ecc; do
	for f in "$PTLS_SRC/deps/$dep"/LICENSE* "$PTLS_SRC/deps/$dep"/COPYING* "$PTLS_SRC/deps/$dep"/UNLICENSE*; do
		[ -f "$f" ] || continue
		install -Dm644 "$f" "$LIC_ROOT/picotls-$dep/$(basename "$f")" || die "安裝 $f 失敗"
	done
done

INFO_OUT="/app/share/VipleStream/SOURCE_INFO"
mkdir -p "$(dirname "$INFO_OUT")" || die "無法建立 $(dirname "$INFO_OUT")"
{
	if [ -f "$ROOT/SOURCE_INFO" ]; then
		cat "$ROOT/SOURCE_INFO"
	else
		echo "source_info=missing"
	fi
	echo "build_flavor=$FLAVOR"
	echo "build_arch=$(uname -m)"
	echo "build_app_id=$APP_ID"
	echo "picotls_pin=$PIN"
	echo "picotls_head=${PTLS_HEAD:-unknown}"
	echo "picoquic_fusion=$([ -f "$PTLS_LIBDIR/libpicotls-fusion.a" ] && echo 1 || echo 0)"
	echo "build_time_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "$INFO_OUT" || die "寫入 $INFO_OUT 失敗"
chmod 644 "$INFO_OUT"

# ---------------------------------------------------------------------------
# 5. 安裝後自檢
# ---------------------------------------------------------------------------
[ -x /app/bin/viplestream ] || die "/app/bin/viplestream 不存在或不可執行（make install 的 PREFIX 不對？）"
[ -f /app/share/applications/viplestream.desktop ] || die "找不到 viplestream.desktop（rename-desktop-file 會失敗）"
[ -f /app/share/metainfo/viplestream.appdata.xml ] || die "找不到 viplestream.appdata.xml（rename-appdata-file 會失敗）"

if command -v ldd >/dev/null 2>&1; then
	LDD_OUT=$(ldd /app/bin/viplestream 2>&1)
	if grep -q 'not found' <<< "$LDD_OUT"; then
		# 不直接失敗：沙箱內 ldd 的搜尋路徑和執行期可能不同；smoke test 才是最後判定。
		log "WARN: ldd 有找不到的函式庫（smoke test 會確認實際能不能啟動）："
		grep 'not found' <<< "$LDD_OUT"
	fi
	if ! grep -q 'libopenxr_loader' <<< "$LDD_OUT"; then
		log "WARN: viplestream 沒有連到 libopenxr_loader（CONFIG+=openxr 沒生效？xr-probe 會回 10）"
	fi
	if ! grep -q 'libncnn' <<< "$LDD_OUT"; then
		log "WARN: viplestream 沒有連到 libncnn（NCNN_PREFIX=/app 沒生效？）"
	fi
fi

log "各步驟耗時（秒）：${STEP_TIMES[*]}"
log "完成：/app/bin/viplestream（flavor=$FLAVOR）"
exit 0
