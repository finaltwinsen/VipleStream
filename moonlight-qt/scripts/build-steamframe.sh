#!/bin/bash
# VipleStream §SF-BUILD（M2a α）— Steam Frame client 的 Flatpak 建置
#
#   bash moonlight-qt/scripts/build-steamframe.sh --arch x86_64|aarch64 --flavor dev|release [選項]
#
# 完整說明：--help。重點：
#   - 不自動安裝任何東西：缺工具或 runtime 時印出確切的安裝指令（flatpak 一律 --user）並以 2 結束。
#   - dev＝工作樹快照（含未 commit 的改動）；release＝pin 在 HEAD 的 git 來源，相關路徑必須乾淨。
#   - G-BUILD 計時只算建置階段：來源先下載（不計時），建置一律 --disable-download --disable-updates；
#     flatpak-builder 的每一行輸出前面加 epoch 秒，結束時印各模組耗時表。
#   - 長建置用 nohup setsid bash … & disown 再輪詢（見 --help；經 SSH 不要包 systemd-inhibit）。
#   - 同一個 --work 同時只能跑一個（flock）；--smoke-only 不建置、只重跑 smoke。
#
# 在 linux-builder（Ubuntu 26.04，x86_64）上跑；aarch64 靠 qemu-user（binfmt 要有 F flag）。
# 建置規範（CLAUDE.md）：Flatpak 一律走這支腳本，不要手動呼叫 flatpak-builder。
set -u
set -o pipefail

readonly APP_ID="io.github.finaltwinsen.VipleStream"
# 和 manifest 的 runtime-version 一致（退路 6.10 時兩邊一起改）。
readonly KDE_BRANCH="6.11"
readonly FLATHUB_REPO_URL="https://dl.flathub.org/repo/flathub.flatpakrepo"
readonly PICOQUIC_URL="https://github.com/finaltwinsen/picoquic.git"
# release 的主 repo 來源一律寫這個公開 URL（/app/manifest.json 會帶著它，不能是 builder 的本機路徑）。
readonly VIPLE_URL="https://github.com/finaltwinsen/VipleStream.git"
readonly LOCAL_REMOTE="viple-local"
readonly MIN_FREE_GB=30

# 結束碼（--help 有同一張表）
readonly RC_USAGE=1
readonly RC_PREREQ=2
readonly RC_SOURCE=3
readonly RC_BUILD=4
readonly RC_BUNDLE=5
readonly RC_SMOKE=6

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P) || exit 1
MQT_DIR=$(cd "$SCRIPT_DIR/.." && pwd -P) || exit 1
REPO=$(cd "$MQT_DIR/.." && pwd -P) || exit 1
FLATPAK_SRC_DIR="$SCRIPT_DIR/steamframe/flatpak"

ARCH=""
FLAVOR=""
WORK="${HOME}/viple-steamframe"
JOBS=""
DO_CLEAN=0
DOWNLOAD_ONLY=0
DO_BUNDLE=0
DO_SMOKE=0
SMOKE_ONLY=0
CHECK_ONLY=0
# release：HEAD 能不能從 GitHub 上的 ref 到達（check_release_source 判定；決定下載方式與產物檔名）
HEAD_PUBLISHED=0
# run_fb 額外帶給 flatpak-builder 的環境變數（VAR=值）
FB_ENV=()

MAIN_LOG=""
TS=$(date +%Y%m%d-%H%M%S)

# ---------------------------------------------------------------------------
# 共用小工具
# ---------------------------------------------------------------------------
log()
{
	local line
	line=$(printf '[build-steamframe] %(%Y-%m-%d %H:%M:%S)T %s' -1 "$*")
	printf '%s\n' "$line"
	if [ -n "$MAIN_LOG" ]; then
		printf '%s\n' "$line" >> "$MAIN_LOG"
	fi
}

die()
{
	local rc=$1
	shift
	log "ERROR: $*" 1>&2
	exit "$rc"
}

# 每行前面加 epoch 秒（G-BUILD 的模組耗時從這裡推算）。
ts_filter()
{
	local line
	while IFS= read -r line || [ -n "$line" ]; do
		printf '%(%s)T %s\n' -1 "$line"
	done
}

bundle_arch_name()
{
	case "$1" in
		aarch64) echo "arm64" ;;
		x86_64) echo "x64" ;;
		*) echo "$1" ;;
	esac
}

# release bundle 檔名。$1=1：HEAD 已在 GitHub 上（正式檔名）；0：還沒 push（-unpublished，不可上 release）。
release_bundle_name()
{
	if [ "$1" -eq 1 ]; then
		echo "VipleStream-Client-$VERSION-linux-arm64.flatpak"
	else
		echo "VipleStream-Client-$VERSION-linux-arm64-unpublished.flatpak"
	fi
}

# 工具版號：取輸出裡第一個像版號的字串（不假設「名稱 版號」的欄位位置），取不到再問 dpkg。
tool_version()
{
	local v
	command -v "$1" >/dev/null 2>&1 || { printf '未安裝'; return 0; }
	v=$("$1" --version 2>&1 | head -n 5)
	v=$(grep -Eo '[0-9]+(\.[0-9]+)+' <<< "$v" | head -n 1)
	if [ -z "$v" ] && command -v dpkg-query >/dev/null 2>&1; then
		# shellcheck disable=SC2016  # ${Version} 是 dpkg-query 的格式欄位，不是 shell 變數
		v=$(dpkg-query -W -f='${Version}' "$1" 2>/dev/null)
	fi
	printf '%s' "${v:-unknown}"
}

