#!/bin/bash
# update-hotspot.sh — 把 Steam Frame 頭盔目前的熱點資訊同步到我們 App 的 Flatpak 資料目錄（§VR-HOTSPOT-AUTO）
# 由 install-steamframe.sh 安裝成 systemd user service（viplestream-hotspot.service）。
#
# 來源：softapmanager.service 的 journal。熱點由頭盔上的 Steam 開機時送 create 指令建立，2026-10-30 在頭盔上確認格式是
#   <標籤>: <編號> [create<TAB><SSID><TAB><passphrase><TAB>hbs6<TAB><介面><TAB>--hidden]
# （hbs6＝6 GHz，6 GHz 只能 WPA3-SAE；--hidden＝不廣播 SSID）。目前使用者在 wheel 群組，讀得到系統 journal。
#
# 安全：金鑰絕不輸出到 stdout／stderr／journal（不 set -x、不 echo）；檔案 0600、目錄 0700、先寫暫存檔再 rename；
#       解析不到就刪掉舊檔，不留過期的機密。JSON 用 python3 產生（正確跳脫），金鑰經環境變數傳給 python（只有本使用者看得到）。
set -euo pipefail
set +x

TARGET_DIR="$HOME/.var/app/io.github.finaltwinsen.VipleStream/data/viplestream"
TARGET_FILE="$TARGET_DIR/hotspot.json"

parse_and_update() {
    local line rest ssid key band_tok _unused hidden=false
    line="$(journalctl -u softapmanager.service -b --no-pager -o cat 2>/dev/null | grep -F "[create$(printf '\t')" | tail -n 1 || true)"
    if [ -z "$line" ]; then
        rm -f "$TARGET_FILE"
        return 0
    fi
    # 欄位以 Tab 分隔（2026-10-30 在頭盔上確認），SSID 可能含空白，所以只用 Tab 切
    rest="${line#*\[create$'\t'}"
    IFS=$'\t' read -r ssid key band_tok _unused <<< "$rest"
    [[ "$line" == *--hidden* ]] && hidden=true

    # 驗證：SSID 1–32 位元組；WPA passphrase 8–63 字元或 64 個十六進位字元
    if [ -z "${ssid:-}" ] || [ ${#ssid} -gt 32 ] || [ -z "${key:-}" ] || [ ${#key} -lt 8 ] || [ ${#key} -gt 64 ]; then
        rm -f "$TARGET_FILE"
        echo "[VIPLE-VR-HOTSPOT] could not parse the softapmanager create line; hotspot.json removed"
        return 0
    fi
    local band="5GHz" channel=0 auth_type=1
    if [ "$band_tok" = "hbs6" ]; then
        band="6GHz"
        auth_type=2  # 6 GHz 只能 WPA3-SAE
    fi

    mkdir -p -m 0700 "$TARGET_DIR"
    local tmp_file="$TARGET_DIR/hotspot.json.tmp.$$"
    (
        umask 0177
        HS_SSID="$ssid" HS_KEY="$key" HS_BAND="$band" HS_CH="$channel" HS_AUTH="$auth_type" HS_HIDDEN="$hidden" python3 -c '
import json, os
print(json.dumps({"version": 1, "ssid": os.environ["HS_SSID"], "key": os.environ["HS_KEY"], "band": os.environ["HS_BAND"],
                  "channel": int(os.environ["HS_CH"]), "auth_type": int(os.environ["HS_AUTH"]),
                  "hidden": os.environ["HS_HIDDEN"] == "true"}, indent=2))' > "$tmp_file"
        chmod 0600 "$tmp_file"
        mv -f "$tmp_file" "$TARGET_FILE"
    )
    echo "[VIPLE-VR-HOTSPOT] updated ssid=${ssid:0:3}*** keylen=${#key} band=$band hidden=$hidden"
}

if [ "${1:-}" = "--daemon" ] || [ "${1:-}" = "--follow" ]; then
    parse_and_update || true
    # softapmanager 有新紀錄（熱點重建、換頻道）就重新解析
    journalctl -u softapmanager.service -b -n 0 -f -o cat 2>/dev/null | while read -r _; do
        parse_and_update || true
    done
else
    parse_and_update
fi
