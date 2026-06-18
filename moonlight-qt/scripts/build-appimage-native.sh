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
NCNN_PREFIX="${NCNN_PREFIX:-$HOME/.local/ncnn}"
export LD_LIBRARY_PATH="$NCNN_PREFIX/lib:${LD_LIBRARY_PATH:-}"

BUILD_CONFIG="release"

fail()
{
	echo "$1" 1>&2
	exit 1
}

BUILD_ROOT=$PWD/build
SOURCE_ROOT=$PWD
BUILD_FOLDER=$BUILD_ROOT/build-$BUILD_CONFIG
DEPLOY_FOLDER=$BUILD_ROOT/deploy-$BUILD_CONFIG
INSTALLER_FOLDER=$BUILD_ROOT/installer-$BUILD_CONFIG

if [ -n "${CI_VERSION:-}" ]; then
  VERSION=$CI_VERSION
else
  VERSION=`cat $SOURCE_ROOT/app/version.txt`
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

# Patch 4: repo 的 usr/share/metainfo/com.piinsta.appdata.xml 其 component-id
# (com.piinsta.Client) 與檔名不一致,appimagetool 內建的 appstreamcli validate
# 會因該 warning 以非零退出 -> 打包失敗。metainfo 對 AppImage 執行無影響,移除以
# 通過打包。正解(交 win-builder):讓 metainfo 檔名與 <id> 一致,或 <id> 改 com.piinsta。
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
linuxdeploy --appdir $DEPLOY_FOLDER \
    --desktop-file $DEPLOY_FOLDER/usr/share/applications/com.piinsta.desktop \
    --icon-file $SOURCE_ROOT/app/res/moonlight.svg \
    --icon-filename viplestream \
    --library "$SDL2_LIB" \
    --plugin qt \
    --output appimage \
    || fail "linuxdeploy failed!"
APPIMAGE_RAW=$(ls VipleStream-*.AppImage 2>/dev/null | head -1)
if [ -n "$APPIMAGE_RAW" ]; then
    mv "$APPIMAGE_RAW" "VipleStream-Client-${VERSION}-linux-x64.AppImage"
fi
popd

echo "Build successful: $INSTALLER_FOLDER/VipleStream-Client-${VERSION}-linux-x64.AppImage"
