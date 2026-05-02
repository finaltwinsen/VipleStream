#!/usr/bin/env python3
"""
VipleStream §J.3.e.2.i VkFruc Auto-tune Harness
================================================
Minimal autotune for VkFrucRenderer's native Vulkan compute FRUC chain
(NV12->RGB + ME + Median + Warp + dual-present).

Differences from fruc_autotune.py (which targets D3D11+FRUC):
  - Binary: VipleStream.exe (rebrand from Moonlight.exe)
  - Log glob: VipleStream-*.log (TEMP)
  - Log regex: [VIPLE-VKFRUC-Stats] / [VIPLE-VKFRUC-SW]
  - Renderer is selected via Settings dropdown (default RS_VULKAN), not env var
  - One config: VkFruc with dual-present + FRUC compute (no preset matrix)
  - PresentMon optional — if not available, falls back to VkFruc's own
    [VIPLE-VKFRUC-Stats] log lines (which are already p50/p95/p99/p99.9
    +compute_gpu_mean, sufficient for tuning)

Usage:
  python vkfruc_autotune.py --host <hostname-or-ip> --app Desktop \
      --width 1920 --height 1080 --fps 180 --seconds 25 --warmup 8

Output:
  output/<timestamp>_<label>/  (or one specified via --out-label)
    summary.json            — all parsed metrics
    moonlight.log           — copy of VipleStream client log
    presentmon.csv          — if PresentMon was available

Exit code 0 = ran to completion, regardless of metric quality.
Exit code 1 = setup failure (binary not found, server unreachable, etc.).
Exit code 2 = stream never received frames (server down, network, init fail).
"""

from __future__ import annotations
import argparse
import json
import re
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

SCRIPT_DIR = Path(__file__).parent.resolve()
PROJECT_ROOT = SCRIPT_DIR.parent.parent
DEFAULT_BIN = PROJECT_ROOT / "temp" / "moonlight" / "VipleStream.exe"
PRESENTMON   = PROJECT_ROOT / "scripts" / "PresentMon-2.4.1-x64.exe"
TEMP_DIR = Path.home() / "AppData" / "Local" / "Temp"

# §J.3.e.2.i.6 stats log:
#   [VIPLE-VKFRUC-Stats] dual-present n=451 fps=90.05 ft_mean=11.10ms p50=11.67 p95=21.27 p99=22.46 p99.9=24.17 (window 5.0s)
RE_VKFRUC_STATS = re.compile(
    r"\[VIPLE-VKFRUC-Stats\]\s+(?P<mode>\S+)\s+"
    r"n=(?P<n>\d+)\s+fps=(?P<fps>[\d.]+)\s+"
    r"ft_mean=(?P<ft_mean>[\d.]+)ms\s+"
    r"p50=(?P<p50>[\d.]+)\s+p95=(?P<p95>[\d.]+)\s+"
    r"p99=(?P<p99>[\d.]+)\s+p99\.9=(?P<p999>[\d.]+)"
)
# [VIPLE-VKFRUC-Stats] cumul real=452 interp=452 compute_gpu_mean=1.100ms (n=450) (swMode=1 frucMode=1 dualMode=1)
RE_VKFRUC_GPU = re.compile(
    r"\[VIPLE-VKFRUC-Stats\]\s+cumul\s+real=(?P<real>\d+)\s+"
    r"interp=(?P<interp>\d+)\s+"
    r"compute_gpu_mean=(?P<gpu>[\d.]+)ms\s+\(n=(?P<gn>\d+)\)\s+"
    r"\(swMode=(?P<sw>\d)\s+frucMode=(?P<fm>\d)\s+dualMode=(?P<dm>\d)\)"
)
# [VIPLE-VKFRUC-SW] frame#0 ENTRY
RE_FIRST_FRAME = re.compile(r"\[VIPLE-VKFRUC-SW\]\s+frame#0\s+ENTRY")
# [VIPLE-VKFRUC-SW] frame#0 DUAL OK
RE_DUAL_OK     = re.compile(r"\[VIPLE-VKFRUC-SW\]\s+frame#\d+\s+DUAL OK")
# Renderer chosen
RE_RENDERER_CHOSEN = re.compile(r"Renderer '(?P<r>[^']+)' chosen")


