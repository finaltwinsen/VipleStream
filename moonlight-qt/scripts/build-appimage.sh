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
command -v linuxdeployqt >/dev/null 2>&1 || fail "Unable to find 'linuxdeployqt' in your PATH!"

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
qmake6 $SOURCE_ROOT/moonlight-qt.pro CONFIG+=disable-wayland CONFIG+=disable-libdrm PREFIX=$DEPLOY_FOLDER/usr DEFINES+=APP_IMAGE || fail "Qmake failed!"
popd

echo Compiling Moonlight in $BUILD_CONFIG configuration
pushd $BUILD_FOLDER
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
cp /usr/local/lib/libSDL3.so.0 $DEPLOY_FOLDER/usr/lib/

echo Creating AppImage
pushd $INSTALLER_FOLDER
# VipleStream §K.1: switched from linuxdeployqt to linuxdeploy + qt plugin.
# Reason: linuxdeployqt continuous build hard-blocks Ubuntu noble's glibc
# 2.39 (> 2.35 max), and -unsupported-allow-new-glibc only suppresses the
# warning — the bundle step then silently fails with exit 1.  linuxdeploy
# (different project) has no such restriction and is actively maintained.
# Both produce equivalent AppImages.

export QML_SOURCES_PATHS="$SOURCE_ROOT/app/gui"
export QMAKE=qmake6
# linuxdeploy looks up the desktop file's Icon= entry by name across standard
# search paths.  Place the icon at the canonical /usr/share/icons/hicolor/...
# path (already there from `make install`) AND at AppDir root so linuxdeploy's
# .DirIcon symlink + AppImage thumbnail both resolve.
cp $DEPLOY_FOLDER/usr/share/icons/hicolor/scalable/apps/moonlight.svg $DEPLOY_FOLDER/moonlight.svg
linuxdeploy --appdir $DEPLOY_FOLDER \
    --desktop-file $DEPLOY_FOLDER/usr/share/applications/com.piinsta.desktop \
    --icon-file $DEPLOY_FOLDER/usr/share/icons/hicolor/scalable/apps/moonlight.svg \
    --icon-filename moonlight \
    --executable $DEPLOY_FOLDER/usr/lib/libSDL3.so.0 \
    --plugin qt \
    --output appimage \
    || fail "linuxdeploy failed!"
# linuxdeploy emits VipleStream-x86_64.AppImage by default; rename to match
# the established release naming scheme.
APPIMAGE_RAW=$(ls VipleStream-*.AppImage 2>/dev/null | head -1)
if [ -n "$APPIMAGE_RAW" ]; then
    mv "$APPIMAGE_RAW" "VipleStream-Client-${VERSION}-linux-x64.AppImage"
fi
popd

echo Build successful