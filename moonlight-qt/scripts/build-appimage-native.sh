#!/bin/bash
# VipleStream linux-builder: NATIVE (non-WSL) AppImage build.
# Derived from scripts/build-appimage.sh with two builder-specific patches:
#   1. APPIMAGE_EXTRACT_AND_RUN=1 so the linuxdeploy / linuxdeploy-plugin-qt
#      AppImages run on a host with fuse3 (no libfuse2).
#   2. SDL staging: this builder links GENUINE SDL2 (sdl2-classic 2.32.10) via
#      `-lSDL2`, NOT SDL2-compat-over-SDL3. The upstream script's
#      `cp /usr/local/lib/libSDL3.so.0` + `--executable .../libSDL3.so.0`
#      assumes the SDL2-compat-over-SDL3 CI layout and FAILS here
#      (/usr/local/lib/libSDL3.so.0 is absent, /usr/local not writable).
#      Replaced with linuxdeploy `--library <genuine libSDL2-2.0.so.0>`.
# Run from the moonlight-qt source root:  ./scripts/build-appimage-native.sh
set -u

# Patch 1: run bundled-tool AppImages without FUSE.
export APPIMAGE_EXTRACT_AND_RUN=1

# Patch 3: ncnn (Vulkan EP) is mandatory for the Linux libplacebo render path
# (plvk.cpp/vkfruc.cpp include ncnn unconditionally). /usr/local is not writable
# without sudo, so ncnn is installed to a user prefix and passed via NCNN_PREFIX
# (app.pro honors it). LD_LIBRARY_PATH lets linuxdeploy's ldd find+bundle libncnn.so.
# M2a 以前 app.pro 其實沒讀 NCNN_PREFIX，是靠 builder 上 /usr/local 的 symlink 才建得起來；
# 現在 app.pro 真的用它（-I/-L 指到這個 prefix）。
NCNN_PREFIX="${NCNN_PREFIX:-$HOME/.local/ncnn}"
# no trailing ":" when LD_LIBRARY_PATH is unset (an empty element means the current directory)
export LD_LIBRARY_PATH="$NCNN_PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

BUILD_CONFIG="release"

fail()
{
	echo "$1" 1>&2
	exit 1
}

# §SLIM-2 第 0 期：AppImage 大小回歸警告。只報告、永遠 return 0，不讓建置失敗。
# 紀錄放在 $BUILD_ROOT/size-history（每版一個 .size 與 AppDir 檔案清單 .lst），和版號比這次小的最新一筆比，
# 大於 +2 MiB 印 [WARN] 並列出新增或變大的檔案（原始大小；squashfs 無法逐檔取壓縮後大小）。
size_report()
{
	local img="$1" hist="$BUILD_ROOT/size-history" cur_size prev_ver prev_size delta
	mkdir -p "$hist" || return 0
	cur_size=$(stat -c %s "$img" 2>/dev/null) || { echo "[VIPLE-SIZE] cannot stat $img - skipped"; return 0; }
	echo "$cur_size" > "$hist/appimage-${VERSION}.size"
	( cd "$DEPLOY_FOLDER" && find . -type f -printf '%s\t%P\n' ) | LC_ALL=C sort -t "$(printf '\t')" -k2,2 > "$hist/appdir-${VERSION}.lst"
	echo "[VIPLE-SIZE] $(basename "$img"): $((cur_size / 1048576)) MiB; top 20 AppDir files (raw KB):"
	sort -t "$(printf '\t')" -k1,1nr "$hist/appdir-${VERSION}.lst" | head -20 | awk -F '\t' '{ printf "  %9.0f  %s\n", $1 / 1024, $2 }'
	# 版號由小到大排，取排在這次前面的那一筆
	prev_ver=$(ls "$hist" | sed -n 's/^appimage-\(.*\)\.size$/\1/p' | sort -V | awk -v cur="$VERSION" '$0 == cur { print last; exit } { last = $0 }')
	if [ -z "$prev_ver" ]; then
		echo "[VIPLE-SIZE] no older record in $hist - recorded this build only"
		return 0
	fi
	prev_size=$(cat "$hist/appimage-${prev_ver}.size" 2>/dev/null || echo 0)
	delta=$((cur_size - prev_size))
	echo "[VIPLE-SIZE] vs $prev_ver: $((prev_size / 1048576)) -> $((cur_size / 1048576)) MiB ($((delta / 1024)) KB)"
	if [ "$delta" -gt $((2 * 1048576)) ]; then
		echo "[WARN] AppImage is $((delta / 1048576)) MiB larger than $prev_ver (threshold 2 MiB). New or grown files (raw):"
		if [ -f "$hist/appdir-${prev_ver}.lst" ]; then
			LC_ALL=C join -t "$(printf '\t')" -1 2 -2 2 -a 1 -e 0 -o 0,1.1,2.1 \
				"$hist/appdir-${VERSION}.lst" "$hist/appdir-${prev_ver}.lst" \
				| awk -F '\t' '{ g = $2 - $3; if (g >= 262144) printf "%d\t%s%s\n", g, $1, ($3 == 0 ? "  (new)" : "") }' \
				| sort -t "$(printf '\t')" -k1,1nr | head -15 \
				| awk -F '\t' '{ printf "  +%8.0f KB  %s\n", $1 / 1024, $2 }'
		fi
	fi
	return 0
}

