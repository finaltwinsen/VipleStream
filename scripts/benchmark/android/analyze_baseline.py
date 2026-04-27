#!/usr/bin/env python3
"""
VipleStream Android FRUC baseline analyzer.
Reads the output of android_baseline.sh and prints a human summary +
optional ASCII chart. No external deps (stdlib only).

Usage:
  python scripts/benchmark/android/analyze_baseline.py temp/baseline_<ts>/
"""
from __future__ import annotations

import os
import re
import sys
from pathlib import Path

THERMAL_STATUS_NAMES = {
    0: "NONE",
    1: "LIGHT",
    2: "MODERATE",
    3: "SEVERE",
    4: "CRITICAL",
    5: "EMERGENCY",
    6: "SHUTDOWN",
}


def load_tsv(path: Path) -> list[dict]:
    rows = []
    with path.open() as f:
        header = f.readline().rstrip("\n").split("\t")
        for line in f:
            parts = line.rstrip("\n").split("\t")
            if len(parts) != len(header):
                continue
            row = {}
            for k, v in zip(header, parts):
                if v == "NA" or v == "":
                    row[k] = None
                elif k in ("ts_unix", "elapsed_s", "thermal_status", "sensor_status_max"):
                    try:
                        row[k] = int(v)
                    except ValueError:
                        row[k] = None
                else:
                    try:
                        row[k] = float(v)
                    except ValueError:
                        row[k] = None
            rows.append(row)
    return rows


def summarize_thermal(rows: list[dict]) -> None:
    if not rows:
        print("  (empty timeseries)")
        return
    n = len(rows)
    duration = rows[-1]["elapsed_s"] - rows[0]["elapsed_s"]
    print(f"  samples={n}  duration={duration}s")

    for col, label in [
        ("skin_avg", "skin"),
        ("gpu_avg", "GPU"),
        ("cpu_avg", "CPU"),
        ("battery", "battery"),
    ]:
        vals = [r[col] for r in rows if r.get(col) is not None]
        if not vals:
            continue
        print(f"  {label:8s} min={min(vals):5.1f}  max={max(vals):5.1f}  mean={sum(vals)/len(vals):5.1f}  delta={vals[-1]-vals[0]:+5.1f}  C")

    status_peak = max((r["thermal_status"] for r in rows if r.get("thermal_status") is not None), default=0)
    sensor_peak = max((r["sensor_status_max"] for r in rows if r.get("sensor_status_max") is not None), default=0)
    print(f"  Thermal status peak: global={status_peak} ({THERMAL_STATUS_NAMES.get(status_peak, '?')})  "
          f"any-sensor={sensor_peak} ({THERMAL_STATUS_NAMES.get(sensor_peak, '?')})")

    transitions = []
    last = None
    for r in rows:
        s = r.get("thermal_status")
        if s is not None and s != last:
            transitions.append((r["elapsed_s"], last, s))
            last = s
    if len(transitions) > 1:
        print("  Status transitions:")
        for t, old, new in transitions:
            from_str = THERMAL_STATUS_NAMES.get(old, "?") if old is not None else "INIT"
            to_str = THERMAL_STATUS_NAMES.get(new, "?")
            print(f"    +{t:4d}s  {from_str} -> {to_str}")


def summarize_fruc_log(path: Path) -> None:
    if not path.exists():
        print("  (no fruc_app.log — app didn't write or FRUC disabled)")
        return
    text = path.read_text(errors="replace")
    lines = text.splitlines()

    init = next((l for l in lines if "FRUC initialized" in l), None)
    if init:
        print(f"  init: {init}")

    # Frame-by-frame entries: "<unix_ms> frame=N interpolated (total=M) fps=F"
    samples = []  # list of (unix_ms, frame, total_interp, fps)
    for l in lines:
        m = re.match(r"(\d+) frame=(\d+) interpolated \(total=(\d+)\) fps=([\d.]+)", l)
        if m:
            samples.append((int(m.group(1)), int(m.group(2)), int(m.group(3)), float(m.group(4))))

    if samples:
        ts0 = samples[0][0]
        ts_last = samples[-1][0]
        elapsed_s = (ts_last - ts0) / 1000
        last_frame = samples[-1][1]
        last_interp = samples[-1][2]
        input_fps = last_frame / elapsed_s if elapsed_s > 0 else 0
        interp_rate = last_interp / last_frame if last_frame else 0

        fps_vals = [s[3] for s in samples if s[3] > 0]
        if fps_vals:
            print(f"  total: input frames={last_frame}  interp={last_interp} ({interp_rate*100:.1f}%)  duration={elapsed_s:.1f}s")
            print(f"  input fps:  ~{input_fps:.1f}")
            print(f"  output fps: min={min(fps_vals):.1f}  max={max(fps_vals):.1f}  mean={sum(fps_vals)/len(fps_vals):.1f}  samples={len(fps_vals)}")

            # FPS over time — bucket every 30s, look for degradation
            print("  output fps over time (30s buckets, mean):")
            bucket_size = 30.0
            buckets: dict[int, list[float]] = {}
            for ts, _f, _i, fps in samples:
                if fps <= 0: continue
                bucket = int((ts - ts0) / 1000 / bucket_size)
                buckets.setdefault(bucket, []).append(fps)
            for b in sorted(buckets):
                vals = buckets[b]
                t_start = int(b * bucket_size)
                t_end = t_start + int(bucket_size)
                bar = "#" * int(sum(vals) / len(vals) / 3)  # rough bar at scale ~30 chars per 90fps
                print(f"    +{t_start:3d}–{t_end:3d}s  fps={sum(vals)/len(vals):5.1f}  n={len(vals):2d}  {bar}")

    skips = [l for l in lines if "SKIPPED" in l]
    if skips:
        print(f"  skipped frames events: {len(skips)} (first: {skips[0]})")
    gl_errs = [l for l in lines if "GL error" in l]
    if gl_errs:
        print(f"  GL errors: {len(gl_errs)} (first: {gl_errs[0]})")
    init_failed = [l for l in lines if "INIT FAILED" in l]
    if init_failed:
        print(f"  ⚠ INIT FAILED: {init_failed[0]}")


