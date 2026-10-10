#!/bin/bash
# §VR-REPAIR-RTT：server 這一側的純邏輯單元測試（googletest）獨立編譯並執行。
#
# 專案的建置把 Sunshine 的測試關掉（-DBUILD_TESTS=OFF），正式的一次是 Linux 上建出來的測試檔。開發機要先看結果時用
# 這一支：只編「不依賴 Sunshine 其他部分」的那幾個測試檔——它們 include 的 tests_common.h 換成只有 gtest 的替身。
# 不是專案的建置腳本，也不接進任何建置流程；只讀 repo 的原始碼。
#
# 用法：srv_gtest.sh [測試檔名…]
#   預設：test_vr_multilink.cpp test_vr_repair_rtt.cpp test_vr_fec_spread.cpp（Sunshine/tests/unit 底下）
# 產物放在 $VRSIM_BUILD（預設是這個目錄底下的 build/）。第一次要編 googletest 本身，約一分鐘；之後每次十幾秒。
# 需要 g++（C++20）；Windows 上用 MSYS2 ucrt64：export PATH=/c/msys64/ucrt64/bin:$PATH
# VRSIM_S（變異測試用）：先在這個目錄底下找 src/… 與 third-party/moonlight-common-c/…，找不到的才用 repo 的。
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../../.." && pwd)"
SUN="$ROOT/Sunshine"
G="$SUN/third-party/googletest/googletest"
OUT="${VRSIM_BUILD:-$HERE/build}"
TESTS="${*:-test_vr_multilink.cpp test_vr_repair_rtt.cpp test_vr_fec_spread.cpp}"
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) EXE=".exe" ;; *) EXE="" ;; esac

[ -f "$G/src/gtest-all.cc" ] || { echo "googletest sources not found in $G"; exit 2; }
GOBJ="${VRSIM_GTEST_OBJ:-$OUT/gtest}"
mkdir -p "$GOBJ" "$OUT/srv_gtest/tests/unit" || exit 2
if [ ! -f "$GOBJ/gtest-all.o" ] || [ ! -f "$GOBJ/gtest_main.o" ]; then
  echo "building googletest (once)"
  g++ -std=c++20 -O1 -w -I"$G/include" -I"$G" -c "$G/src/gtest-all.cc" -o "$GOBJ/gtest-all.o" || exit 1
  g++ -std=c++20 -O1 -w -I"$G/include" -I"$G" -c "$G/src/gtest_main.cc" -o "$GOBJ/gtest_main.o" || exit 1
fi

# 測試檔 include "../tests_common.h"：複製到暫存目錄，旁邊放只有 gtest 的替身
T="$OUT/srv_gtest/tests"
printf '// 替身：只給獨立編譯用（正式的 tests_common.h 會拉進整個 Sunshine）\n#pragma once\n#include <gtest/gtest.h>\n' > "$T/tests_common.h"
SRC=""
for t in $TESTS; do
  [ -f "$SUN/tests/unit/$t" ] || { echo "no such test file: Sunshine/tests/unit/$t"; exit 2; }
  cp "$SUN/tests/unit/$t" "$T/unit/$t" || exit 2
  SRC="$SRC $T/unit/$t"
done
INC=""
[ -n "$VRSIM_S" ] && INC="-I$VRSIM_S -I$VRSIM_S/third-party"
g++ -std=c++20 -O1 -Wall -Wextra $INC -I"$SUN" -I"$SUN/third-party" -I"$G/include" -I"$G" \
    $SRC "$GOBJ/gtest-all.o" "$GOBJ/gtest_main.o" -pthread -o "$OUT/srv_gtest/vr_ut$EXE" || { echo "srv_gtest: BUILD FAILED"; exit 1; }
"$OUT/srv_gtest/vr_ut$EXE" --gtest_brief=1
rc=$?
echo "srv_gtest: $([ $rc = 0 ] && echo PASS || echo FAILED)"
exit $rc