BUILD_ROOT=$PWD/build
SOURCE_ROOT=$PWD
BUILD_FOLDER=$BUILD_ROOT/build-$BUILD_CONFIG
DEPLOY_FOLDER=$BUILD_ROOT/deploy-$BUILD_CONFIG
INSTALLER_FOLDER=$BUILD_ROOT/installer-$BUILD_CONFIG

if [ -n "${CI_VERSION:-}" ]; then
  VERSION=$CI_VERSION
else
  # §REVIEW-1119：讀不到版號時不要默默用空字串；CRLF 的 \r 會進 AppImage 檔名。
  VERSION=$(tr -d '\r\n' < "$SOURCE_ROOT/app/version.txt") || fail "Unable to read version.txt"
  [ -n "$VERSION" ] || fail "Unable to read version.txt"
fi

# Patch 2: locate the genuine SDL2 runtime library actually linked via -lSDL2.
SDL2_LIB=$(pkg-config --variable=libdir sdl2 2>/dev/null)/libSDL2-2.0.so.0
[ -e "$SDL2_LIB" ] || SDL2_LIB=/usr/lib/x86_64-linux-gnu/libSDL2-2.0.so.0
[ -e "$SDL2_LIB" ] || fail "Unable to find genuine libSDL2-2.0.so.0 to bundle!"

command -v qmake6 >/dev/null 2>&1 || fail "Unable to find 'qmake6' in your PATH!"
command -v linuxdeploy >/dev/null 2>&1 || fail "Unable to find 'linuxdeploy' in your PATH!"

echo Cleaning output directories
rm -rf $BUILD_FOLDER
rm -rf $DEPLOY_FOLDER
rm -rf $INSTALLER_FOLDER
mkdir -p $BUILD_ROOT
mkdir -p $BUILD_FOLDER
mkdir -p $DEPLOY_FOLDER
mkdir -p $INSTALLER_FOLDER

echo Configuring the project
pushd $BUILD_FOLDER
qmake6 $SOURCE_ROOT/moonlight-qt.pro CONFIG+=disable-wayland CONFIG+=disable-libdrm PREFIX=$DEPLOY_FOLDER/usr NCNN_PREFIX=$NCNN_PREFIX DEFINES+=APP_IMAGE DEFINES+=VIPLE_MPQUIC || fail "Qmake failed!"
popd

echo Compiling Moonlight in $BUILD_CONFIG configuration
pushd $BUILD_FOLDER
# qmake6 noble/resolute moc-race workaround (see upstream build-appimage.sh).
echo "  Recursing qmake into subdirs so app/Makefile.Release exists"
make -j$(nproc) qmake_all || fail "qmake recursion failed!"
echo "  Pre-generating moc sources to work around qmake6 noble path bug"
make -C app -f Makefile.Release -j$(nproc) compiler_moc_source_make_all \
    || fail "moc pre-generation failed!"