def summarize_gfxinfo(path: Path) -> None:
    if not path.exists():
        return
    text = path.read_text(errors="replace")
    profile = re.search(r"Total frames rendered: (\d+).*?Janky frames: (\d+) \(([\d.]+)%\).*?50th percentile: (\d+)ms.*?90th percentile: (\d+)ms.*?95th percentile: (\d+)ms.*?99th percentile: (\d+)ms",
                        text, re.DOTALL)
    if profile:
        total, jank, jank_pct, p50, p90, p95, p99 = profile.groups()
        print(f"  total={total}  janky={jank} ({jank_pct}%)  p50={p50}ms  p90={p90}ms  p95={p95}ms  p99={p99}ms")
    else:
        print("  (gfxinfo profile section not found)")


def summarize_logcat(path: Path) -> None:
    if not path.exists():
        return
    therm_evts = []
    fatals = []
    pixel_thermal: dict[str, list[tuple[float, float]]] = {}  # sensor -> [(elapsed_s, temp)]
    t0_ms = None

    pattern_pixel = re.compile(r"^(\d{2}-\d{2}) (\d{2}):(\d{2}):(\d{2})\.(\d{3})\s+\d+\s+\d+ I pixel-thermal:\s+([a-zA-Z0-9_-]+):([\-\d.]+)")
    with path.open(errors="replace") as f:
        for line in f:
            if "ThermalManagerService" in line and ("Status" in line or "throttl" in line.lower()):
                therm_evts.append(line.strip())
            if "FATAL" in line or ("AndroidRuntime" in line and "E/" in line):
                fatals.append(line.strip())
            m = pattern_pixel.match(line)
            if m:
                _date, hh, mm, ss, ms, sensor, temp_str = m.groups()
                # Wall-clock relative is enough — pixel-thermal emits ~every 2s
                t_ms = (int(hh)*3600 + int(mm)*60 + int(ss))*1000 + int(ms)
                if t0_ms is None:
                    t0_ms = t_ms
                try:
                    pixel_thermal.setdefault(sensor, []).append(((t_ms - t0_ms) / 1000.0, float(temp_str)))
                except ValueError:
                    pass

    if pixel_thermal:
        print("  pixel-thermal kernel log (high-frequency, more accurate than dumpsys cache):")
        for sensor in sorted(pixel_thermal.keys()):
            samples = pixel_thermal[sensor]
            if len(samples) < 2: continue
            temps = [t for _, t in samples if t > -100]
            if not temps: continue
            first_t, last_t = samples[0][1], samples[-1][1]
            duration = samples[-1][0] - samples[0][0]
            print(f"    {sensor:25s} n={len(samples):4d}  start={first_t:5.1f}  end={last_t:5.1f}  delta={last_t-first_t:+5.1f}C  range=[{min(temps):.1f},{max(temps):.1f}]  span={duration:.0f}s")

    if therm_evts:
        print(f"  ThermalManagerService events ({len(therm_evts)}):")
        for e in therm_evts[:5]:
            print(f"    {e}")
        if len(therm_evts) > 5:
            print(f"    ... and {len(therm_evts) - 5} more")
    if fatals:
        print(f"  ⚠ fatal/E events ({len(fatals)}):")
        for e in fatals[:3]:
            print(f"    {e}")


def main():
    if len(sys.argv) < 2:
        print(f"usage: {sys.argv[0]} <baseline_dir>", file=sys.stderr)
        sys.exit(1)
    d = Path(sys.argv[1])
    if not d.is_dir():
        print(f"error: {d} not a directory", file=sys.stderr)
        sys.exit(1)

    print(f"=== Thermal timeseries ({d / 'thermal_timeseries.tsv'}) ===")
    tsv = d / "thermal_timeseries.tsv"
    if tsv.exists():
        summarize_thermal(load_tsv(tsv))
    else:
        print("  (file missing)")

    print()
    print("=== FRUC app log ===")
    summarize_fruc_log(d / "fruc_app.log")

    print()
    print("=== gfxinfo (post-stream) ===")
    summarize_gfxinfo(d / "post_gfxinfo.txt")

    print()
    print("=== logcat events ===")
    summarize_logcat(d / "logcat.log")


if __name__ == "__main__":
    main()