usage()
{
	cat <<EOF
VipleStream Steam Frame client — Flatpak 建置（app-id $APP_ID，KDE runtime $KDE_BRANCH）

用法：
  bash moonlight-qt/scripts/build-steamframe.sh --arch x86_64|aarch64 --flavor dev|release [選項]

必要參數：
  --arch x86_64|aarch64   目標架構。x86_64 主機上建 aarch64 靠 qemu-user（binfmt 要有 F flag）。
  --flavor dev|release    dev：工作樹快照（含未 commit 的改動）、flatpak branch=dev、app 關 LTO。
                          release：pin 在 HEAD 的 git 來源、branch=stable、app 開 LTO；
                          moonlight-qt 與 picoquic 必須乾淨，version.txt 必須等於 version.json。
                          只允許 aarch64（x86_64 Flatpak 只給開發用，見 finish-args.md「libdrm 決定」）。

選項：
  --work DIR        工作目錄（預設 \$HOME/viple-steamframe；不可在 repo 內）。state（下載與快取）、
                    manifest/、repo/ 兩個 arch 共用。同一個 --work 同時只能跑一個建置（腳本用 flock 擋下，
                    rc 1）；兩個 arch 要同時建，就各用自己的 --work（state 不共用，下載會各做一次）。
  --jobs N          平行度（傳給 flatpak-builder --jobs）。
  --clean           建置前清掉快取：state 的 cache/ ccache/ build/ checksums/ 與該 arch 的 build 目錄；
                    保留 git/ downloads/（量乾淨建置時不把下載時間算進去）。快取兩個 arch 共用，會一起清。
  --download-only   只下載來源，不建置、不計時。runtime 還沒裝好也能先跑。
  --bundle          產出 .flatpak（release 一律產）。
  --smoke           把產物裝進 --user 的本地 remote（$LOCAL_REMOTE）後跑 --help ×2、--version ×1，
                    GUI 環境與純 SSH 環境（--nosocket=wayland --nosocket=fallback-x11）各一輪；
                    release 另外用 flatpak install --bundle 裝產出的 .flatpak 再驗一次。
                    這（和 --smoke-only）是唯一會改動 builder 上 flatpak 狀態的選項（--user，不需要 sudo）。
  --smoke-only      不準備來源、不下載、不建置，直接對 --work 的 repo/ 裡現有的 <app-id>//<branch> 跑
                    --smoke 的那幾輪（例：建置時沒有 GUI session，登入桌面後補跑；viplestream 模組每次都會
                    重建，不必為了重跑 smoke 再建一次）。--version 以安裝進來的 SOURCE_INFO 版號核對；
                    release 另外用 out/ 裡同版號最新的 bundle 驗（沒有就略過那一輪）。
                    不能和 --clean、--download-only、--bundle 一起用。
  --check           只做前置檢查，印出缺什麼與確切的安裝指令，不建置。
  -h, --help        顯示這段說明。

結束碼：
  0 成功；1 參數錯誤；2 前置條件不足（已印出安裝指令）；3 來源／版號檢查失敗；
  4 flatpak-builder 失敗；5 bundle 失敗；6 smoke test 失敗。

輸出（都在 --work 底下）：
  manifest/                                  這次實際用的 manifest、viplestream-source.json、
                                             flavor.txt、SOURCE_INFO、dev 快照（src/）
  logs/steamframe-<arch>-<flavor>-<時間>.log      本腳本的記錄
  logs/fb-<arch>-<flavor>-<download|build>-<時間>.log
                                             flatpak-builder 輸出（每行前面是 epoch 秒）
  logs/gbuild-<arch>-<flavor>-<時間>.tsv          各模組耗時表
  logs/gbuild-history.tsv                    每次建置一行（G-BUILD 紀錄）
  logs/smoke-<arch>-<flavor>-<時間>-<case>.txt    smoke test 各次輸出
  repo/                                      ostree repo（branch dev／stable）
  out/VipleStream-Client-<版號>-linux-arm64.flatpak           release（HEAD 已在 GitHub 上）
  out/VipleStream-Client-<版號>-linux-arm64-unpublished.flatpak
                                             release 但 HEAD 還沒 push：不可上 release（見「原始碼同步」）
  out/VipleStream-Client-<版號>-linux-<arm64|x64>-dev.flatpak  dev（--bundle）
                                             每個 .flatpak 旁附 .sha256

長建置（SSH 斷線不影響；repo 內 .sh 是 100644，一律用 bash 呼叫）：
  nohup setsid bash moonlight-qt/scripts/build-steamframe.sh --arch aarch64 --flavor dev \\
      > ~/steamframe-build.log 2>&1 < /dev/null & disown
  之後輪詢：tail -n 50 ~/steamframe-build.log
  經 SSH 不要包 systemd-inhibit：polkit 會回 "interactive authentication required"，整條命令直接失敗。
  builder 已把 sleep.target mask 掉，本來就不會休眠。只有在本機桌面 session 內（或用 sudo 取得
  inhibitor）才可選擇性地加 systemd-inhibit --what=sleep:idle；建置本身一律用自己的帳號跑，不要以 root 執行。

G-BUILD 量法：
  1. bash moonlight-qt/scripts/build-steamframe.sh --arch aarch64 --flavor release --download-only
  2. 乾淨建置：同上改成 --clean（不加 --download-only）；接 AC 電源、不要同時跑串流測試。
  3. 增量：改一個葉節點 .cpp 的註解（或 bump version.txt），再建一次；兩種變更各跑兩次。
     release 要求工作樹乾淨，所以改動要先本地 commit（不必 push）；依賴模組應全部是 Cache hit。
  結果看最後印出的耗時表與 logs/gbuild-history.tsv。增量 > 45 分鐘就停下，交使用者做 U7 決策。
  dev flavor 關掉 app 的 LTO，數字只當參考，不拿來判定 G-BUILD。

原始碼同步（<dev-client> → <builder>）：
  dev（未 commit 的改動，含新檔）：
    <dev-client>:  git add -N <新檔…>              讓新檔出現在 diff 裡（不會 stage 內容）
                   git diff --binary HEAD > m2a.patch
                   scp m2a.patch <user>@<builder>:~/
    <builder>:     cd ~/VipleStream
                   git apply -R ~/last.patch        如果上一份 patch 還套著
                   git apply ~/m2a.patch && cp ~/m2a.patch ~/last.patch
    或把變更的檔案打成 tar 再解到 builder 的 repo。
    不要在 builder 上 git clean -fdx／git stash -u：會連 .deb 建置需要的未追蹤目錄
    Sunshine/third-party/inputtino、wlr-protocols 一起刪掉。
  release（commit SHA 必須和 <dev-client> 完全一致，工作樹乾淨）：
    已 push：<builder> git pull --ff-only
    未 push：<dev-client>:  git bundle create viple.bundle origin/main..main
                            （builder 上沒有 origin/main 那些 commit 時改用完整的：git bundle create viple.bundle main）
                            scp viple.bundle <user>@<builder>:~/
             <builder>:     git bundle verify ~/viple.bundle
                            git fetch ~/viple.bundle main:refs/remotes/bundle/main
                            git merge --ff-only bundle/main
    picoquic submodule 的 commit 必須能從 GitHub（finaltwinsen/picoquic）上的某個 ref 到達（只是「依 SHA
    取得到」不算），腳本會直接問 GitHub，不通過就以 rc 3 結束。
    release 的 manifest 一律寫主 repo 的公開 URL（$VIPLE_URL）＋ HEAD commit，
    /app/manifest.json 就是原始碼參照、不帶 builder 的本機路徑。HEAD 還沒 push 時照常建置（G-BUILD 增量
    量法不受影響）：下載階段用 git 的 url.insteadOf 把這個 URL 導到本機 repo，產物改名 -unpublished、
    不可上 release；push 之後重跑同一條命令（模組全部 cache hit），就會產出正式檔名。
EOF
}

