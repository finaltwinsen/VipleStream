#!/usr/bin/env bash
# VipleStream Android FRUC baseline monitor.
# Polls thermal + gfxinfo + logcat while the user manually streams to the test host,
# then pulls the FRUC app log + post-stream snapshots.
#
# Usage:
#   scripts/benchmark/android/android_baseline.sh [duration_sec] [out_dir]
#
# Defaults: 300s (5 min), out_dir = temp/baseline_<timestamp>
#
# Pixel 5 thermal mType reference:
#   0 = CPU per-core    1 = GPU subsystem    2 = Battery
#   3 = Skin            4 = USB port         5 = Power amplifier (cellular)
#  -1 = Unknown (sdm-therm, panel-audio-therm)
# Thermal Status:
#   0 = NONE   1 = LIGHT   2 = MODERATE   3 = SEVERE
#   4 = CRITICAL   5 = EMERGENCY   6 = SHUTDOWN

set -u

# Git Bash on Windows mangles Android paths like /sdcard/... into
# C:\Program Files\Git\sdcard\... before they reach adb. Defeat that
# per-path with a leading // — MSYS leaves doubled-slash prefixes alone
# while local /d/... destinations still translate normally. (Setting
# MSYS_NO_PATHCONV=1 globally would break local paths the other way.)
SDCARD="//sdcard"

# Required: serial of the Android device under test (`adb devices`).
DEVICE="${VIPLE_ADB_DEVICE:-}"
if [[ -z "$DEVICE" ]]; then
    echo "[ERROR] Set VIPLE_ADB_DEVICE to your device serial (see 'adb devices')." >&2
    exit 1
fi

# Locate adb: PATH first, then $ANDROID_HOME, then $LOCALAPPDATA SDK on Windows.
if [[ -z "${ADB:-}" ]]; then
    if command -v adb >/dev/null 2>&1; then
        ADB="$(command -v adb)"
    elif [[ -n "${ANDROID_HOME:-}" && -x "$ANDROID_HOME/platform-tools/adb" ]]; then
        ADB="$ANDROID_HOME/platform-tools/adb"
    elif [[ -n "${LOCALAPPDATA:-}" && -x "$LOCALAPPDATA/Android/Sdk/platform-tools/adb.exe" ]]; then
        ADB="$LOCALAPPDATA/Android/Sdk/platform-tools/adb.exe"
    else
        echo "[ERROR] adb not found in PATH, ANDROID_HOME, or default Android SDK location." >&2
        exit 1
    fi
fi

# Debug builds carry the .debug applicationIdSuffix from app/build.gradle.
APP_PKG="${VIPLE_APP_PKG:-com.piinsta.debug}"

DURATION="${1:-300}"
OUT_DIR="${2:-temp/baseline_$(date +%Y%m%d_%H%M%S)}"

mkdir -p "$OUT_DIR"
echo "[VIPLE-BASELINE] device=$DEVICE  duration=${DURATION}s  out=$OUT_DIR"

if ! "$ADB" -s "$DEVICE" get-state >/dev/null 2>&1; then
    echo "[ERROR] adb device $DEVICE not connected" >&2
    exit 1
fi

extract_temp_avg() {
    # $1 = mType to filter on. Restrict to Temperature{...} lines only —
    # CoolingDevice{...} also has mType but its mValue is a state index, not
    # a temperature, so it would skew the average toward 0.
    grep -E '^[[:space:]]*Temperature\{' \
        | grep -oE "mValue=[0-9.]+, mType=$1," \
        | sed -E 's/mValue=([0-9.]+).*/\1/' \
        | awk '{ if ($1 > -100) { s += $1; c++ } } END { if (c) printf "%.2f", s/c; else printf "NA" }'
}

extract_status_max() {
    # Highest mStatus across Temperature{...} sensor lines only (Cached/HAL).
    grep -E '^[[:space:]]*Temperature\{' \
        | grep -oE "mStatus=[0-9]+" \
        | sed -E 's/mStatus=//' \
        | awk '{ if ($1 > m) m = $1 } END { printf "%d", m+0 }'
}

echo "[VIPLE-BASELINE] pre-stream snapshot"
"$ADB" -s "$DEVICE" shell dumpsys thermalservice > "$OUT_DIR/pre_thermal.txt" 2>&1
"$ADB" -s "$DEVICE" shell dumpsys gfxinfo "$APP_PKG" reset >/dev/null 2>&1 || true

# Do NOT rm fruc_log.txt — if FRUC is currently writing to it (stream
# already running), `rm` only unlinks the dirent and the open FD keeps
# writing to an orphan inode, so we lose the entire run's data. Instead
# record the start timestamp; analyzer filters by it.
date +%s%3N > "$OUT_DIR/run_start_ms.txt"
"$ADB" -s "$DEVICE" logcat -c >/dev/null 2>&1 || true

TSV="$OUT_DIR/thermal_timeseries.tsv"
printf "ts_unix\telapsed_s\tthermal_status\tsensor_status_max\tskin_avg\tgpu_avg\tcpu_avg\tbattery\n" > "$TSV"

