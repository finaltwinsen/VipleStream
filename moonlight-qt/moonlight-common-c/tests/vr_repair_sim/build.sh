#!/bin/bash
# §VR-REPAIR-RTT：模擬器的獨立編譯（不是專案的建置腳本，也不接進任何建置流程；只讀 repo 的原始碼）。
#
# 用法：build.sh [--ref <git 提交>]
#   預設：編目前工作樹的佇列
#     sim_final          每個封包的決定走 vrrrDecide()（和 VideoStream.c 一樣）
#     sim_final_inline   每個封包的決定用「改動前寫在 VideoStream.c 接收迴圈裡的那幾行」（對照用）
#     test_vrrr、test_vrrr_cpp   VrRepairRtt.h 的單元測試（C 與 C++ 各編一次）
#   --ref <提交>：另外用 git archive 取出那個提交的 common-c 與 vr_multilink_logic.h，編出
#     sim_ref_inline     那個提交的佇列＋原樣搬來的逐封包設定   ← golden 就是它的輸出
#     sim_ref            那個提交的佇列＋vrrrDecide()
#   重新產生 golden、或想看時間軸哪裡不同時才需要（<提交> 要是佇列還沒有 §VR-REPAIR-RTT 欄位的版本）。
#
# 產物放在 $VRSIM_BUILD（預設是這個目錄底下的 build/，已列在 .gitignore）。
# 需要 gcc／g++；Windows 上用 MSYS2 ucrt64：export PATH=/c/msys64/ucrt64/bin:$PATH
#
# 變異測試（mutate.py）用的兩個環境變數：VRSIM_Q＝改用這個目錄當 common-c 的 src（改過的複本；enet、nanors 仍用
# repo 的）、VRSIM_S＝改用這個目錄當 Sunshine 的根（底下要有 src/vr/vr_multilink_logic.h 與
# third-party/moonlight-common-c/src/VipleVr.h）。平常不用設。
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../../.." && pwd)"
DEPS="$ROOT/moonlight-qt/moonlight-common-c/moonlight-common-c"
Q="${VRSIM_Q:-$DEPS/src}"
S="${VRSIM_S:-$ROOT/Sunshine}"
OUT="${VRSIM_BUILD:-$HERE/build}"
REF=""
if [ "$1" = "--ref" ]; then
  REF="$2"
  [ -n "$REF" ] || { echo "usage: build.sh [--ref <commit>]"; exit 2; }
fi
case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*) LIBS="-lws2_32"; EXE=".exe" ;;
  *) LIBS=""; EXE="" ;;
esac
CF="-std=gnu11 -O1 -DHAS_SOCKLEN_T -w"
mkdir -p "$OUT"

# build_sim <輸出名> <common-c 的 src> <enet／nanors 所在的目錄> <Sunshine 根> [額外的旗標…]
build_sim() {
  local name="$1" q="$2" deps="$3" s="$4"; shift 4
  local obj="$OUT/$name.obj"
  local inc="-I$q -I$deps/enet/include -I$deps/nanors -I$deps/nanors/deps -I$deps/nanors/deps/obl"
  mkdir -p "$obj"
  gcc $CF $inc -c "$q/rswrapper.c" -o "$obj/rswrapper.o"
  gcc $CF $inc -c "$q/RtpVideoQueue.c" -o "$obj/RtpVideoQueue.o"
  gcc $CF "$@" $inc -c "$HERE/sim.c" -o "$obj/sim.o"
  g++ -std=c++20 -O1 -w "$@" -I"$s" -I"$s/third-party" -c "$HERE/server_model.cpp" -o "$obj/server_model.o"
  g++ "$obj/sim.o" "$obj/RtpVideoQueue.o" "$obj/rswrapper.o" "$obj/server_model.o" $LIBS -o "$OUT/$name$EXE"
  echo "built $name"
}

build_sim sim_final "$Q" "$DEPS" "$S"
build_sim sim_final_inline "$Q" "$DEPS" "$S" -DLEGACY_INLINE

gcc -std=gnu11 -Wall -Wextra -I"$Q" -o "$OUT/test_vrrr$EXE" "$HERE/test_vrrr.c"
g++ -std=c++20 -x c++ -Wall -Wextra -I"$Q" -o "$OUT/test_vrrr_cpp$EXE" "$HERE/test_vrrr.c"
echo "built test_vrrr, test_vrrr_cpp"

if [ -n "$REF" ]; then
  REFDIR="$OUT/ref_src"
  rm -rf "$REFDIR"
  mkdir -p "$REFDIR/shim"
  git -C "$ROOT" archive --format=tar "$REF" moonlight-qt/moonlight-common-c/moonlight-common-c Sunshine/src/vr/vr_multilink_logic.h \
      Sunshine/third-party/moonlight-common-c/src/VipleVr.h | tar -x -C "$REFDIR"
  RQ="$REFDIR/moonlight-qt/moonlight-common-c/moonlight-common-c/src"
  if grep -q mlRttMode "$RQ/RtpVideoQueue.h"; then
    echo "error: $REF already has the §VR-REPAIR-RTT queue; --ref needs a commit from before it" >&2
    exit 2
  fi
  # 那個提交還沒有 VrRepairRtt.h（模擬器要用它的型別）：用目前這一份，NACK 的兩個常數也補上
  cp "$Q/VrRepairRtt.h" "$REFDIR/shim/"
  REFFLAGS="-DSIM_QUEUE_ORIG -DSIM_SERVER_ORIG -idirafter $REFDIR/shim"
  grep -q VIPLE_VR_NACK_F_RTT "$RQ/VipleVr.h" || REFFLAGS="$REFFLAGS -DVIPLE_VR_NACK_F_RTT=0x80 -DVIPLE_VR_NACK_ETA_MASK=0x7F"
  build_sim sim_ref_inline "$RQ" "$RQ/.." "$REFDIR/Sunshine" $REFFLAGS -DLEGACY_INLINE
  build_sim sim_ref "$RQ" "$RQ/.." "$REFDIR/Sunshine" $REFFLAGS
fi
echo "output: $OUT"