# ---------------------------------------------------------------------------
# 參數
# ---------------------------------------------------------------------------
parse_args()
{
	while [ $# -gt 0 ]; do
		case "$1" in
			--arch)
				[ $# -ge 2 ] || die "$RC_USAGE" "--arch 需要值"
				ARCH=$2; shift 2 ;;
			--arch=*) ARCH=${1#*=}; shift ;;
			--flavor)
				[ $# -ge 2 ] || die "$RC_USAGE" "--flavor 需要值"
				FLAVOR=$2; shift 2 ;;
			--flavor=*) FLAVOR=${1#*=}; shift ;;
			--work)
				[ $# -ge 2 ] || die "$RC_USAGE" "--work 需要值"
				WORK=$2; shift 2 ;;
			--work=*) WORK=${1#*=}; shift ;;
			--jobs)
				[ $# -ge 2 ] || die "$RC_USAGE" "--jobs 需要值"
				JOBS=$2; shift 2 ;;
			--jobs=*) JOBS=${1#*=}; shift ;;
			--clean) DO_CLEAN=1; shift ;;
			--download-only) DOWNLOAD_ONLY=1; shift ;;
			--bundle) DO_BUNDLE=1; shift ;;
			--smoke) DO_SMOKE=1; shift ;;
			--smoke-only) SMOKE_ONLY=1; DO_SMOKE=1; shift ;;
			--check) CHECK_ONLY=1; shift ;;
			-h|--help) usage; exit 0 ;;
			*) usage 1>&2; die "$RC_USAGE" "不認得的參數：$1" ;;
		esac
	done

	case "$ARCH" in
		x86_64|aarch64) ;;
		"") die "$RC_USAGE" "缺少 --arch（x86_64 或 aarch64）" ;;
		*) die "$RC_USAGE" "--arch 只能是 x86_64 或 aarch64（收到 '$ARCH'）" ;;
	esac

	if [ "$CHECK_ONLY" -eq 0 ]; then
		case "$FLAVOR" in
			dev|release) ;;
			"") die "$RC_USAGE" "缺少 --flavor（dev 或 release）" ;;
			*) die "$RC_USAGE" "--flavor 只能是 dev 或 release（收到 '$FLAVOR'）" ;;
		esac
	fi

	if [ "$ARCH" = "x86_64" ] && [ "$FLAVOR" = "release" ]; then
		die "$RC_USAGE" "拒絕 --arch x86_64 --flavor release：x86_64 Flatpak 只給開發用（開 libdrm，NVIDIA 上有 DRM master hook deadlock 風險；見 finish-args.md）"
	fi

	if [ "$SMOKE_ONLY" -eq 1 ] && { [ "$DO_CLEAN" -eq 1 ] || [ "$DOWNLOAD_ONLY" -eq 1 ] || [ "$DO_BUNDLE" -eq 1 ]; }; then
		die "$RC_USAGE" "--smoke-only 不建置，不能和 --clean、--download-only、--bundle 一起用"
	fi

	if [ -n "$JOBS" ]; then
		case "$JOBS" in
			''|*[!0-9]*) die "$RC_USAGE" "--jobs 必須是正整數（收到 '$JOBS'）" ;;
		esac
		[ "$JOBS" -ge 1 ] || die "$RC_USAGE" "--jobs 必須 >= 1"
	fi

	# 工作目錄：絕對路徑、不可是 / 或 $HOME、不可在 repo 內（快照的 git ls-files -o 會把建置產物收進去）。
	case "$WORK" in
		/*) ;;
		*) WORK="$PWD/$WORK" ;;
	esac
	WORK=${WORK%/}
	[ -n "$WORK" ] && [ "$WORK" != "$HOME" ] || die "$RC_USAGE" "--work 不可是 / 或 \$HOME 本身"
	case "$WORK/" in
		"$REPO"/*) die "$RC_USAGE" "--work 不可在 repo（$REPO）底下" ;;
	esac
}

# ---------------------------------------------------------------------------
# 前置檢查：缺什麼就記下來，最後一次印出確切的安裝指令。絕不自動安裝。
# ---------------------------------------------------------------------------
MISSING=()
HINTS=()

add_missing()
{
	MISSING+=("$1")
	shift
	local h e
	for h in "$@"; do
		for e in "${HINTS[@]}"; do
			[ "$e" = "$h" ] && continue 2
		done
		HINTS+=("$h")
	done
}

# $1 = full（完整建置）| download（只下載：runtime、binfmt 缺了只警告）
#    | smoke（--smoke-only：不需要 flatpak-builder 與 SDK，只要 flatpak、Platform runtime、binfmt）
preflight()
{
	local mode=$1
	local host_arch tool ref free_kb df_target
	local -a tools=(flatpak flatpak-builder) refs=(org.kde.Sdk org.kde.Platform)
	host_arch=$(uname -m)
	MISSING=()
	HINTS=()
	if [ "$mode" = "smoke" ]; then
		tools=(flatpak)
		refs=(org.kde.Platform)
	fi

	log "前置檢查：target=$ARCH host=$host_arch mode=$mode"

	command -v git >/dev/null 2>&1 || add_missing "git" "sudo apt install git"
	for tool in "${tools[@]}"; do
		command -v "$tool" >/dev/null 2>&1 || add_missing "$tool" "sudo apt install flatpak flatpak-builder"
	done

	if [ ! -f "$REPO/version.json" ] || [ ! -f "$MQT_DIR/app/version.txt" ] || [ ! -f "$FLATPAK_SRC_DIR/$APP_ID.yml" ]; then
		add_missing "repo 結構（$REPO 應有 version.json、moonlight-qt/app/version.txt 與 manifest）" \
			"確認是從 VipleStream repo 內執行：bash moonlight-qt/scripts/build-steamframe.sh …"
	fi

	if command -v flatpak >/dev/null 2>&1; then
		log "flatpak $(tool_version flatpak)；flatpak-builder $(tool_version flatpak-builder)"
		local want=""
		for ref in "${refs[@]}"; do
			want+=" $ref//$KDE_BRANCH"
		done
		for ref in "${refs[@]}"; do
			# 不指定 --user／--system：user 或 system 裝的都算。
			if ! flatpak info --arch="$ARCH" "$ref//$KDE_BRANCH" >/dev/null 2>&1; then
				if [ "$mode" = "download" ]; then
					log "WARN: $ref//$KDE_BRANCH ($ARCH) 還沒裝；只下載來源不需要，建置前要裝"
				else
					add_missing "$ref//$KDE_BRANCH ($ARCH)" \
						"flatpak remote-add --user --if-not-exists flathub $FLATHUB_REPO_URL" \
						"flatpak install --user -y --arch=$ARCH flathub$want"
				fi
			fi
		done
	fi

	if [ "$ARCH" != "$host_arch" ]; then
		if [ "$ARCH" = "aarch64" ] && [ "$host_arch" = "x86_64" ]; then
			local binfmt=/proc/sys/fs/binfmt_misc/qemu-aarch64
			# F（fix-binary）flag：bwrap 沙箱內看不到 host 的 /usr/bin/qemu-aarch64，必須在註冊時就開檔。
			if [ ! -r "$binfmt" ] || ! grep -q '^enabled' "$binfmt" 2>/dev/null \
				|| ! grep -Eq '^flags:.*F' "$binfmt" 2>/dev/null; then
				if [ "$mode" = "download" ]; then
					log "WARN: qemu-aarch64 binfmt 未就緒（只下載來源不需要）"
				else
					add_missing "qemu-aarch64 binfmt（需 enabled 且 flags 含 F）" \
						"sudo apt install qemu-user qemu-user-binfmt" \
						"cat /proc/sys/fs/binfmt_misc/qemu-aarch64   # 確認 enabled，flags 行含 F"
				fi
			fi
		else
			add_missing "不支援的組合：host=$host_arch target=$ARCH" \
				"在 x86_64 主機上建 x86_64 或 aarch64（qemu）；原生 aarch64 主機只建 aarch64"
		fi
	fi

	# 可用空間（兩個 arch 的 SDK、git 鏡像、ccache、ostree repo 合計數十 GB）
	df_target=$WORK
	[ -d "$df_target" ] || df_target=$HOME
	free_kb=$(df -Pk "$df_target" 2>/dev/null | awk 'NR == 2 { print $4 }')
	if [ -n "$free_kb" ] && [ "$free_kb" -lt $((MIN_FREE_GB * 1024 * 1024)) ]; then
		log "WARN: $df_target 可用空間只剩 $((free_kb / 1024 / 1024)) GB（建議 >= ${MIN_FREE_GB} GB）"
	fi

	# 沙箱實跑：一次驗證 bwrap（AppArmor userns 限制）、binfmt F flag、SDK 可執行。
	if [ "$mode" = "full" ] && [ ${#MISSING[@]} -eq 0 ]; then
		local sb_timeout=120
		[ "$ARCH" != "$host_arch" ] && sb_timeout=300
		if timeout "$sb_timeout" flatpak run --arch="$ARCH" --command=true "org.kde.Sdk//$KDE_BRANCH" >/dev/null 2>&1; then
			log "沙箱實跑 OK（flatpak run --arch=$ARCH --command=true org.kde.Sdk//$KDE_BRANCH）"
		else
			add_missing "沙箱無法執行 org.kde.Sdk//$KDE_BRANCH ($ARCH)" \
				"flatpak run --arch=$ARCH --command=true org.kde.Sdk//$KDE_BRANCH   # 手動重跑看錯誤訊息" \
				"bwrap 被 AppArmor 擋時：確認 /etc/apparmor.d/bwrap-userns-restrict、flatpak profile 已載入（sudo aa-status）" \
				"aarch64：確認 /proc/sys/fs/binfmt_misc/qemu-aarch64 的 flags 含 F"
		fi
	fi

	if [ ${#MISSING[@]} -gt 0 ]; then
		local m h
		log "前置檢查未通過，缺少："
		for m in "${MISSING[@]}"; do
			log "  - $m"
		done
		log "請在 builder 上手動執行（本腳本不會自動安裝）："
		for h in "${HINTS[@]}"; do
			log "  $h"
		done
		return 1
	fi
	log "前置檢查通過"
	return 0
}

# ---------------------------------------------------------------------------
# 版號與來源檢查
# ---------------------------------------------------------------------------
VERSION=""
HEAD_SHA=""
PQ_GITLINK=""

json_num()
{
	sed -n "s/.*\"$1\"[[:space:]]*:[[:space:]]*\([0-9][0-9]*\).*/\1/p" "$REPO/version.json" | head -n 1
}

