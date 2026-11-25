#!/bin/bash
# 把 install-steamframe.sh 與該版 .flatpak 包成單一的 VipleStream-SteamFrame-<版>.run（§FRAME-RUN）。
# 為什麼不用 makeself：Steam Frame 上沒有，WSL 與樹莓派也不保證有。只用 bash、coreutils、tar、sha256sum。
set -euo pipefail

die() { echo "error: $*" >&2; exit 1; }

usage() {
    cat <<'EOF'
usage: make-frame-run.sh <flatpak> <version> <out-dir>

  產出 <out-dir>/VipleStream-SteamFrame-<version>.run
  flatpak 檔名須為 VipleStream-Client-<version>-linux-*.flatpak。
  檔名含 -unpublished 時，產出改為 …-<version>-unpublished.run（正式檔名會被頭盔的更新規則接受）。
EOF
}

if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
    usage
    exit 0
fi
[ $# -eq 3 ] || die "usage: make-frame-run.sh <flatpak> <version> <out-dir>"

flatpak=$1
version=$2
out_dir_arg=$3

# 三個數字：app 的 asset 規則只接受 X.Y.Z，多一段或少一段做得出檔、通知卻認不得。
ver_rest=$version
nparts=0
while [ -n "$ver_rest" ]; do
    part=${ver_rest%%.*}
    case "$part" in
        ''|*[!0-9]*) die "version must be X.Y.Z (got $version)" ;;
    esac
    nparts=$((nparts + 1))
    case "$ver_rest" in
        *.*) ver_rest=${ver_rest#*.} ;;
        *) ver_rest='' ;;
    esac
done
[ "$nparts" -eq 3 ] || die "version must be X.Y.Z (got $version)"

[ -f "$flatpak" ] || die "flatpak not found: $flatpak"
[ -r "$flatpak" ] || die "flatpak not readable: $flatpak"
command -v tar >/dev/null || die "tar not found"
command -v sha256sum >/dev/null || die "sha256sum not found"
command -v mktemp >/dev/null || die "mktemp not found"

flat_base=$(basename -- "$flatpak")
case "$flat_base" in
    "VipleStream-Client-${version}-linux-"*.flatpak) ;;
    *) die "flatpak must be named VipleStream-Client-${version}-linux-*.flatpak (got $flat_base)" ;;
esac
# 檔名會寫進標頭的單引號字串。奇怪的字元會把標頭弄壞，或讓 tar 把參數當成選項。
case "$flat_base" in
    *[!A-Za-z0-9._-]*) die "unsafe flatpak name: $flat_base" ;;
esac

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
installer="$script_dir/install-steamframe.sh"
[ -f "$installer" ] || die "missing $installer"

out_dir=$(cd "$out_dir_arg" && pwd) || die "cannot enter $out_dir_arg"
[ -d "$out_dir" ] || die "not a directory: $out_dir_arg"

# §FRAME-RUN：正式檔名會被頭盔自動更新與 app 的通知規則接受。還沒 push 的 flatpak 不能用那個名字。
run_base="VipleStream-SteamFrame-${version}.run"
case "$flat_base" in
    *-unpublished.flatpak) run_base="VipleStream-SteamFrame-${version}-unpublished.run" ;;
esac
out_path="$out_dir/$run_base"

work=$(mktemp -d "${TMPDIR:-/tmp}/make-frame-run.XXXXXXXX") || die "mktemp failed"
tmp_out=""
cleanup() {
    rm -rf -- "$work"
    if [ -n "${tmp_out:-}" ] && [ -f "$tmp_out" ]; then
        rm -f -- "$tmp_out"
    fi
}
trap cleanup EXIT
trap 'cleanup || true; exit 130' INT
trap 'cleanup || true; exit 143' TERM

stage="$work/stage"
mkdir -p "$stage"
cp -f -- "$installer" "$stage/install-steamframe.sh"
chmod 755 "$stage/install-steamframe.sh"
cp -f -- "$flatpak" "$stage/$flat_base"

