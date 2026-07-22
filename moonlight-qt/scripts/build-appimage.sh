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

if [ -n "$CI_VERSION" ]; then
  VERSION=$CI_VERSION
else
  VERSION=`cat $SOURCE_ROOT/app/version.txt`
fi

command -v qmake6 >/dev/null 2>&1 || fail "Unable to find 'qmake6' in your PATH!"
# VipleStream §K.1: switched from upstream linuxdeployqt to linuxdeploy +
# qt plugin (see line ~91 invocation).  Sanity-check the new binary
# instead of the old one to actually catch a missing dep early.
command -v linuxdeploy >/dev/null 2>&1 || fail "Unable to find 'linuxdeploy' in your PATH!"

echo Cleaning output directories
rm -rf $BUILD_FOLDER
rm -rf $DEPLOY_FOLDER
rm -rf $INSTALLER_FOLDER
mkdir $BUILD_ROOT
mkdir $BUILD_FOLDER
mkdir $DEPLOY_FOLDER
mkdir $INSTALLER_FOLDER

echo Configuring the project
pushd $BUILD_FOLDER
# Building with Wayland support will cause linuxdeployqt to include libwayland-client.so in the AppImage.
# Since we always use the host implementation of EGL, this can cause libEGL_mesa.so to fail to load due
# to missing symbols from the host's version of libwayland-client.so that aren't present in the older
# version of libwayland-client.so from our AppImage build environment. When this happens, EGL fails to
# work even in X11. To avoid this, we will disable Wayland support for the AppImage.
#
# We disable DRM support because linuxdeployqt doesn't bundle the appropriate libraries for Qt EGLFS.
qmake6 $SOURCE_ROOT/moonlight-qt.pro CONFIG+=disable-wayland CONFIG+=disable-libdrm PREFIX=$DEPLOY_FOLDER/usr DEFINES+=APP_IMAGE DEFINES+=VIPLE_MPQUIC || fail "Qmake failed!"
popd

echo Compiling Moonlight in $BUILD_CONFIG configuration
pushd $BUILD_FOLDER
# VipleStream §K.1 / §v1.4.0: qmake6 on Ubuntu noble emits a broken absolute
# path in the .o rule's prereq list for source files that #include their
# own .moc (boxartmanager.cpp / computermanager.cpp / computermodel.cpp).
# The path points to <SOURCE_ROOT>/app/release/<name>.moc which never
# exists, so make never triggers the moc rule, and the parallel .cpp
# compile races ahead and fails with "<name>.moc: No such file or directory".
#
# Workaround: explicitly run compiler_moc_source_make_all (defined in
# Makefile.Release / Makefile.Debug) before the main compile so all .moc
# files are generated up-front.  Then the .cpp compile finds them via
# the -Irelease path that's already in CXXFLAGS.
#
# Three preconditions for the workaround to actually do anything on a
# fresh build dir:
#   1. Top-level qmake6 has already produced $BUILD_FOLDER/Makefile (above).
#   2. `make qmake_all` recurses qmake into each SUBDIR (app, AntiHooking,
#      moonlight-common-c, qmdnsengine, h264bitstream, 3rdparty/nvvideoparser)
#      so that $BUILD_FOLDER/app/Makefile.Release exists.  Skipping this
#      step on a clean build dir → "make: app/Makefile.Release: No such
#      file or directory" (v1.4.0 release run hit this).
#   3. The make invocation must put `-C app` BEFORE `-f Makefile.Release`,
#      otherwise make resolves `-f` against the post-`-C` cwd, looking for
#      app/app/Makefile.Release.  Original syntax `-f app/Makefile.Release
#      -C app` was wrong on both make 4.3 and 4.4 (silent path mismatch).
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

# We need to manually place SDL3 in our AppImage, since linuxdeployqt
# cannot see the dependency via ldd when it looks at SDL2-compat.
echo Staging SDL3 library
mkdir -p $DEPLOY_FOLDER/usr/lib
# VipleStream §K.1.fix (2026-06-20): SDL3 may live in /usr/local/lib (built from
# source) OR /usr/lib/<triplet> (distro sdl2-compat package).  Hardcoding
# /usr/local/lib/libSDL3.so.0 silently no-ops the cp on hosts where SDL3 came
# from apt (cp has no '|| fail'), then linuxdeploy --executable aborts later with
# "No such file: .../usr/lib/libSDL3.so.0".  Locate it, and fail loudly if absent.
SDL3_LIB=""
for cand in \
    /usr/local/lib/libSDL3.so.0 \
    /usr/lib/x86_64-linux-gnu/libSDL3.so.0 \
    /usr/lib/libSDL3.so.0; do
    if [ -e "$cand" ]; then SDL3_LIB="$cand"; break; fi
done
[ -z "$SDL3_LIB" ] && SDL3_LIB=$(find /usr/local/lib /usr/lib -name 'libSDL3.so.0' 2>/dev/null | head -1)
[ -n "$SDL3_LIB" ] || fail "libSDL3.so.0 not found (is SDL3 / sdl2-compat installed?)"
cp "$SDL3_LIB" $DEPLOY_FOLDER/usr/lib/libSDL3.so.0