check_version()
{
	local major minor patch json_ver
	VERSION=$(tr -d '[:space:]' < "$MQT_DIR/app/version.txt")
	[ -n "$VERSION" ] || return 1
	major=$(json_num major)
	minor=$(json_num minor)
	patch=$(json_num patch)
	if [ -z "$major" ] || [ -z "$minor" ] || [ -z "$patch" ]; then
		log "無法解析 $REPO/version.json"
		return 1
	fi
	json_ver="$major.$minor.$patch"
	if [ "$VERSION" != "$json_ver" ]; then
		if [ "$FLAVOR" = "release" ]; then
			log "version.txt=$VERSION 和 version.json=$json_ver 不同（release 要求一致；先跑 build-tools\\version.ps1 propagate）"
			return 1
		fi
		log "WARN: version.txt=$VERSION 和 version.json=$json_ver 不同（dev 允許）"
	fi
	log "版號 $VERSION"
	return 0
}

# commit_on_github <url> <sha>：這個 commit 能不能從 GitHub 上的某個 ref 到達；判定依據印在 stdout。
# flatpak-builder 對只有 commit 的 git 來源，只在 commit 剛好是某個 ref 的 head 時抓那個 ref，否則完整抓
# 所有 ref（"*:*"），不會依 SHA 抓。所以「GitHub 依 SHA 給得出來」不算數（被 force-push 蓋掉的舊 commit、
# fork network 裡別的 fork 的 commit 都給得出來），要確認能從 ref 到達。直接問 GitHub，不看本機可能過期的
# origin/* ref。
commit_on_github()
{
	local url=$1 sha=$2 heads tmp rc
	heads=$(timeout 60 git ls-remote "$url" 2>/dev/null || true)
	if [ -z "$heads" ]; then
		echo "git ls-remote $url 失敗（網路不通？）"
		return 1
	fi
	if grep -q "^$sha[[:space:]]" <<< "$heads"; then
		echo "是 GitHub 上某個分支／tag 的 head"
		return 0
	fi
	# 不是任何 ref 的 head：refspec 對齊 flatpak-builder 的 "*:*"（含 tags、refs/pull/*），只抓 commit 物件
	# （tree:0）。rev-list 不帶 --objects 只走 commit，不會觸發 partial clone 的 lazy fetch（cat-file -e 會，
	# 而 GitHub 依 SHA 給得出來，檢查就又被騙過）。先寫檔再 grep：pipefail 下 grep -q 提早結束會讓
	# rev-list 吃 SIGPIPE，整條管線被誤判成失敗。
	tmp=$(mktemp -d) || return 1
	git init -q --bare "$tmp/repo" \
		&& git -C "$tmp/repo" remote add origin "$url" \
		&& timeout 300 git -C "$tmp/repo" fetch -q --filter=tree:0 origin '+refs/*:refs/*' >/dev/null 2>&1 \
		&& git -C "$tmp/repo" rev-list --all > "$tmp/commits" 2>/dev/null \
		&& grep -qx "$sha" "$tmp/commits"
	rc=$?
	rm -rf -- "$tmp"
	if [ "$rc" -eq 0 ]; then
		echo "可從 GitHub 上的 ref 到達（非 ref head）"
		return 0
	fi
	echo "GitHub 上沒有任何 ref 包含它（依 SHA 取得到也不算），或抓取失敗"
	return 1
}

# picoquic 的 gitlink commit 必須能從 GitHub 上的 ref 到達（manifest 直接引用 GitHub URL，flatpak-builder
# 下載時要抓得到；這裡不過，下載階段才會以 rc 4 失敗、訊息也難懂）。
check_picoquic_public()
{
	local sha=$1 why
	if why=$(commit_on_github "$PICOQUIC_URL" "$sha"); then
		log "picoquic $sha：$why"
		return 0
	fi
	log "picoquic $sha：$why"
	log "flatpak-builder 會抓不到：先 push finaltwinsen/picoquic 的 viplestream-main 再建 release（或網路不通）"
	return 1
}

check_release_source()
{
	local st pq_head
	st=$(git -C "$REPO" status --porcelain -- moonlight-qt Sunshine/third-party/picoquic 2>&1) || {
		log "git status 失敗：$st"
		return 1
	}
	if [ -n "$st" ]; then
		log "release 要求 moonlight-qt 與 picoquic 沒有未 commit 的改動／未追蹤檔："
		printf '%s\n' "$st" | head -n 40
		return 1
	fi
	HEAD_SHA=$(git -C "$REPO" rev-parse HEAD) || return 1
	PQ_GITLINK=$(git -C "$REPO" ls-tree HEAD Sunshine/third-party/picoquic | awk '{ print $3 }')
	[ -n "$PQ_GITLINK" ] || {
		log "HEAD 裡找不到 Sunshine/third-party/picoquic 的 gitlink"
		return 1
	}
	pq_head=$(git -C "$REPO/Sunshine/third-party/picoquic" rev-parse HEAD 2>/dev/null || true)
	if [ "$pq_head" != "$PQ_GITLINK" ]; then
		log "picoquic submodule 的 HEAD（$pq_head）不等於 gitlink（$PQ_GITLINK）：git submodule update 後再建"
		return 1
	fi
	check_picoquic_public "$PQ_GITLINK" || return 1
	# 主 repo：manifest 一律寫公開 URL＋HEAD；這裡只決定下載方式與產物檔名。沒 push 不擋建置
	# （G-BUILD 增量量法是「本地 commit、不 push」），但產物不能用正式檔名。
	local why
	if why=$(commit_on_github "$VIPLE_URL" "$HEAD_SHA"); then
		HEAD_PUBLISHED=1
		log "主 repo $HEAD_SHA：$why"
	else
		HEAD_PUBLISHED=0
		log "WARN: 主 repo HEAD $HEAD_SHA 還不在 GitHub 上（$why）。照常建置：manifest 仍寫 $VIPLE_URL ＋ 這個 commit，"
		log "WARN: 下載階段用 git 的 url.insteadOf 從本機 repo 取；產物命名 -unpublished、不可上 release。"
		log "WARN: push 之後重跑同一條命令（模組全部 cache hit），就會產出正式檔名。"
	fi
	log "release 來源：repo $HEAD_SHA（published=$HEAD_PUBLISHED），picoquic $PQ_GITLINK"
	return 0
}