@dataclass
class StatsAgg:
    fps_mean: Optional[float] = None
    ft_mean: Optional[float] = None
    p50: Optional[float] = None
    p95: Optional[float] = None
    p99: Optional[float] = None
    p999: Optional[float] = None
    gpu_mean_ms: Optional[float] = None
    cumul_real: Optional[int] = None
    cumul_interp: Optional[int] = None
    n_buckets: int = 0
    n_dual_ok: int = 0
    first_frame_seen: bool = False
    renderer_name: Optional[str] = None
    mode: Optional[str] = None


def parse_log(path: Path) -> StatsAgg:
    s = StatsAgg()
    if not path.is_file():
        return s
    text = path.read_text(encoding="utf-8", errors="replace")
    if RE_FIRST_FRAME.search(text):
        s.first_frame_seen = True
    s.n_dual_ok = len(RE_DUAL_OK.findall(text))
    m = RE_RENDERER_CHOSEN.search(text)
    if m:
        s.renderer_name = m.group("r")

    fps_l, ft_l, p50_l, p95_l, p99_l, p999_l = [], [], [], [], [], []
    for m in RE_VKFRUC_STATS.finditer(text):
        s.mode = m.group("mode")
        fps_l.append(float(m.group("fps")))
        ft_l.append(float(m.group("ft_mean")))
        p50_l.append(float(m.group("p50")))
        p95_l.append(float(m.group("p95")))
        p99_l.append(float(m.group("p99")))
        p999_l.append(float(m.group("p999")))
    s.n_buckets = len(fps_l)

    def avg(xs: List[float]) -> Optional[float]:
        return sum(xs) / len(xs) if xs else None
    s.fps_mean = avg(fps_l)
    s.ft_mean = avg(ft_l)
    s.p50 = avg(p50_l)
    s.p95 = avg(p95_l)
    s.p99 = avg(p99_l)
    s.p999 = avg(p999_l)

    gpu_last = None
    cumul_r_last = None
    cumul_i_last = None
    for m in RE_VKFRUC_GPU.finditer(text):
        gpu_last = float(m.group("gpu"))
        cumul_r_last = int(m.group("real"))
        cumul_i_last = int(m.group("interp"))
    s.gpu_mean_ms = gpu_last
    s.cumul_real = cumul_r_last
    s.cumul_interp = cumul_i_last
    return s


def stop_viplestream():
    subprocess.run(["taskkill", "/F", "/IM", "VipleStream.exe"],
                   capture_output=True, check=False)
    # ETW / driver cleanup window observed empirically.
    time.sleep(2.0)


def latest_log(after_ts: float) -> Optional[Path]:
    best, best_mt = None, 0.0
    for p in TEMP_DIR.glob("VipleStream-*.log"):
        try:
            mt = p.stat().st_mtime
        except OSError:
            continue
        if mt > after_ts and mt > best_mt:
            best, best_mt = p, mt
    return best


