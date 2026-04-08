#!/usr/bin/bash
# Called by build_sunshine.cmd — do NOT run directly
export MSYSTEM=UCRT64
source /etc/profile
set -e

# Derive project root from script location
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"
SRC="$ROOT/Sunshine"
BUILD="$SRC/build_mingw"

# Clear stale CMake cache but keep object files for incremental build
if [ -f "$BUILD/CMakeCache.txt" ]; then
    rm -f "$BUILD/CMakeCache.txt"
    rm -rf "$BUILD/CMakeFiles"
fi

# Use cached Boost/FFmpeg if available, otherwise let CMake download them
EXTRA_FLAGS=""
if [ -d "$SRC/build/_deps/boost-src" ]; then
    EXTRA_FLAGS="$EXTRA_FLAGS -DFETCHCONTENT_SOURCE_DIR_BOOST=$SRC/build/_deps/boost-src"
fi

# Prefer build_mingw/_deps/ffmpeg (auto-downloaded), then build/_deps/ffmpeg (manual cache)
if [ -d "$BUILD/_deps/ffmpeg" ]; then
    EXTRA_FLAGS="$EXTRA_FLAGS -DFFMPEG_PREPARED_BINARIES=$BUILD/_deps/ffmpeg"
elif [ -d "$SRC/build/_deps/ffmpeg" ]; then
    EXTRA_FLAGS="$EXTRA_FLAGS -DFFMPEG_PREPARED_BINARIES=$SRC/build/_deps/ffmpeg"
fi

cmake -S "$SRC" -B "$BUILD" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_SYSTEM_PROCESSOR=AMD64 \
    -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF \
    -DBUILD_TESTS=OFF \
    -DBUILD_DOCS=OFF \
    -DCMAKE_EXE_LINKER_FLAGS="-Wl,--allow-multiple-definition" \
    $EXTRA_FLAGS

cmake --build "$BUILD" --parallel $(nproc)