# ---------------------------------------------------------------------------
# manifest 工作目錄：manifest、gamescope JSON、flavor.txt、SOURCE_INFO、viplestream-source.json、
# dev 快照（src/）全部放在同一棵樹內（--bundle-sources 才帶得到）。
# ---------------------------------------------------------------------------
write_source_info()
{
	local kind=$1 dirty untracked diff_sha pq_head pq_dirty
	dirty=0
	[ -n "$(git -C "$REPO" status --porcelain -- moonlight-qt LICENSE Sunshine/third-party/picoquic 2>/dev/null)" ] && dirty=1
	untracked=$(git -C "$REPO" ls-files -o --exclude-standard -- moonlight-qt 2>/dev/null | wc -l)
	diff_sha=$(git -C "$REPO" diff --binary HEAD -- moonlight-qt LICENSE 2>/dev/null | sha256sum | awk '{ print $1 }')
	pq_head=$(git -C "$REPO/Sunshine/third-party/picoquic" rev-parse HEAD 2>/dev/null || echo unknown)
	pq_dirty=0
	[ -n "$(git -C "$REPO/Sunshine/third-party/picoquic" status --porcelain 2>/dev/null)" ] && pq_dirty=1
	echo "source_kind=$kind"
	echo "flavor=$FLAVOR"
	echo "arch=$ARCH"
	echo "version=$VERSION"
	echo "repo_head=$(git -C "$REPO" rev-parse HEAD 2>/dev/null || echo unknown)"
	echo "repo_describe=$(git -C "$REPO" describe --always --dirty 2>/dev/null || echo unknown)"
	echo "repo_dirty=$dirty"
	echo "repo_untracked_in_moonlight_qt=$untracked"
	echo "repo_tracked_diff_sha256=$diff_sha"
	echo "picoquic_gitlink=$(git -C "$REPO" ls-tree HEAD Sunshine/third-party/picoquic 2>/dev/null | awk '{ print $3 }')"
	echo "picoquic_head=$pq_head"
	echo "picoquic_dirty=$pq_dirty"
	echo "generated_by=build-steamframe.sh"
	# release 的來源已 pin 住，SOURCE_INFO 不放時間戳：內容不變時 viplestream 模組才能命中快取
	# （建置時間由 build-viplestream.sh 另外寫進 /app/share/VipleStream/SOURCE_INFO）。
	if [ "$kind" = "snapshot" ]; then
		echo "generated_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
	fi
}

# copy_listed <git 工作樹> <目的地> [pathspec…]：複製 git 追蹤的檔案＋未追蹤但未被忽略的檔案。
# git ls-files -c 會列出工作樹已刪除的檔案，先過濾掉（否則 tar 失敗）。
copy_listed()
{
	local src=$1 dst=$2
	shift 2
	local raw="$TMP_DIR/ls-raw" lst="$TMP_DIR/ls-filtered" f n=0
	local -a rcs
	git -C "$src" ls-files -z -co --exclude-standard -- "$@" > "$raw" || return 1
	: > "$lst"
	while IFS= read -r -d '' f; do
		if [ -e "$src/$f" ] || [ -L "$src/$f" ]; then
			printf '%s\0' "$f" >> "$lst"
			n=$((n + 1))
		fi
	done < "$raw"
	mkdir -p "$dst" || return 1
	tar -C "$src" --null --no-recursion -T "$lst" -cf - | tar -C "$dst" -xf -
	rcs=("${PIPESTATUS[@]}")
	if [ "${rcs[0]}" -ne 0 ] || [ "${rcs[1]}" -ne 0 ]; then
		log "複製快照失敗（tar rc=${rcs[*]}）：$src"
		return 1
	fi
	log "快照：$src → $n 個檔案"
	return 0
}

make_snapshot()
{
	local dst="$MANIFEST_DIR/src"
	rm -rf -- "$dst"
	# 主 repo：moonlight-qt 子樹＋根目錄 LICENSE（flatpak-builder 會自動收模組根目錄的授權檔）。
	copy_listed "$REPO" "$dst" moonlight-qt LICENSE || return 1
	# picoquic submodule（它的 .gitignore 排除了 /build/，builder 上舊的建置產物不會帶進沙箱）。
	copy_listed "$REPO/Sunshine/third-party/picoquic" "$dst/Sunshine/third-party/picoquic" || return 1
	log "快照大小：$(du -sh "$dst" 2>/dev/null | awk '{ print $1 }')"
	return 0
}

prepare_manifest()
{
	rm -rf -- "$MANIFEST_DIR"
	mkdir -p "$MANIFEST_DIR" || return 1
	cp -- "$FLATPAK_SRC_DIR/$APP_ID.yml" "$MANIFEST_DIR/" || return 1
	cp -R -- "$FLATPAK_SRC_DIR/gamescope-wsi" "$MANIFEST_DIR/" || return 1
	printf '%s\n' "$FLAVOR" > "$MANIFEST_DIR/flavor.txt" || return 1

	if [ "$FLAVOR" = "dev" ]; then
		write_source_info snapshot > "$MANIFEST_DIR/SOURCE_INFO" || return 1
		make_snapshot || return 1
		cat > "$MANIFEST_DIR/viplestream-source.json" <<'EOF' || return 1
[
  { "type": "dir", "path": "src" },
  { "type": "file", "path": "flavor.txt" },
  { "type": "file", "path": "SOURCE_INFO" }
]
EOF
	else
		write_source_info git > "$MANIFEST_DIR/SOURCE_INFO" || return 1
		# 主 repo 一律寫公開 URL：flatpak-builder 會把展開後的 manifest 寫進 /app/manifest.json，
		# file://<本機路徑> 會把 builder 的帳號與路徑帶進要發佈的 bundle，也不是可取得的原始碼參照。
		# HEAD 還沒 push 時，下載階段由 url.insteadOf 導到本機 repo（見 main 的 FB_ENV），manifest 不變。
		{
			printf '[\n'
			printf '  { "type": "git", "url": "%s", "commit": "%s", "disable-submodules": true, "disable-shallow-clone": true },\n' \
				"$VIPLE_URL" "$HEAD_SHA"
			printf '  { "type": "git", "url": "%s", "commit": "%s", "dest": "Sunshine/third-party/picoquic" },\n' \
				"$PICOQUIC_URL" "$PQ_GITLINK"
			printf '  { "type": "file", "path": "flavor.txt" },\n'
			printf '  { "type": "file", "path": "SOURCE_INFO" }\n'
			printf ']\n'
		} > "$MANIFEST_DIR/viplestream-source.json" || return 1
	fi
	log "manifest 工作目錄：$MANIFEST_DIR（flavor=$FLAVOR）"
	return 0
}