payload="$work/payload.tar.gz"
tar --format=ustar -czf "$payload" -C "$stage" -- install-steamframe.sh "$flat_base"

# 清單必須是這兩個檔名本身。tar 若加上 ./ 前綴，頭盔上的 .run 會找不到。
tar -tzf "$payload" > "$work/list"
ok_inst=0
ok_flat=0
while IFS= read -r line || [ -n "$line" ]; do
    line=${line#./}
    [ "$line" = "install-steamframe.sh" ] && ok_inst=1
    [ "$line" = "$flat_base" ] && ok_flat=1
done < "$work/list"
[ "$ok_inst" = 1 ] && [ "$ok_flat" = 1 ] || die "tar contents unexpected"

payload_sha=$(sha256sum "$payload") || die "sha256sum failed"
payload_sha=${payload_sha%% *}
case "$payload_sha" in
    *[!0-9a-f]*|'') die "bad payload sha256" ;;
esac
[ "${#payload_sha}" -eq 64 ] || die "bad payload sha256"

# 標頭用 LF。CRLF 會讓頭盔上的 bash 把選項當成帶 \r 的命令；這裡再剝一次，避免產生器自己被存成 CRLF 時標頭跟著壞。
raw="$work/header.raw"
cat > "$raw" <<'END_HEADER'
#!/bin/bash
# VipleStream Steam Frame 單一安裝檔（§FRAME-RUN）。
# 為什麼先驗 sha256：雜湊是建置時對標記行之後的 tar.gz 算的，對不上就不要執行裡面的腳本。
set -euo pipefail

PAYLOAD_SHA256='@@SHA256@@'
FLATPAK_NAME='@@FLATPAK@@'
PAYLOAD_LINE=@@LINE@@

die() { echo "error: $*" >&2; exit 1; }

usage() {
    cat <<'EOF'
VipleStream Steam Frame 單一安裝檔

  bash VipleStream-SteamFrame-X.Y.Z.run [--no-steam] [--uninstall]
  bash VipleStream-SteamFrame-X.Y.Z.run --extract <目錄>
  bash VipleStream-SteamFrame-X.Y.Z.run --help

不需要 root、不需要網路。其它參數交給裡面的 install-steamframe.sh，
後面再接解出來的 .flatpak。結束碼與安裝腳本相同。
EOF
}

EXTRACT_DIR=""
PASS=()
while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help)
            usage
            exit 0
            ;;
        --extract)
            [ $# -ge 2 ] || die "--extract needs a directory"
            EXTRACT_DIR=$2
            shift 2
            ;;
        --extract=*)
            EXTRACT_DIR=${1#*=}
            [ -n "$EXTRACT_DIR" ] || die "--extract needs a directory"
            shift
            ;;
        *)
            PASS+=("$1")
            shift
            ;;
    esac
done

if [ -n "$EXTRACT_DIR" ] && [ ${#PASS[@]} -gt 0 ]; then
    die "--extract does not take installer arguments"
fi

command -v sha256sum >/dev/null || die "sha256sum not found"
command -v tar >/dev/null || die "tar not found"
[ -f "$0" ] || die "cannot read $0"
case "$PAYLOAD_SHA256" in
    *[!0-9a-f]*|'') die "installer header has no payload sha256" ;;
esac
[ "${#PAYLOAD_SHA256}" -eq 64 ] || die "installer header has no payload sha256"
case "$PAYLOAD_LINE" in
    ''|*[!0-9]*) die "bad payload offset" ;;
esac

work=$(mktemp -d "${TMPDIR:-/tmp}/viplestream-frame-run.XXXXXXXX") || die "mktemp failed"
# §FRAME-RUN：中斷時也清暫存。只掛 EXIT 的話，Ctrl-C 在有些 bash 不會跑。
cleanup() { rm -rf -- "$work"; }
trap cleanup EXIT
trap 'cleanup || true; exit 130' INT
trap 'cleanup || true; exit 143' TERM

tail -n +"$PAYLOAD_LINE" -- "$0" > "$work/payload.tar.gz" || die "cannot read payload"
got=$(sha256sum "$work/payload.tar.gz") || die "sha256sum failed"
got=${got%% *}
[ "$got" = "$PAYLOAD_SHA256" ] || die "payload sha256 mismatch"