def run_one(args, label: str, out_dir: Path) -> Dict[str, Any]:
    binary = Path(args.bin) if args.bin else DEFAULT_BIN
    if not binary.is_file():
        return {"error": f"binary_not_found: {binary}"}

    out_dir.mkdir(parents=True, exist_ok=True)
    print(f"\n=== Run: {label} ===")
    print(f"  binary: {binary}")
    print(f"  host:   {args.host}  app: {args.app}")
    print(f"  res:    {args.width}x{args.height}@{args.fps}fps "
          f"warmup={args.warmup}s capture={args.seconds}s")

    stop_viplestream()
    t_launch = time.time()
    cmd = [
        str(binary), "stream", args.host, args.app,
        "--fps", str(args.fps),
        "--resolution", f"{args.width}x{args.height}",
        "--display-mode", "windowed",
        "--video-codec", args.codec,
        "--video-decoder", "software",  # VkFruc 走 SW upload path, 必須 force SW decoder
        "--frame-interpolation",
    ]
    proc = subprocess.Popen(cmd)
    print(f"  launched pid={proc.pid}")

    # Warmup, then optionally start PresentMon for the capture window.
    time.sleep(args.warmup)

    pm_csv = out_dir / "presentmon.csv"
    pm_proc: Optional[subprocess.Popen] = None
    if PRESENTMON.is_file() and not args.no_presentmon:
        # Stop any leftover ETW session from prior runs.
        subprocess.run(["logman", "stop", "viple_vkfruc_pm", "-ets"],
                       capture_output=True, check=False)
        pm_args = [
            str(PRESENTMON),
            "--process_id", str(proc.pid),
            "--output_file", str(pm_csv),
            "--timed", str(args.seconds),
            "--terminate_after_timed",
            "--terminate_on_proc_exit",
            "--v2_metrics",
            "--no_console_stats",
            "--stop_existing_session",
            "--session_name", "viple_vkfruc_pm",
        ]
        try:
            pm_log = open(out_dir / "presentmon.log", "w")
            pm_proc = subprocess.Popen(pm_args, stdout=pm_log, stderr=subprocess.STDOUT)
            print(f"  presentmon started pid={pm_proc.pid}")
        except Exception as e:
            print(f"  [WARN] presentmon failed to start: {e}")
            pm_proc = None

    # Wait for the capture window to elapse (or for either the stream
    # or PresentMon to die, whichever happens first).
    t_capture_end = time.time() + args.seconds
    while time.time() < t_capture_end:
        if proc.poll() is not None:
            print(f"  [WARN] stream process exited mid-capture")
            break
        if pm_proc is not None and pm_proc.poll() is not None:
            break
        time.sleep(0.5)

    if pm_proc is not None and pm_proc.poll() is None:
        try: pm_proc.wait(timeout=5)
        except: pm_proc.kill()
    stop_viplestream()
    log_src = latest_log(t_launch)
    log_copy = None
    if log_src:
        log_copy = out_dir / f"vplog_{label}.log"
        try:
            shutil.copy2(log_src, log_copy)
        except OSError as e:
            print(f"  [WARN] copy log failed: {e}")
            log_copy = None

    parsed = parse_log(log_copy) if log_copy else StatsAgg()
    print(f"  renderer={parsed.renderer_name}  first_frame={parsed.first_frame_seen}  "
          f"dual_ok_frames={parsed.n_dual_ok}  stats_buckets={parsed.n_buckets}")
    if parsed.fps_mean:
        print(f"  fps={parsed.fps_mean:.2f}  ft_mean={parsed.ft_mean:.2f}ms  "
              f"p99={parsed.p99:.2f}  p99.9={parsed.p999:.2f}  "
              f"gpu={parsed.gpu_mean_ms or 0:.2f}ms")

    # PresentMon CSV → DisplayLatency / GPUBusy stats.
    pm_metrics: Dict[str, Any] = {}
    if pm_csv.is_file() and pm_csv.stat().st_size > 0:
        try:
            import csv as csvlib
            ftimes, lats, gpus = [], [], []
            with open(pm_csv, newline="", encoding="utf-8", errors="replace") as f:
                r = csvlib.DictReader(f)
                for row in r:
                    for k_ft in ("MsBetweenPresents", "FrameTime"):
                        v = row.get(k_ft)
                        if v and v != "NA":
                            try: ftimes.append(float(v))
                            except: pass
                            break
                    for k_lat in ("MsUntilDisplayed", "DisplayLatency"):
                        v = row.get(k_lat)
                        if v and v != "NA":
                            try: lats.append(float(v))
                            except: pass
                            break
                    for k_gpu in ("MsGPUActive", "GPUBusy"):
                        v = row.get(k_gpu)
                        if v and v != "NA":
                            try: gpus.append(float(v))
                            except: pass
                            break

            def stats_of(xs: List[float]) -> Optional[Dict[str, float]]:
                if not xs: return None
                s = sorted(xs)
                def pct(p):
                    idx = int(len(s) * p / 100)
                    return s[min(idx, len(s) - 1)]
                return {"n": len(xs), "mean": round(sum(xs) / len(xs), 3),
                        "p50": round(pct(50), 3), "p95": round(pct(95), 3),
                        "p99": round(pct(99), 3), "p999": round(pct(99.9), 3)}
            pm_metrics = {
                "n_present":     len(ftimes),
                "frame_time":    stats_of(ftimes),
                "display_lat":   stats_of(lats),
                "gpu_busy":      stats_of(gpus),
            }
            if pm_metrics["display_lat"]:
                lat = pm_metrics["display_lat"]
                print(f"  pm: lat_mean={lat['mean']:.2f}ms  lat_p99={lat['p99']:.2f}ms  "
                      f"presents={len(ftimes)}")
        except Exception as e:
            print(f"  [WARN] PresentMon CSV parse failed: {e}")
            pm_metrics = {"error": str(e)}

    res = {
        "label":       label,
        "log_path":    str(log_copy) if log_copy else None,
        "renderer":    parsed.renderer_name,
        "mode":        parsed.mode,
        "first_frame": parsed.first_frame_seen,
        "dual_ok_frames": parsed.n_dual_ok,
        "n_buckets":   parsed.n_buckets,
        "fps_mean":    parsed.fps_mean,
        "ft_mean":     parsed.ft_mean,
        "p50":         parsed.p50,
        "p95":         parsed.p95,
        "p99":         parsed.p99,
        "p99_9":       parsed.p999,
        "gpu_mean_ms": parsed.gpu_mean_ms,
        "cumul_real":  parsed.cumul_real,
        "cumul_interp":parsed.cumul_interp,
        "presentmon":  pm_metrics,
    }
    (out_dir / "summary.json").write_text(json.dumps(res, indent=2), encoding="utf-8")
    return res


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--host", required=True)
    p.add_argument("--app", default="Desktop")
    p.add_argument("--width", type=int, default=1920)
    p.add_argument("--height", type=int, default=1080)
    p.add_argument("--fps", type=int, default=180)
    p.add_argument("--seconds", type=int, default=20)
    p.add_argument("--warmup", type=int, default=8)
    p.add_argument("--bin", default=None)
    p.add_argument("--codec", default="HEVC", choices=["AUTO", "H264", "HEVC", "AV1"],
                   help="Force codec via --video-codec CLI arg (default HEVC; "
                        "VkFruc + AV1 has cascade conflict, HEVC keeps cascade clean)")
    p.add_argument("--no-presentmon", action="store_true",
                   help="Skip PresentMon capture (useful if PresentMon ETW perms not granted)")
    p.add_argument("--out-label", default="run")
    p.add_argument("--out-dir", default=str(SCRIPT_DIR / "output"))
    a = p.parse_args()

    out_root = Path(a.out_dir)
    ts = time.strftime("%Y%m%d_%H%M%S")
    out_dir = out_root / f"{ts}_{a.out_label}"
    res = run_one(a, a.out_label, out_dir)

    if res.get("error"):
        print(f"[ERR] {res['error']}")
        return 1
    if not res.get("first_frame"):
        print("[ERR] stream never received first frame — check host/app")
        return 2
    if res.get("n_buckets", 0) == 0:
        print("[WARN] no [VIPLE-VKFRUC-Stats] buckets — too short or VkFruc not used")
    print(f"\n[OK] output dir: {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