make -j$(nproc) $(echo "$BUILD_CONFIG" | tr '[:upper:]' '[:lower:]') || fail "Make failed!"
popd

echo Deploying to staging directory
pushd $BUILD_FOLDER
make install || fail "Make install failed!"
popd

# §SLIM-2 L1：主程式剝除錯資訊（124.9 MB → 約 11.6 MB；AppImage 少約 41 MB）。linuxdeploy 不會剝 make install 裝進來的主程式。
# 採修改稿的退路：不用 objcopy --only-keep-debug／--add-gnu-debuglink（和 linuxdeploy 之後的 patchelf 互動沒實測過），
# 剝之前把「未剝的原檔」存成 $BUILD_ROOT/debug/VipleStream-Client-<版號>-linux-x64.unstripped（不進 AppImage），
# 只做 strip --strip-debug（保留 .symtab）。要解 core 時 gdb 用 symbol-file 指向原檔。保留最近 5 版。
echo Stripping debug info from the main binary
APP_BIN="$DEPLOY_FOLDER/usr/bin/viplestream"
[ -f "$APP_BIN" ] || fail "make install did not produce $APP_BIN"
DEBUG_DIR="$BUILD_ROOT/debug"
mkdir -p "$DEBUG_DIR" || fail "Unable to create $DEBUG_DIR"
DEBUG_FILE="$DEBUG_DIR/VipleStream-Client-${VERSION}-linux-x64.unstripped"
BIN_SIZE_BEFORE=$(stat -c %s "$APP_BIN")
cp -f "$APP_BIN" "$DEBUG_FILE" || fail "Saving the unstripped binary to $DEBUG_FILE failed!"
strip --strip-debug "$APP_BIN" || fail "strip --strip-debug failed!"
echo "  $APP_BIN: $((BIN_SIZE_BEFORE / 1048576)) MiB -> $(( $(stat -c %s "$APP_BIN") / 1048576 )) MiB; unstripped copy in $DEBUG_FILE"
ls -1t "$DEBUG_DIR"/*.unstripped 2>/dev/null | tail -n +6 | xargs -r rm -f

# §SLIM-2 L2：flownet.bin 不內附（app.pro 只安裝 flownet.param，執行期由 ModelFetcher 下載並驗 sha256）。
# 這裡只是防呆：app.pro 改回去或別的步驟又帶進來時，不要默默多 10 MB。
RIFE_DIR="$DEPLOY_FOLDER/usr/share/VipleStream/rife-v4.25-lite"
[ -f "$RIFE_DIR/flownet.param" ] || fail "flownet.param missing in $RIFE_DIR - RIFE FRUC would be disabled"
if [ -e "$RIFE_DIR/flownet.bin" ]; then
    echo "[WARN] flownet.bin was installed into the AppDir - removing (it is lazy-fetched at runtime)"
    rm -f "$RIFE_DIR/flownet.bin"
fi

# Patch 4: metainfo 檔名與 <id> 已對齊（viplestream.appdata.xml / <id>viplestream</id>，
# §K.4-DESKTOP-ID），但 <id> 非 reverse-DNS 仍會被 appstreamcli validate 警告，
# appimagetool 會因 warning 以非零退出 -> 打包失敗。metainfo 對 AppImage 執行無影響，
# 移除以通過打包。
rm -f "$DEPLOY_FOLDER"/usr/share/metainfo/*.xml
rmdir "$DEPLOY_FOLDER/usr/share/metainfo" 2>/dev/null || true

echo Staging CJK font
mkdir -p $DEPLOY_FOLDER/usr/share/fonts/opentype
CJK_FONT_SRC=""
for candidate in \
    /usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc \
    /usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc; do
    if [ -f "$candidate" ]; then
        CJK_FONT_SRC="$candidate"
        break
    fi
done
if [ -n "$CJK_FONT_SRC" ]; then
    cp "$CJK_FONT_SRC" $DEPLOY_FOLDER/usr/share/fonts/opentype/NotoSansCJK-Regular.ttc
    echo "  bundled $CJK_FONT_SRC"
else
    echo "  fonts-noto-cjk not installed on builder — AppImage will fall back to host fontconfig"
fi

echo Creating AppImage
pushd $INSTALLER_FOLDER
export QML_SOURCES_PATHS="$SOURCE_ROOT/app/gui"
export QMAKE=qmake6
echo "  Bundling genuine SDL2: $SDL2_LIB"
# Patch 5（M2a R2）：經 SSH 執行（沒有 Wayland／X11）時，--help／--version 由 main.cpp
# 早期掃描改用 offscreen QPA，不再落到 EGLFS。linuxdeploy-plugin-qt 預設只部署
# libqxcb.so，要明確多帶 offscreen 平台外掛，否則 AppImage 裡找不到它而 abort。
EXTRA_PLATFORM_PLUGINS=libqoffscreen.so \
linuxdeploy --appdir $DEPLOY_FOLDER \
    --desktop-file $DEPLOY_FOLDER/usr/share/applications/viplestream.desktop \
    --icon-file $SOURCE_ROOT/app/res/moonlight.svg \
    --icon-filename viplestream \
    --library "$SDL2_LIB" \
    --plugin qt \
    || fail "linuxdeploy failed!"
# Patch 6（2026-11-03，CachyOS Qt 6.12 實測）：AppImage 從 AppRun（→ usr/bin/viplestream 的
# symlink）啟動時，Qt 6.12 以 argv[0] 的目錄（AppImage 根目錄）當 applicationDirPath，
# 讀不到 linuxdeploy-plugin-qt 放在 usr/bin/qt.conf 的路徑，改到根目錄的 platforms/ 找外掛
# → 「no Qt platform plugin could be initialized」abort。根目錄也放一份指向 usr/ 的 qt.conf。
cat > $DEPLOY_FOLDER/qt.conf <<'QTCONF'
[Paths]
Prefix = usr
Plugins = plugins
Imports = qml
Qml2Imports = qml
QTCONF
# Patch 7（2026-11-11）：AppImage 規範要 AppDir 根目錄有 PNG 的 .DirIcon（檔案管理員顯示用）；
# 只有 SVG 時 AppImage 目錄（appimage.github.io）的檢查會以「.DirIcon is missing」判失敗。
# 從 SVG 轉一張 256×256 放根目錄。沒有 rsvg-convert 就明講並中止，不要默默出一個沒圖示的包。
command -v rsvg-convert >/dev/null 2>&1 || fail "Unable to find 'rsvg-convert' (librsvg2-bin) - needed for the AppImage .DirIcon"
rsvg-convert -w 256 -h 256 "$SOURCE_ROOT/app/res/moonlight.svg" -o "$DEPLOY_FOLDER/viplestream.png" || fail "rsvg-convert failed!"
cp -f "$DEPLOY_FOLDER/viplestream.png" "$DEPLOY_FOLDER/.DirIcon" || fail "writing .DirIcon failed!"
linuxdeploy --appdir $DEPLOY_FOLDER \
    --desktop-file $DEPLOY_FOLDER/usr/share/applications/viplestream.desktop \
    --icon-file $SOURCE_ROOT/app/res/moonlight.svg \
    --icon-filename viplestream \
    --output appimage \
    || fail "linuxdeploy (appimage) failed!"
APPIMAGE_RAW=$(ls VipleStream-*.AppImage 2>/dev/null | head -1)
if [ -n "$APPIMAGE_RAW" ]; then
    mv "$APPIMAGE_RAW" "VipleStream-Client-${VERSION}-linux-x64.AppImage"
else
    fail "linuxdeploy did not produce a VipleStream-*.AppImage"
fi
popd

size_report "$INSTALLER_FOLDER/VipleStream-Client-${VERSION}-linux-x64.AppImage"

echo "Build successful:$INSTALLER_FOLDER/VipleStream-Client-${VERSION}-linux-x64.AppImage"