# VipleStream §N.5.linux (v1.4.41): bundle Noto Sans CJK so zh_TW / ja / ko
# overlay text renders inside the AppImage even on hosts without
# fonts-noto-cjk installed (and without inheriting host fontconfig, which
# linuxdeploy + linuxdeploy-plugin-qt does NOT do).  main.cpp explicitly
# loads this path via QFontDatabase::addApplicationFont at runtime; if
# fontconfig already has CJK glyphs system-wide the load is harmless.
# Silent skip if the source font isn't installed on the builder — the
# AppImage then falls back to whatever the user's system has.
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
    echo "  fonts-noto-cjk not installed on builder — AppImage will fall back to"
    echo "  host fontconfig (CJK text may render as boxes on hosts without"
    echo "  fonts-noto-cjk).  apt install fonts-noto-cjk to bundle next time."
fi

echo Creating AppImage
pushd $INSTALLER_FOLDER
# VipleStream §K.1: linuxdeploy + qt plugin for library bundling ONLY.
# linuxdeploy's icon validation ("Could not find suitable icon") is
# fatally broken for SVG-only projects — it rejects SVG even with PNG
# fallback, even without --output appimage.  Solution: use linuxdeploy
# purely for Qt/lib bundling (no --desktop-file, no --icon-file), then
# hand-craft the AppDir root and package with appimagetool.

export QML_SOURCES_PATHS="$SOURCE_ROOT/app/gui"
export QMAKE=qmake6

# Step 1: linuxdeploy Qt library bundling only.
# linuxdeploy auto-scans usr/share/applications/ and validates icons
# even without --desktop-file.  Hide the desktop file during bundling
# to prevent the broken SVG icon validation from aborting the run.
DESKTOP_STASH=""
if [ -f $DEPLOY_FOLDER/usr/share/applications/viplestream.desktop ]; then
    DESKTOP_STASH=$(mktemp)
    mv $DEPLOY_FOLDER/usr/share/applications/viplestream.desktop "$DESKTOP_STASH"
fi
# VipleStream §K.1.fix (2026-06-20): libncnn.so.1 is built from source into a
# non-standard prefix (~/.local/ncnn/lib, or /usr/local/lib that only ships the
# unversioned libncnn.so without the libncnn.so.1 soname symlink), so it is NOT
# on ldconfig's default search path.  linuxdeploy's ldd-based dependency scan
# then aborts the whole AppImage build with
#   "Could not find dependency: libncnn.so.1".
# Locate libncnn.so.1 and expose its dir on LD_LIBRARY_PATH for the linuxdeploy
# invocation ONLY (scoped, so the later appimagetool/strip steps are unaffected).
NCNN_LIB_DIR=""
for d in "$HOME/.local/ncnn/lib" /usr/local/lib /usr/lib /usr/lib/x86_64-linux-gnu; do
    if [ -e "$d/libncnn.so.1" ]; then NCNN_LIB_DIR="$d"; break; fi
done
if [ -z "$NCNN_LIB_DIR" ]; then
    NCNN_LIB_DIR=$(dirname "$(find "$HOME/.local" -name 'libncnn.so.1' 2>/dev/null | head -1)" 2>/dev/null)
fi

LD_LIBRARY_PATH="${NCNN_LIB_DIR}:/usr/local/lib:${LD_LIBRARY_PATH}" \
linuxdeploy --appdir $DEPLOY_FOLDER \
    --executable $DEPLOY_FOLDER/usr/lib/libSDL3.so.0 \
    --plugin qt \
    || fail "linuxdeploy Qt bundling failed! (libncnn.so.1 findable? NCNN_LIB_DIR='$NCNN_LIB_DIR')"
# Restore desktop file
if [ -n "$DESKTOP_STASH" ] && [ -f "$DESKTOP_STASH" ]; then
    mv "$DESKTOP_STASH" $DEPLOY_FOLDER/usr/share/applications/viplestream.desktop
fi

# Step 2: Hand-craft AppDir root (desktop, icon, AppRun)
# Fix CRLF→LF (source tree may have Windows line endings from /mnt/d sync)
cp $SOURCE_ROOT/app/deploy/linux/viplestream.desktop $DEPLOY_FOLDER/viplestream.desktop
sed -i 's/\r$//' $DEPLOY_FOLDER/viplestream.desktop
mkdir -p $DEPLOY_FOLDER/usr/share/applications
cp $DEPLOY_FOLDER/viplestream.desktop $DEPLOY_FOLDER/usr/share/applications/
# §K.4-DESKTOP-ID 改名遺留：清掉舊 AppDir 可能殘存的 com.piinsta.desktop，
# 避免同一個 AppImage 桌面整合出現兩個 entry
rm -f $DEPLOY_FOLDER/com.piinsta.desktop $DEPLOY_FOLDER/usr/share/applications/com.piinsta.desktop

