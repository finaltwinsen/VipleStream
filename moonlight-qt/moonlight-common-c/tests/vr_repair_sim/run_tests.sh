#!/bin/bash
# §VR-REPAIR-RTT：這個目錄的測試全部跑一遍（先編、再跑）。任何一項沒過就回傳非 0。說明見 README.md。
#
#   bash run_tests.sh               單元測試（C、C++）＋現行規則對 golden（2,844 個情境）＋亂數不變式＋逐項情境＋行為表
#                                   ＋server 的 googletest（獨立編譯）
#   bash run_tests.sh --quick       只跑單元測試與 golden
#   bash run_tests.sh --ref <提交>  另外從那個提交取出改動前的佇列，三個執行檔直接逐字比（不靠 golden 檔）
#   bash run_tests.sh --mutate      最後再跑變異測試（約 10～15 分鐘）
#
# 需要 gcc／g++、bash、python 3；Windows 上用 MSYS2 ucrt64：export PATH=/c/msys64/ucrt64/bin:$PATH
# 產物放在 $VRSIM_BUILD（預設是這個目錄底下的 build/）。不含變異測試約 3 分鐘（第一次要多編 googletest，約 1 分鐘）。
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE" || exit 2
export PYTHONIOENCODING=utf-8
OUT="${VRSIM_BUILD:-$HERE/build}"
QUICK=""; REF=""; MUTATE=""
while [ $# -gt 0 ]; do
  case "$1" in
    --quick) QUICK=1 ;;
    --mutate) MUTATE=1 ;;
    --ref) REF="$2"; shift ;;
    *) echo "usage: run_tests.sh [--quick] [--ref <commit>] [--mutate]"; exit 2 ;;
  esac
  shift
done
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) EXE=".exe" ;; *) EXE="" ;; esac
PY=python3; command -v python3 >/dev/null 2>&1 || PY=python

if [ -n "$REF" ]; then bash build.sh --ref "$REF" || exit 1; else bash build.sh || exit 1; fi

rc=0
step() {  # 名稱 指令…
  local name="$1"; shift
  echo "=== $name"
  "$@" || { echo "=== $name: FAILED"; rc=1; }
}
step "VrRepairRtt.h (C)" "$OUT/test_vrrr$EXE"
step "VrRepairRtt.h (C++)" "$OUT/test_vrrr_cpp$EXE"
step "legacy rules vs golden" $PY golden.py check
[ -n "$REF" ] && step "legacy rules, three-way against $REF" $PY golden.py three
if [ -z "$QUICK" ]; then
  step "fuzz invariants" $PY fuzz.py all
  step "scenarios" $PY scen.py
  step "behaviour matrix" $PY matrix.py
  step "server unit tests (googletest, standalone)" bash srv_gtest.sh
fi
[ -n "$MUTATE" ] && step "mutation tests" $PY mutate.py
echo "=== vr_repair_sim: $([ $rc = 0 ] && echo ALL PASS || echo FAILED)"
exit $rc