# ---------------------------------------------------------------------------
# flatpak-builder
# ---------------------------------------------------------------------------
# run_fb <phase> <flatpak-builder 參數…>：輸出加時間戳 tee 到 log，回傳 flatpak-builder 本身的 rc。
run_fb()
{
	local phase=$1
	shift
	local fb_log="$LOG_DIR/fb-$ARCH-$FLAVOR-$phase-$TS.log"
	local -a rcs
	log "flatpak-builder（$phase）：flatpak-builder $*"
	[ ${#FB_ENV[@]} -gt 0 ] && log "flatpak-builder 額外環境：${FB_ENV[*]}"
	log "flatpak-builder 輸出：$fb_log"
	FB_LOG=$fb_log
	( cd "$WORK" && env ${FB_ENV[@]+"${FB_ENV[@]}"} flatpak-builder "$@" ) 2>&1 | ts_filter | tee -a "$fb_log"
	rcs=("${PIPESTATUS[@]}")
	return "${rcs[0]}"
}

# 從加了時間戳的 flatpak-builder log 推算各階段耗時。
# $1=log $2=結束 epoch $3=輸出 tsv
module_times()
{
	awk -v end="$2" '
	{
		ts = $1
		line = substr($0, length($1) + 2)
		kind = ""; name = ""
		if (line ~ /^Cache hit for cleanup/ || line ~ /^Cleaning up$/) { kind = "phase"; name = "cleanup" }
		else if (line ~ /^Cache hit for finish/ || line ~ /^Finishing app$/) { kind = "phase"; name = "finish" }
		else if (line ~ /^Building module /) {
			kind = "built"; name = line
			sub(/^Building module /, "", name); sub(/ in .*$/, "", name)
		}
		else if (line ~ /^Cache hit for .*, skipping build$/) {
			kind = "cache-hit"; name = line
			sub(/^Cache hit for /, "", name); sub(/, skipping build$/, "", name)
		}
		else if (line ~ /^Exporting .* to repo$/) {
			kind = "phase"; name = line
			sub(/^Exporting /, "export ", name); sub(/ to repo$/, "", name)
		}
		else if (line ~ /^Starting build of /) { kind = "phase"; name = "prepare" }
		if (kind != "") { n++; K[n] = kind; N[n] = name; T[n] = ts }
	}
	END {
		printf "module\tstate\tseconds\n"
		for (i = 1; i <= n; i++) {
			stop = (i < n) ? T[i + 1] : end
			printf "%s\t%s\t%d\n", N[i], K[i], stop - T[i]
		}
	}' "$1" > "$3"
}

print_module_times()
{
	local tsv=$1
	log "各階段耗時（秒）："
	while IFS=$'\t' read -r name state secs; do
		[ "$name" = "module" ] && continue
		log "$(printf '  %-44s %-10s %8s' "$name" "$state" "$secs")"
	done < "$tsv"
}

# ---------------------------------------------------------------------------
# smoke test
# ---------------------------------------------------------------------------
SMOKE_FAIL=0
SMOKE_SUMMARY=()

smoke_env_prepare()
{
	if [ -z "${XDG_RUNTIME_DIR:-}" ] && [ -d "/run/user/$(id -u)" ]; then
		XDG_RUNTIME_DIR="/run/user/$(id -u)"
		export XDG_RUNTIME_DIR
	fi
	# SSH 下 flatpak run 需要 session bus（portal、document portal）；GNOME 有登入時 socket 在這裡。
	if [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ] && [ -n "${XDG_RUNTIME_DIR:-}" ] && [ -S "$XDG_RUNTIME_DIR/bus" ]; then
		export DBUS_SESSION_BUS_ADDRESS="unix:path=$XDG_RUNTIME_DIR/bus"
	fi
}

# smoke_case <label> <expect_version:0|1> <命令…>
smoke_case()
{
	local label=$1 expect_version=$2
	shift 2
	local out="$LOG_DIR/smoke-$ARCH-$FLAVOR-$TS-$label.txt" rc
	timeout --kill-after=15 "$SMOKE_TIMEOUT" "$@" > "$out" 2>&1
	rc=$?
	if [ "$rc" -eq 0 ] && [ "$expect_version" -eq 1 ] && ! grep -qF "$VERSION" "$out"; then
		log "smoke $label：rc=0 但輸出沒有版號 $VERSION（$out）"
		rc=99
	fi
	if [ "$rc" -eq 0 ]; then
		SMOKE_SUMMARY+=("$label=ok")
	else
		SMOKE_SUMMARY+=("$label=rc$rc")
		SMOKE_FAIL=1
		log "smoke $label 失敗 rc=$rc，輸出末段："
		tail -n 20 "$out" | while IFS= read -r l; do log "    $l"; done
	fi
	log "smoke $label rc=$rc（$out）"
}

# 對已安裝的 ref 跑一輪：GUI 環境（有 Wayland socket 或 DISPLAY 時）＋純 SSH 環境。
smoke_round()
{
	local tag=$1 ref="$APP_ID//$BRANCH"
	local -a run_gui run_ssh
	local wl=${WAYLAND_DISPLAY:-wayland-0}

	run_ssh=(env -u WAYLAND_DISPLAY -u DISPLAY
		flatpak run --arch="$ARCH" --nosocket=wayland --nosocket=fallback-x11 --nosocket=x11 "$ref")
	if [ -n "${XDG_RUNTIME_DIR:-}" ] && [ -S "$XDG_RUNTIME_DIR/$wl" ]; then
		run_gui=(env "WAYLAND_DISPLAY=$wl" flatpak run --arch="$ARCH" "$ref")
	elif [ -n "${DISPLAY:-}" ]; then
		run_gui=(flatpak run --arch="$ARCH" "$ref")
	else
		run_gui=()
	fi

	if [ ${#run_gui[@]} -gt 0 ]; then
		smoke_case "$tag-gui-help1" 0 "${run_gui[@]}" --help
		smoke_case "$tag-gui-help2" 0 "${run_gui[@]}" --help
		smoke_case "$tag-gui-version" 1 "${run_gui[@]}" --version
	else
		SMOKE_SUMMARY+=("$tag-gui=SKIPPED")
		log "WARN: smoke $tag：沒有 GUI session（找不到 \$XDG_RUNTIME_DIR/$wl，也沒有 DISPLAY），GUI 環境這輪略過；有人登入桌面後請重跑 --smoke"
	fi
	smoke_case "$tag-ssh-help1" 0 "${run_ssh[@]}" --help
	smoke_case "$tag-ssh-help2" 0 "${run_ssh[@]}" --help
	smoke_case "$tag-ssh-version" 1 "${run_ssh[@]}" --version
}

# --smoke-only：repo 裡的建置可能比現在的 version.txt 舊（或新），--version 以安裝進來的 SOURCE_INFO 為準。
smoke_only_version()
{
	local loc v
	loc=$(flatpak --user info --show-location --arch="$ARCH" "$APP_ID//$BRANCH" 2>/dev/null) || loc=""
	v=""
	[ -n "$loc" ] && v=$(sed -n 's/^version=//p' "$loc/files/share/VipleStream/SOURCE_INFO" 2>/dev/null | head -n 1)
	if [ -z "$v" ]; then
		log "WARN: --smoke-only：讀不到安裝的 SOURCE_INFO（${loc:-找不到安裝位置}），--version 以 version.txt 的 $VERSION 核對"
	elif [ "$v" != "$VERSION" ]; then
		log "WARN: --smoke-only：repo 裡的建置是 $v，version.txt 現在是 $VERSION；--version 以 $v 核對"
		VERSION=$v
	else
		log "--smoke-only：repo 裡的建置版號 $v（SOURCE_INFO）"
	fi
	return 0
}

# --smoke-only 的 release：out/ 裡同版號的 bundle，正式與 -unpublished 都有時取較新的；沒有就印空字串。
find_release_bundle()
{
	local f best=""
	for f in "$OUT_DIR/$(release_bundle_name 1)" "$OUT_DIR/$(release_bundle_name 0)"; do
		[ -f "$f" ] || continue
		if [ -z "$best" ] || [ "$f" -nt "$best" ]; then
			best=$f
		fi
	done
	printf '%s' "$best"
}

do_smoke()
{
	local host_arch
	host_arch=$(uname -m)
	SMOKE_TIMEOUT=180
	[ "$ARCH" != "$host_arch" ] && SMOKE_TIMEOUT=900   # qemu 下 Qt 初始化很慢
	smoke_env_prepare

	log "smoke：本地 remote $LOCAL_REMOTE → $REPO_DIR（--user，--no-gpg-verify）"
	flatpak --user remote-add --if-not-exists --no-gpg-verify "$LOCAL_REMOTE" "$REPO_DIR" >> "$MAIN_LOG" 2>&1 \
		|| { log "remote-add 失敗"; return 1; }
	flatpak --user remote-modify --url="file://$REPO_DIR" "$LOCAL_REMOTE" >> "$MAIN_LOG" 2>&1 \
		|| { log "remote-modify 失敗"; return 1; }
	flatpak --user install -y --noninteractive --reinstall --arch="$ARCH" "$LOCAL_REMOTE" "$APP_ID//$BRANCH" >> "$MAIN_LOG" 2>&1 \
		|| { log "從 $LOCAL_REMOTE 安裝 $APP_ID//$BRANCH 失敗（細節在 $MAIN_LOG）"; return 1; }
	if [ "$SMOKE_ONLY" -eq 1 ]; then
		smoke_only_version
	fi
	smoke_round repo

	if [ "$FLAVOR" = "release" ] && [ "$SMOKE_ONLY" -eq 1 ]; then
		OUT_FILE=$(find_release_bundle)
		if [ -z "$OUT_FILE" ]; then
			SMOKE_SUMMARY+=("bundle=SKIPPED")
			log "WARN: --smoke-only：$OUT_DIR 沒有 $VERSION 的 release bundle，bundle 那一輪略過"
		fi
	fi
	if [ "$FLAVOR" = "release" ] && [ -n "$OUT_FILE" ] && [ -f "$OUT_FILE" ]; then
		log "smoke：安裝產出的 bundle $OUT_FILE"
		flatpak --user install -y --noninteractive --reinstall --bundle "$OUT_FILE" >> "$MAIN_LOG" 2>&1 \
			|| { log "flatpak install --bundle 失敗（細節在 $MAIN_LOG）"; return 1; }
		smoke_round bundle
	fi

	log "smoke 結果：${SMOKE_SUMMARY[*]}"
	[ "$SMOKE_FAIL" -eq 0 ]
}

# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------
parse_args "$@"

if [ "$CHECK_ONLY" -eq 1 ]; then
	if preflight full; then
		exit 0
	fi
	exit "$RC_PREREQ"
fi

mkdir -p "$WORK" || die "$RC_USAGE" "無法建立 $WORK"
WORK=$(cd "$WORK" && pwd -P) || die "$RC_USAGE" "無法進入 $WORK"
# 同一個 --work 同時只能有一個 build-steamframe：state、manifest/、repo/ 兩個 arch 共用，第二次執行的
# --clean 與 prepare_manifest（rm -rf manifest/）會改掉第一次執行中 flatpak-builder 還沒讀到的
# flavor.txt、SOURCE_INFO、src/，flatpak-builder 本身也沒有 state-dir 鎖。fd 9 會被 flatpak-builder
# 等子行程繼承：腳本被砍掉但子行程還在跑時鎖會繼續保留；fd 全部關閉時自動釋放，不會留下過期的鎖。
command -v flock >/dev/null 2>&1 || die "$RC_PREREQ" "缺少 flock（util-linux）：sudo apt install util-linux"
exec 9>"$WORK/.build-steamframe.lock" || die "$RC_USAGE" "無法建立 $WORK/.build-steamframe.lock"
if ! flock -n 9; then
	die "$RC_USAGE" "另一個 build-steamframe 正在使用 $WORK（state、manifest、repo 兩個 arch 共用；請等它結束再跑，或改用不同的 --work）"
fi
LOG_DIR="$WORK/logs"
STATE_DIR="$WORK/state"
BUILD_DIR="$WORK/build-$ARCH"
REPO_DIR="$WORK/repo"
OUT_DIR="$WORK/out"
MANIFEST_DIR="$WORK/manifest"
MANIFEST_FILE="$MANIFEST_DIR/$APP_ID.yml"
TMP_DIR="$WORK/tmp"
mkdir -p "$LOG_DIR" "$OUT_DIR" "$TMP_DIR" || die "$RC_USAGE" "無法建立 $WORK 底下的目錄"
MAIN_LOG="$LOG_DIR/steamframe-$ARCH-$FLAVOR-$TS.log"
OUT_FILE=""
FB_LOG=""
if [ "$FLAVOR" = "release" ]; then
	BRANCH="stable"
else
	BRANCH="dev"
fi

log "build-steamframe：arch=$ARCH flavor=$FLAVOR branch=$BRANCH work=$WORK jobs=${JOBS:-auto} clean=$DO_CLEAN download_only=$DOWNLOAD_ONLY bundle=$DO_BUNDLE smoke=$DO_SMOKE smoke_only=$SMOKE_ONLY"
log "repo=$REPO"

if [ "$DOWNLOAD_ONLY" -eq 1 ]; then
	preflight download || exit "$RC_PREREQ"
elif [ "$SMOKE_ONLY" -eq 1 ]; then
	preflight smoke || exit "$RC_PREREQ"
else
	preflight full || exit "$RC_PREREQ"
fi

check_version || die "$RC_SOURCE" "版號檢查失敗"

# --smoke-only：不碰來源（不跑 release 來源檢查、不動 manifest/）、不下載、不建置，只對 repo/ 現有的 ref 跑 smoke。
if [ "$SMOKE_ONLY" -eq 1 ]; then
	[ -f "$REPO_DIR/config" ] || die "$RC_PREREQ" "--smoke-only：$REPO_DIR 還不是 ostree repo（先完整建置一次 --arch $ARCH --flavor $FLAVOR）"
	log "--smoke-only：不準備來源、不建置，直接拿 $REPO_DIR 裡現有的 $APP_ID//$BRANCH（$ARCH）跑 smoke"
	do_smoke || die "$RC_SMOKE" "smoke test 失敗（${SMOKE_SUMMARY[*]}）"
	log "完成（--smoke-only）：arch=$ARCH flavor=$FLAVOR 版號 $VERSION${OUT_FILE:+；bundle $OUT_FILE}"
	log "記錄：$MAIN_LOG"
	exit 0
fi

if [ "$FLAVOR" = "release" ]; then
	check_release_source || die "$RC_SOURCE" "release 來源檢查失敗"
fi

if [ "$DO_CLEAN" -eq 1 ]; then
	for d in "$STATE_DIR/cache" "$STATE_DIR/ccache" "$STATE_DIR/build" "$STATE_DIR/checksums" "$BUILD_DIR"; do
		[ -e "$d" ] || continue
		log "--clean：刪除 $d"
		rm -rf -- "$d" || die "$RC_BUILD" "無法刪除 $d"
	done
fi

prepare_manifest || die "$RC_SOURCE" "準備 manifest 工作目錄失敗"

# manifest 解析檢查（YAML 支援、字串來源 include、語法）；不需要建置目錄。
if ! flatpak-builder --show-manifest "$MANIFEST_FILE" > "$LOG_DIR/resolved-manifest-$ARCH-$FLAVOR-$TS.json" 2>> "$MAIN_LOG"; then
	die "$RC_SOURCE" "flatpak-builder 無法解析 $MANIFEST_FILE（細節在 $MAIN_LOG；flatpak-builder 沒有 YAML 支援時會在這裡失敗）"
fi

# --disable-rofiles-fuse：finish 階段的 rofiles-fuse 掛載點偶發卸載不掉（fusermount: Device or resource busy
# → Failure spawning rofiles-fuse，2026-09-28／29 兩次），整次建置白費；不用 rofiles 只是少一層對 cache
# 的防寫保護（CI 常見做法）。
FB_COMMON=(--arch="$ARCH" --state-dir="$STATE_DIR" --default-branch="$BRANCH" --force-clean --disable-rofiles-fuse)
[ -n "$JOBS" ] && FB_COMMON+=(--jobs="$JOBS")

# 下載階段（不計時）。--disable-updates：已經有的 commit 不再連網更新，只補缺的。
DL_ARGS=("${FB_COMMON[@]}" --download-only --disable-updates)
[ "$DOWNLOAD_ONLY" -eq 1 ] && DL_ARGS+=(--allow-missing-runtimes)
# release 的 HEAD 還沒 push：manifest 仍寫公開 URL，只在這次下載讓 git 把它改寫成本機 repo
# （GIT_CONFIG_COUNT 形式的 url.<本機>.insteadOf，只影響這個 flatpak-builder 的 git 子行程）。
# flatpak-builder 的鏡像目錄與模組 checksum 都依 manifest 的 URL＋commit 計算，push 後重跑時鏡像裡已有
# 這個 commit（不再連網），模組也全部 cache hit。
FB_ENV=()
if [ "$FLAVOR" = "release" ] && [ "$HEAD_PUBLISHED" -eq 0 ]; then
	gi=${GIT_CONFIG_COUNT:-0}
	case "$gi" in
		''|*[!0-9]*) gi=0 ;;
	esac
	FB_ENV=("GIT_CONFIG_COUNT=$((gi + 1))" "GIT_CONFIG_KEY_$gi=url.file://$REPO.insteadOf" "GIT_CONFIG_VALUE_$gi=$VIPLE_URL")
	# 自檢：git 要認得 GIT_CONFIG_COUNT（>= 2.31），ls-remote 公開 URL 時要看到本機的 HEAD。
	lsr=$(env "${FB_ENV[@]}" timeout 60 git ls-remote "$VIPLE_URL" 2>&1)
	grep -q "^$HEAD_SHA[[:space:]]" <<< "$lsr" \
		|| die "$RC_SOURCE" "url.insteadOf 沒有生效：git ls-remote $VIPLE_URL 看不到本機 HEAD $HEAD_SHA（git 需要 >= 2.31）"
	log "HEAD 未公開：下載階段以 url.insteadOf 把 $VIPLE_URL 導到 file://$REPO（manifest 不變）"
fi
DL_START=$(date +%s)
run_fb download "${DL_ARGS[@]}" "$BUILD_DIR" "$MANIFEST_FILE"
rc=$?
FB_ENV=()
[ "$rc" -eq 0 ] || die "$RC_BUILD" "下載來源失敗（flatpak-builder rc=$rc，$FB_LOG）"
log "下載階段 $(( $(date +%s) - DL_START )) 秒（不計入 G-BUILD）"

if [ "$DOWNLOAD_ONLY" -eq 1 ]; then
	log "--download-only 完成"
	exit 0
fi

# 建置階段（計時）
BUILD_ARGS=("${FB_COMMON[@]}" --ccache --repo="$REPO_DIR" --disable-download --disable-updates)
BUILD_START=$(date +%s)
run_fb build "${BUILD_ARGS[@]}" "$BUILD_DIR" "$MANIFEST_FILE"
rc=$?
BUILD_END=$(date +%s)
BUILD_SECS=$((BUILD_END - BUILD_START))

GBUILD_TSV="$LOG_DIR/gbuild-$ARCH-$FLAVOR-$TS.tsv"
module_times "$FB_LOG" "$BUILD_END" "$GBUILD_TSV"
print_module_times "$GBUILD_TSV"
VS_SECS=$(awk -F'\t' '$1 == "viplestream" { s = $3 } END { print (s == "" ? "-" : s) }' "$GBUILD_TSV")
HIST="$LOG_DIR/gbuild-history.tsv"
[ -f "$HIST" ] || printf 'time\tarch\tflavor\tclean\tjobs\trc\ttotal_s\tviplestream_s\thead\n' > "$HIST"
printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$(date +%Y-%m-%dT%H:%M:%S%z)" "$ARCH" "$FLAVOR" "$DO_CLEAN" \
	"${JOBS:-auto}" "$rc" "$BUILD_SECS" "$VS_SECS" "$(git -C "$REPO" rev-parse --short HEAD 2>/dev/null)" >> "$HIST"
log "建置階段總耗時 ${BUILD_SECS} 秒（viplestream 模組 ${VS_SECS} 秒；clean=$DO_CLEAN）→ $GBUILD_TSV"

[ "$rc" -eq 0 ] || die "$RC_BUILD" "flatpak-builder 失敗（rc=$rc，$FB_LOG）"

# bundle
if [ "$FLAVOR" = "release" ] || [ "$DO_BUNDLE" -eq 1 ]; then
	if [ "$FLAVOR" = "release" ]; then
		OUT_NAME=$(release_bundle_name "$HEAD_PUBLISHED")
	else
		OUT_NAME="VipleStream-Client-$VERSION-linux-$(bundle_arch_name "$ARCH")-dev.flatpak"
	fi
	OUT_FILE="$OUT_DIR/$OUT_NAME"
	rm -f -- "$OUT_FILE" "$OUT_FILE.sha256"
	log "bundle：$OUT_FILE"
	BUNDLE_START=$(date +%s)
	if ! flatpak build-bundle --arch="$ARCH" --runtime-repo="$FLATHUB_REPO_URL" \
		"$REPO_DIR" "$OUT_FILE" "$APP_ID" "$BRANCH" >> "$MAIN_LOG" 2>&1; then
		die "$RC_BUNDLE" "flatpak build-bundle 失敗（細節在 $MAIN_LOG）"
	fi
	( cd "$OUT_DIR" && sha256sum "$OUT_NAME" > "$OUT_NAME.sha256" ) || die "$RC_BUNDLE" "sha256sum 失敗"
	log "bundle 完成（$(( $(date +%s) - BUNDLE_START )) 秒，G-BUILD 要一併記錄）：$OUT_FILE（$(du -h "$OUT_FILE" | awk '{ print $1 }')）sha256 $(awk '{ print $1 }' "$OUT_FILE.sha256")"
	if [ "$FLAVOR" = "release" ] && [ "$HEAD_PUBLISHED" -eq 0 ]; then
		log "WARN: HEAD 還不在 GitHub 上，這是 -unpublished 產物，不可上 release；push 後重跑同一條命令產出正式檔名"
	fi
fi

# smoke
if [ "$DO_SMOKE" -eq 1 ]; then
	do_smoke || die "$RC_SMOKE" "smoke test 失敗（${SMOKE_SUMMARY[*]}）"
fi

log "完成：arch=$ARCH flavor=$FLAVOR 版號 $VERSION；建置 ${BUILD_SECS} 秒${OUT_FILE:+；產物 $OUT_FILE}"
log "記錄：$MAIN_LOG"
exit 0