tar -xzf "$work/payload.tar.gz" -C "$work" || die "cannot unpack payload"
rm -f -- "$work/payload.tar.gz"

inst="$work/install-steamframe.sh"
bundle="$work/$FLATPAK_NAME"
[ -f "$inst" ] || die "archive is missing install-steamframe.sh"
[ -f "$bundle" ] || die "archive is missing $FLATPAK_NAME"

if [ -n "$EXTRACT_DIR" ]; then
    mkdir -p -- "$EXTRACT_DIR" || die "cannot create $EXTRACT_DIR"
    cp -f -- "$inst" "$bundle" "$EXTRACT_DIR/" || die "cannot extract to $EXTRACT_DIR"
    echo "[viplestream] extracted to $EXTRACT_DIR"
    exit 0
fi

rc=0
if [ ${#PASS[@]} -eq 0 ]; then
    bash "$inst" "$bundle" || rc=$?
else
    bash "$inst" "${PASS[@]}" "$bundle" || rc=$?
fi
exit "$rc"
# 上面每條路都會離開。走到這裡表示沒把結束碼交回去，不要讓 bash 去讀後面的二進位。
exit 1
__VIPLESTREAM_FRAME_RUN_PAYLOAD__
END_HEADER

header="$work/header"
tr -d '\r' < "$raw" > "$header"

nlines=$(wc -l < "$header")
nlines=${nlines//[[:space:]]/}
case "$nlines" in
    ''|*[!0-9]*) die "bad header line count" ;;
esac
payload_line=$((nlines + 1))
[ "$payload_line" -gt 1 ] || die "bad payload offset"
last=$(tail -n 1 "$header")
last=${last//$'\r'/}
[ "$last" = "__VIPLESTREAM_FRAME_RUN_PAYLOAD__" ] || die "header marker is not the last line"

sed \
    -e "s/@@SHA256@@/${payload_sha}/g" \
    -e "s/@@FLATPAK@@/${flat_base}/g" \
    -e "s/@@LINE@@/${payload_line}/g" \
    "$header" > "$work/header.sub"
tr -d '\r' < "$work/header.sub" > "$header"

if grep -E -q '@@SHA256@@|@@FLATPAK@@|@@LINE@@' "$header"; then
    die "placeholder left in header"
fi
if grep -q $'\r' "$header"; then
    die "CR in header"
fi
embedded=$(sed -n 's/^PAYLOAD_LINE=//p' "$header")
[ "$embedded" = "$payload_line" ] || die "payload line was not written"
nlines2=$(wc -l < "$header")
nlines2=${nlines2//[[:space:]]/}
[ "$nlines2" = "$nlines" ] || die "header line count changed"
last=$(tail -n 1 "$header")
[ "$last" = "__VIPLESTREAM_FRAME_RUN_PAYLOAD__" ] || die "header marker moved"

tmp_out=$(mktemp "$out_dir/.${run_base}.XXXXXX") || die "mktemp failed"
cat "$header" "$payload" > "$tmp_out"

# 跟頭盔上同一種讀法再驗一次。行號算錯時，整檔的 sha256 仍會變，但標頭裡的雜湊對不上就該停。
tail -n +"$payload_line" -- "$tmp_out" > "$work/check.tar.gz" || die "self-check read failed"
check=$(sha256sum "$work/check.tar.gz") || die "self-check sha256 failed"
check=${check%% *}
[ "$check" = "$payload_sha" ] || die "self-check sha256 mismatch (offset $payload_line)"

chmod 755 "$tmp_out" || die "chmod failed"
mv -f -- "$tmp_out" "$out_path"
tmp_out=""

# 旁檔是整個 .run 的 sha256（GitHub asset digest 對的是這個）。標頭裡的是 payload。
( cd "$out_dir" && sha256sum "$run_base" > "$run_base.sha256" ) || die "sha256sum of .run failed"

printf '%s\n' "$out_path"