start_ts=$(date +%s)

(
    while true; do
        now=$(date +%s)
        elapsed=$((now - start_ts))
        snap=$("$ADB" -s "$DEVICE" shell dumpsys thermalservice 2>/dev/null) || snap=""
        if [[ -z "$snap" ]]; then sleep 1; continue; fi
        status=$(echo "$snap" | grep -oE 'Thermal Status: [0-9]+' | head -1 | awk '{print $3}')
        status=${status:-NA}
        sensor_max=$(echo "$snap" | extract_status_max)
        skin=$(echo "$snap" | extract_temp_avg 3)
        gpu=$(echo  "$snap" | extract_temp_avg 1)
        cpu=$(echo  "$snap" | extract_temp_avg 0)
        bat=$(echo  "$snap" | extract_temp_avg 2)
        printf "%d\t%d\t%s\t%s\t%s\t%s\t%s\t%s\n" \
            "$now" "$elapsed" "$status" "$sensor_max" "$skin" "$gpu" "$cpu" "$bat" >> "$TSV"
        sleep 1
    done
) &
POLL_PID=$!

# Logcat filter spec uses single-letter levels (V/D/I/W/E/F/S). "tag:*" is
# silently ignored, leaving the *:S as the only matched filter and the file
# empty. pixel-thermal is a Pixel-specific kernel tag emitting per-sensor
# temps every ~2s — useful as a high-frequency cross-check on the dumpsys poll.
"$ADB" -s "$DEVICE" logcat -v threadtime \
    "FRUC:V" \
    "ThermalManagerService:V" \
    "thermal-engine:V" \
    "ThermalHAL:V" \
    "pixel-thermal:V" \
    "AndroidRuntime:E" \
    "Moonlight:V" \
    "*:S" \
    > "$OUT_DIR/logcat.log" 2>&1 &
LOGCAT_PID=$!

cleanup() {
    echo "[VIPLE-BASELINE] stopping..."
    kill "$POLL_PID" "$LOGCAT_PID" 2>/dev/null || true
    wait 2>/dev/null || true
}
trap cleanup INT TERM EXIT

cat <<EOF

[VIPLE-BASELINE] monitoring active for ${DURATION}s — Ctrl+C to stop early.
Output: $OUT_DIR

>>> ON THE PIXEL 5, NOW:
    1. Open VipleStream app
    2. Connect to your VipleStream-Server host
    3. Verify FRUC is enabled (Settings -> Frame interpolation)
    4. Start a game and play steadily for the full duration
    5. Wait for this script to finish (or Ctrl+C here when done)

EOF

sleep "$DURATION"

cleanup
trap - INT TERM EXIT

echo "[VIPLE-BASELINE] post-stream snapshot"
"$ADB" -s "$DEVICE" shell dumpsys thermalservice > "$OUT_DIR/post_thermal.txt" 2>&1
"$ADB" -s "$DEVICE" shell dumpsys gfxinfo "$APP_PKG" framestats > "$OUT_DIR/post_gfxinfo.txt" 2>&1
"$ADB" -s "$DEVICE" shell dumpsys SurfaceFlinger --latency "SurfaceView - $APP_PKG" > "$OUT_DIR/post_sf_latency.txt" 2>&1 || true
"$ADB" -s "$DEVICE" pull "$SDCARD/Download/fruc_log.txt" "$OUT_DIR/fruc_app.log" >/dev/null 2>&1 \
    && echo "  fruc_app.log pulled" \
    || echo "  (no fruc_log.txt on device — FRUC disabled or app didn't run?)"

echo "[VIPLE-BASELINE] done. Files:"
ls -la "$OUT_DIR"

# Quick summary for the impatient
echo
echo "=== Quick summary ==="
if [[ -f "$TSV" ]]; then
    rows=$(($(wc -l < "$TSV") - 1))
    if [[ "$rows" -gt 0 ]]; then
        max_status=$(awk -F'\t' 'NR>1 && $3!="NA" {if($3>m) m=$3} END {print m+0}' "$TSV")
        max_sensor_status=$(awk -F'\t' 'NR>1 && $4!="NA" {if($4>m) m=$4} END {print m+0}' "$TSV")
        peak_skin=$(awk -F'\t' 'NR>1 && $5!="NA" {if($5>m) m=$5} END {print m+0}' "$TSV")
        peak_gpu=$(awk -F'\t' 'NR>1 && $6!="NA" {if($6>m) m=$6} END {print m+0}' "$TSV")
        peak_cpu=$(awk -F'\t' 'NR>1 && $7!="NA" {if($7>m) m=$7} END {print m+0}' "$TSV")
        echo "samples=$rows  thermal_status_peak=$max_status  sensor_status_peak=$max_sensor_status"
        echo "peak temps: skin=${peak_skin}C  gpu=${peak_gpu}C  cpu=${peak_cpu}C"
    fi
fi
if [[ -f "$OUT_DIR/fruc_app.log" ]]; then
    echo "FRUC app log tail:"
    tail -5 "$OUT_DIR/fruc_app.log"
fi
