#!/usr/bin/env bash
# VipleStream §Q: Cross-compile picoquic + picotls for Android NDK.
#
# Produces prebuilt static libraries (.a) for arm64-v8a and armeabi-v7a
# that Android.mk picks up as PREBUILT_STATIC_LIBRARY modules.
#
# Prerequisites:
#   - Android NDK r30 installed (ANDROID_NDK_HOME set or detected)
#   - CMake 3.20+ on PATH
#   - Git (to clone picotls)
#   - Internet access (first run clones picotls)
#
# Usage:
#   cd moonlight-android/app/src/main/jni/moonlight-core/picoquic
#   bash build_picoquic_android.sh
#
# After success: arm64-v8a/ and armeabi-v7a/ contain .a files,
# include/ has headers.  Commit these prebuilt files.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PICOQUIC_SRC="${SCRIPT_DIR}/../../../../../../Sunshine/third-party/picoquic"
PICOTLS_SRC="${SCRIPT_DIR}/picotls-src"
BUILD_ROOT="${SCRIPT_DIR}/build-tmp"
OUTPUT_DIR="${SCRIPT_DIR}"

# ── Detect NDK ───────────────────────────────────────────────
if [ -z "${ANDROID_NDK_HOME:-}" ]; then
    # Try common locations
    for candidate in \
        "$HOME/Android/Sdk/ndk/30.0.14904198" \
        "$HOME/Library/Android/sdk/ndk/30.0.14904198" \
        "/opt/android-ndk" \
        "${ANDROID_SDK_ROOT:-/nonexistent}/ndk/30.0.14904198"; do
        if [ -d "$candidate" ]; then
            ANDROID_NDK_HOME="$candidate"
            break
        fi
    done
fi

if [ -z "${ANDROID_NDK_HOME:-}" ] || [ ! -d "$ANDROID_NDK_HOME" ]; then
    echo "ERROR: ANDROID_NDK_HOME not set or not found."
    echo "Set ANDROID_NDK_HOME to your NDK r30 installation path."
    exit 1
fi
echo "Using NDK: $ANDROID_NDK_HOME"

TOOLCHAIN="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake"
if [ ! -f "$TOOLCHAIN" ]; then
    echo "ERROR: CMake toolchain not found at $TOOLCHAIN"
    exit 1
fi

# ── Clone picotls if needed ──────────────────────────────────
if [ ! -d "$PICOTLS_SRC" ]; then
    echo "Cloning picotls..."
    git clone --depth 1 https://github.com/h2o/picotls.git "$PICOTLS_SRC"
fi

# ── Verify picoquic source ───────────────────────────────────
if [ ! -f "$PICOQUIC_SRC/CMakeLists.txt" ]; then
    echo "ERROR: picoquic source not found at $PICOQUIC_SRC"
    echo "Ensure Sunshine/third-party/picoquic submodule is checked out."
    exit 1
fi

# ── OpenSSL headers from the Android prebuilt ────────────────
OPENSSL_INCLUDE="${SCRIPT_DIR}/../openssl/include"
if [ ! -f "$OPENSSL_INCLUDE/openssl/ssl.h" ]; then
    echo "ERROR: OpenSSL headers not found at $OPENSSL_INCLUDE"
    exit 1
fi

# ── Build function ───────────────────────────────────────────
build_abi() {
    local ABI=$1
    local API=$2
    local BUILD_DIR="${BUILD_ROOT}/${ABI}"

    echo "═══ Building for $ABI (API $API) ═══"
    rm -rf "$BUILD_DIR"
    mkdir -p "$BUILD_DIR"

    # Build picotls first
    echo "  Building picotls..."
    cmake -S "$PICOTLS_SRC" -B "$BUILD_DIR/picotls" \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
        -DANDROID_ABI="$ABI" \
        -DANDROID_PLATFORM="android-$API" \
        -DCMAKE_BUILD_TYPE=Release \
        -DOPENSSL_ROOT_DIR="$OPENSSL_INCLUDE/.." \
        -DOPENSSL_INCLUDE_DIR="$OPENSSL_INCLUDE" \
        -DWITH_AEGIS=OFF \
        -DWITH_FUSION=OFF \
        -DWITH_MBEDTLS=OFF \
        -DBUILD_TESTING=OFF \
        2>&1 | tail -5

    cmake --build "$BUILD_DIR/picotls" --parallel 2>&1 | tail -3

    # Build picoquic
    echo "  Building picoquic..."
    cmake -S "$PICOQUIC_SRC" -B "$BUILD_DIR/picoquic" \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
        -DANDROID_ABI="$ABI" \
        -DANDROID_PLATFORM="android-$API" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_PREFIX_PATH="$BUILD_DIR/picotls" \
        -DPTLS_INCLUDE_DIRS="$PICOTLS_SRC/include" \
        -DPTLS_LIBRARIES="$BUILD_DIR/picotls" \
        -DOPENSSL_ROOT_DIR="$OPENSSL_INCLUDE/.." \
        -DOPENSSL_INCLUDE_DIR="$OPENSSL_INCLUDE" \
        -DPICOQUIC_FETCH_PTLS=OFF \
        -DWITH_OPENSSL=ON \
        -DWITH_AEGIS=OFF \
        -DBUILD_TESTING=OFF \
        2>&1 | tail -5

    cmake --build "$BUILD_DIR/picoquic" --target picoquic-core --parallel 2>&1 | tail -3

    # Copy artifacts
    echo "  Copying artifacts for $ABI..."
    local OUT="${OUTPUT_DIR}/${ABI}"
    mkdir -p "$OUT"

    # picoquic-core
    find "$BUILD_DIR/picoquic" -name "libpicoquic-core.a" | head -1 | while read f; do
        cp "$f" "$OUT/"
        echo "    → libpicoquic-core.a"
    done

    # picotls libraries
    for lib in picotls-openssl picotls-core picotls-minicrypto; do
        find "$BUILD_DIR/picotls" -name "lib${lib}.a" | head -1 | while read f; do
            cp "$f" "$OUT/"
            echo "    → lib${lib}.a"
        done
    done

    echo "  Done: $ABI"
}

# ── Build both ABIs ──────────────────────────────────────────
build_abi "arm64-v8a"   21
build_abi "armeabi-v7a"  21

# ── Verify ───────────────────────────────────────────────────
echo ""
echo "═══ Build complete ═══"
for abi in arm64-v8a armeabi-v7a; do
    echo "  $abi:"
    for lib in libpicoquic-core.a libpicotls-openssl.a libpicotls-core.a libpicotls-minicrypto.a; do
        if [ -f "${OUTPUT_DIR}/${abi}/${lib}" ]; then
            size=$(du -h "${OUTPUT_DIR}/${abi}/${lib}" | cut -f1)
            echo "    ✓ $lib ($size)"
        else
            echo "    ✗ $lib MISSING"
        fi
    done
done

# ── Cleanup ──────────────────────────────────────────────────
echo ""
echo "Cleaning up build temp..."
rm -rf "$BUILD_ROOT"
echo "Done. Prebuilt libraries are ready for ndk-build."
