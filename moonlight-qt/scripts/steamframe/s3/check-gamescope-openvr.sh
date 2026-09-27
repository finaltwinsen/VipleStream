#!/bin/bash
# VipleStream §S3（M2a，PoC-F-pre 前置）— 偵測這台機器的 gamescope 有沒有編進 OpenVR backend
#
#   bash moonlight-qt/scripts/steamframe/s3/check-gamescope-openvr.sh [--gamescope PATH]
#
# S3（docs/vr_architecture.md §8.1）要在 linux-builder 上用 `gamescope --backend openvr` 把平面
# app 變成 SteamVR overlay（α 的形態）。Ubuntu 26.04 的 gamescope 套件有沒有開 openvr 支援
# 沒有查證過（UNVERIFIED），這支就是用來回答它。
#
# 只讀：不啟動 compositor、不需要 Steam、不連網、不安裝任何東西。
# 判斷依據（由強到弱）：
#   1. 執行檔動態連結 libopenvr_api（ldd）；
#   2. 執行檔內含 OpenVR 的符號／字串（VR_InitInternal、IVROverlay、openvr_api）——openvr 以
#      meson subproject 靜態連結時只剩這條；
#   3. `gamescope --help` 的 backend 說明提到 openvr（說明文字可能是靜態的，只當旁證）。
#
# 結束碼：0 有 openvr backend；3 沒有或無法判斷；4 沒裝 gamescope；1 參數錯誤。
set -u

GS=""

usage()
{
	cat <<'EOF'
用法：bash check-gamescope-openvr.sh [--gamescope PATH]
  --gamescope PATH   要檢查的 gamescope 執行檔（預設 PATH 上的 gamescope）
結束碼：0 有 openvr backend；3 沒有或無法判斷；4 沒裝 gamescope；1 參數錯誤
EOF
}

while [ $# -gt 0 ]; do
	case "$1" in
		--gamescope) [ $# -ge 2 ] || { usage 1>&2; exit 1; }; GS=$2; shift 2 ;;
		--gamescope=*) GS=${1#*=}; shift ;;
		-h|--help) usage; exit 0 ;;
		*) echo "不認得的參數：$1" 1>&2; usage 1>&2; exit 1 ;;
	esac
done

say()
{
	printf '[s3-gamescope] %s\n' "$*"
}

if [ -z "$GS" ]; then
	GS=$(command -v gamescope 2>/dev/null || true)
fi

if [ -z "$GS" ] || [ ! -x "$GS" ]; then
	say "gamescope：沒有安裝（或 --gamescope 指的檔案不可執行）"
	if command -v apt-cache >/dev/null 2>&1; then
		say "apt-cache policy gamescope（本機快取，不連網）："
		apt-cache policy gamescope 2>&1 | sed 's/^/    /'
	fi
	say "要測 S3 時，經使用者同意後手動安裝（本腳本不安裝）：sudo apt install gamescope，裝完重跑這支"
	say "verdict: not-installed"
	exit 4
fi

GS=$(readlink -f "$GS")
say "gamescope：$GS"
"$GS" --version 2>&1 | head -n 3 | sed 's/^/    /'
if command -v dpkg-query >/dev/null 2>&1; then
	pkg=$(dpkg-query -S "$GS" 2>/dev/null | head -n 1 | cut -d: -f1)
	if [ -n "$pkg" ]; then
		say "套件：$(dpkg-query -W -f='${Package} ${Version}' "$pkg" 2>/dev/null)"
	fi
fi

evidence=()
strong=0

# 1. 動態連結
if command -v ldd >/dev/null 2>&1; then
	ldd_out=$(ldd "$GS" 2>&1)
	if grep -qi 'libopenvr_api' <<< "$ldd_out"; then
		evidence+=("ldd: $(grep -i 'libopenvr_api' <<< "$ldd_out" | head -n 1 | sed 's/^[[:space:]]*//')")
		strong=1
	fi
fi

# 2. 執行檔內的 OpenVR 符號／字串（不需要 binutils）
hits=$(grep -a -o -E 'VR_InitInternal2?|IVROverlay_[0-9]+|openvr_api|VROverlay' "$GS" 2>/dev/null | sort -u | head -n 8 | tr '\n' ' ')
if [ -n "$hits" ]; then
	evidence+=("binary strings: $hits")
	strong=1
fi

# 3. --help 的 backend 說明（旁證）
help_out=$("$GS" --help 2>&1 | head -n 200)
if grep -qi 'openvr' <<< "$help_out"; then
	evidence+=("--help 提到 openvr：$(grep -i 'openvr' <<< "$help_out" | head -n 3 | sed 's/^[[:space:]]*//' | tr '\n' ' ')")
fi

if [ ${#evidence[@]} -gt 0 ]; then
	say "證據："
	for e in "${evidence[@]}"; do
		say "  - $e"
	done
fi

if [ "$strong" -eq 1 ]; then
	say "verdict: openvr-backend-present"
	say "下一步：s3/README.md 第 5 步（SteamVR 以 null driver 啟動後，gamescope --backend openvr -- <app>）"
	exit 0
fi

if [ ${#evidence[@]} -gt 0 ]; then
	say "verdict: unknown（只有 --help 的旁證，執行檔裡找不到 OpenVR 符號；很可能沒編進 openvr）"
else
	say "verdict: openvr-backend-absent"
fi
say "沒有 openvr backend 時的選項（交使用者決定）：自建 gamescope（meson 選項 enable_openvr_support=enabled，"
say "選項名稱依 gamescope 版本 UNVERIFIED；裝到隔離前綴，不進 /usr/local）、改用 Flatpak／容器內的 gamescope，"
say "或 S3 只做 xr-probe 那一半（run-xr-probe-steamvr.sh，不需要 gamescope）"
exit 3