# Icon: SVG in hicolor as viplestream.svg (matches Icon=viplestream in .desktop)
# Also keep moonlight.svg for backwards compat with older .desktop files
mkdir -p $DEPLOY_FOLDER/usr/share/icons/hicolor/scalable/apps
cp $SOURCE_ROOT/app/res/moonlight.svg $DEPLOY_FOLDER/usr/share/icons/hicolor/scalable/apps/viplestream.svg
cp $SOURCE_ROOT/app/res/moonlight.svg $DEPLOY_FOLDER/usr/share/icons/hicolor/scalable/apps/moonlight.svg
# PNG at AppDir root for .DirIcon
if command -v rsvg-convert >/dev/null 2>&1; then
    rsvg-convert -w 256 -h 256 "$SOURCE_ROOT/app/res/moonlight.svg" -o "$DEPLOY_FOLDER/viplestream.png"
elif command -v convert >/dev/null 2>&1; then
    convert "$SOURCE_ROOT/app/res/moonlight.svg" -resize 256x256 "$DEPLOY_FOLDER/viplestream.png"
else
    cp "$SOURCE_ROOT/app/res/moonlight.svg" "$DEPLOY_FOLDER/viplestream.svg"
fi
[ -f "$DEPLOY_FOLDER/viplestream.png" ] && ln -sf viplestream.png $DEPLOY_FOLDER/.DirIcon

# AppRun
cat > "$DEPLOY_FOLDER/AppRun" << 'APPRUN_EOF'
#!/bin/bash
HERE="$(dirname "$(readlink -f "$0")")"
export LD_LIBRARY_PATH="$HERE/usr/lib:$LD_LIBRARY_PATH"
export QT_PLUGIN_PATH="$HERE/usr/plugins"
export QML2_IMPORT_PATH="$HERE/usr/qml"
exec "$HERE/usr/bin/viplestream" "$@"
APPRUN_EOF
chmod +x "$DEPLOY_FOLDER/AppRun"

# Step 2.5: Slim AppDir (§SLIM — 100 MB → ~48 MB target)
echo "Slimming AppDir..."

# 2.5a: Strip unused QtQuick Controls styles (keep Basic + Material only)
for sub in Fusion Imagine Universal designer; do
    [ -d "$DEPLOY_FOLDER/usr/qml/QtQuick/Controls/$sub" ] && \
        rm -rf "$DEPLOY_FOLDER/usr/qml/QtQuick/Controls/$sub"
done

# 2.5b: Strip unused plugins
rm -f "$DEPLOY_FOLDER/usr/plugins/imageformats/libqgif.so" \
      "$DEPLOY_FOLDER/usr/plugins/imageformats/libqico.so" \
      "$DEPLOY_FOLDER/usr/plugins/platforminputcontexts/libcomposeplatforminputcontextplugin.so" \
      "$DEPLOY_FOLDER/usr/plugins/tls/libqcertonlybackend.so" \
      "$DEPLOY_FOLDER/usr/plugins/xcbglintegrations/libqxcb-egl-integration.so"

# 2.5c: Strip translations (keep zh_TW + en only)
if [ -d "$DEPLOY_FOLDER/usr/translations" ]; then
    find "$DEPLOY_FOLDER/usr/translations" -maxdepth 1 -name '*.qm' \
        ! -name '*_zh_TW.qm' ! -name '*_en.qm' -delete
fi

# 2.5d: Drop flownet.bin (ModelFetcher lazy-downloads on first use)
rm -f "$DEPLOY_FOLDER/usr/share/VipleStream/rife-v4.25-lite/flownet.bin"

# 2.5e: Drop bundled NotoSansCJK (44 MB — falls back to system font)
rm -f "$DEPLOY_FOLDER/usr/share/fonts/opentype/NotoSansCJK-Regular.ttc" \
      "$DEPLOY_FOLDER/usr/share/fonts/truetype/NotoSansCJK-Regular.ttc"
rmdir "$DEPLOY_FOLDER/usr/share/fonts/opentype" \
      "$DEPLOY_FOLDER/usr/share/fonts/truetype" \
      "$DEPLOY_FOLDER/usr/share/fonts" 2>/dev/null || true

# 2.5f: Strip debug_info from all ELF binaries
for f in "$DEPLOY_FOLDER/usr/bin/viplestream"; do
    [ -f "$f" ] && strip --strip-unneeded "$f" 2>/dev/null || true
done
find "$DEPLOY_FOLDER/usr/lib" -maxdepth 1 -type f \( -name "*.so" -o -name "*.so.*" \) \
    -exec strip --strip-unneeded {} \; 2>/dev/null || true

# 2.5g: Drop usr/share/doc copyright noise (~3 MB)
rm -rf "$DEPLOY_FOLDER/usr/share/doc"

echo "AppDir after slim: $(du -sh $DEPLOY_FOLDER | cut -f1)"

# Step 3: Package with appimagetool
command -v appimagetool >/dev/null 2>&1 || fail "appimagetool not found in PATH"
# --no-appstream: skip desktop-file-validate pedantic checks (e.g.
# "more than one main category" warning that appimagetool treats as fatal).
ARCH=x86_64 appimagetool --no-appstream "$DEPLOY_FOLDER" \
    "VipleStream-Client-${VERSION}-linux-x64.AppImage" \
    || fail "appimagetool failed!"

popd

echo Build successful