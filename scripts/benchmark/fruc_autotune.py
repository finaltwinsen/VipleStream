#!/usr/bin/env python3
"""
VipleStream FRUC Auto-tune Harness
===================================
Automates the full iterate loop:

    1. Pick a FRUC configuration (quality preset + any registry knobs)
    2. Write settings into HKCU\\Software\\Moonlight...\\Moonlight
    3. Launch Moonlight in stream mode (server must already be streaming)
    4. Let it warm up
    5. Capture N seconds:
         - PresentMon CSV (frame pacing, GPU busy, display latency)
         - The Moonlight log file that gets written to %TEMP%\\Moonlight-*.log
           (parses [VIPLE-FRUC-Stats], [VIPLE-FRUC-Generic], [VIPLE-FRUC],
            and anything else we emitted)
    6. Gracefully shut Moonlight down
    7. Parse both inputs into a single per-config summary
    8. After all configs run, write out a ranking table + JSON

Differences from the existing test_fruc_30s.ps1:
    - Captures and parses the Moonlight log (so per-shader GPU timestamps
      added in v1.1.133 actually land in the summary)
    - Correlates MV statistics, skip_ratio, me_gpu_ms, warp_gpu_ms with
      PresentMon frame pacing in a single table
    - Designed to be driven by an outer tuning loop (see --sweep)

Usage (run once, 4 presets):
    python fruc_autotune.py --host <host-ip> --app Desktop

Usage (sweep one preset across FRUC on/off):
    python fruc_autotune.py --configs off balanced

Prereqs:
    - Sunshine on the target host with a video-playing app (e.g. browser
      on a YouTube loop) so the stream has motion to work with
    - `scripts/PresentMon-2.4.1-x64.exe` present (downloaded by maintainer)
    - User is in the Performance Log Users group
      (see grant_presentmon_access.cmd)
"""

from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import json
import re
import shutil
import subprocess
import sys
import time
import urllib.request
import winreg
import zipfile
from dataclasses import dataclass, field, asdict
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

# ------------------------------------------------------------
# Paths / constants
# ------------------------------------------------------------

SCRIPT_DIR = Path(__file__).parent.resolve()
PROJECT_ROOT = SCRIPT_DIR.parent.parent
PRESENTMON = PROJECT_ROOT / "scripts" / "PresentMon-2.4.1-x64.exe"
PRESENTMON_URL = (
    "https://github.com/GameTechDev/PresentMon/releases/download/"
    "v2.4.1/PresentMon-2.4.1-x64.exe"
)
DEFAULT_MOONLIGHT_CANDIDATES = [
    Path(r"C:\Program Files\Moonlight Game Streaming\Moonlight.exe"),
    Path(r"C:\Users") / Path.home().name
        / r"AppData\Local\Programs\Moonlight Game Streaming\Moonlight.exe",
]
MOONLIGHT_EXE: Optional[Path] = None  # resolved in main()


def find_moonlight() -> Optional[Path]:
    """Locate Moonlight.exe. Check standard install paths first, then
    fall back to the newest VipleStream-Client-* folder in release/ so
    testing a freshly-built client doesn't require a system install."""
    for p in DEFAULT_MOONLIGHT_CANDIDATES:
        if p.is_file():
            return p
    # Fallback: highest-versioned extracted release
    rel = PROJECT_ROOT / "release"
    best: Optional[Path] = None
    best_version: Tuple[int, int, int] = (0, 0, 0)
    if rel.is_dir():
        for d in rel.glob("VipleStream-Client-*"):
            m = re.match(r"VipleStream-Client-(\d+)\.(\d+)\.(\d+)$", d.name)
            exe = d / "Moonlight.exe"
            if m and exe.is_file():
                v = (int(m[1]), int(m[2]), int(m[3]))
                if v > best_version:
                    best, best_version = exe, v
    return best
REG_PATH = r"Software\Moonlight Game Streaming Project\Moonlight"
TEMP_DIR = Path.home() / "AppData" / "Local" / "Temp"


def ensure_presentmon() -> Path:
    """Download PresentMon 2.4.1 to scripts/ if not already present.
    The binary is gitignored; users who grab the repo fresh just let
    the harness fetch it on first run."""
    if PRESENTMON.is_file():
        return PRESENTMON
    PRESENTMON.parent.mkdir(parents=True, exist_ok=True)
    print(f"[*] PresentMon not found. Downloading from GameTechDev...")
    print(f"    URL: {PRESENTMON_URL}")
    try:
        urllib.request.urlretrieve(PRESENTMON_URL, PRESENTMON)
        print(f"[OK] Cached at {PRESENTMON}")
    except Exception as e:
        print(f"[ERR] PresentMon download failed: {e}")
        sys.exit(1)
    return PRESENTMON

PRESET_MAP: Dict[str, Dict[str, Any]] = {
    "off":          {"interp": "false", "backend": 0, "quality": 1},
    "quality":      {"interp": "true",  "backend": 0, "quality": 0},
    "balanced":     {"interp": "true",  "backend": 0, "quality": 1},
    "performance":  {"interp": "true",  "backend": 0, "quality": 2},
    "nvof_quality": {"interp": "true",  "backend": 1, "quality": 0},
}

# ------------------------------------------------------------
# Log parsing — extract FRUC metrics from Moonlight's runtime log
# ------------------------------------------------------------

# Each 5 s the renderer emits one of these:
#   [VIPLE-FRUC-Stats] submit=N skip=M skip_ratio=P% gapAvg=Xms (expected=Yms)
#     me_gpu=A.ABms warp_gpu=B.BBms (total=C.CCms)
RE_FRUC_STATS = re.compile(
    r"\[VIPLE-FRUC-Stats\]\s+"
    r"submit=(?P<submit>\d+)\s+"
    r"skip=(?P<skip>\d+)\s+"
    r"skip_ratio=(?P<skip_ratio>[\d.]+)%\s+"
    r"gapAvg=(?P<gap_avg>\d+)ms\s+\(expected=(?P<expected>\d+)ms\)"
    r"(?:\s+me_gpu=(?P<me_gpu>[\d.]+)ms"
    r"(?:\s+median_gpu=(?P<median_gpu>[\d.]+)ms)?"
    r"\s+warp_gpu=(?P<warp_gpu>[\d.]+)ms"
    r"\s+\(total=(?P<fruc_gpu_total>[\d.]+)ms\))?"
)

# Periodic MV diagnostic:
#   [VIPLE-FRUC-Generic] frame=N MV[WxH]: X[a..b] Y[c..d] avgAbs=v nonZero=k/t
RE_FRUC_MV = re.compile(
    r"\[VIPLE-FRUC-Generic\]\s+frame=(?P<frame>\d+)\s+"
    r"MV\[(?P<mv_w>\d+)x(?P<mv_h>\d+)\]:\s+"
    r"X\[(?P<x_min>[-\d.]+)\.\.(?P<x_max>[-\d.]+)\]\s+"
    r"Y\[(?P<y_min>[-\d.]+)\.\.(?P<y_max>[-\d.]+)\]\s+"
    r"avgAbs=(?P<avg_abs>[\d.]+)\s+"
    r"nonZero=(?P<nz>\d+)/(?P<total>\d+)"
)

RE_FRUC_READY = re.compile(
    r"\[VIPLE-FRUC\]\s+(?:Generic compute shader|NVIDIA Optical Flow)\s+ready:"
    r"\s+(?P<w>\d+)x(?P<h>\d+)\s+\(display\s+(?P<dw>\d+)x(?P<dh>\d+)\)"
)


@dataclass
class LogParse:
    fruc_backend: Optional[str] = None
    fruc_size: Optional[Tuple[int, int]] = None
    display_size: Optional[Tuple[int, int]] = None
    stats_samples: List[Dict[str, float]] = field(default_factory=list)
    mv_samples: List[Dict[str, float]] = field(default_factory=list)

    def summary(self) -> Dict[str, Any]:
        """Collapse per-sample lists into single numbers we'll report."""
        def avg(xs):
            xs = [x for x in xs if x is not None]
            return sum(xs) / len(xs) if xs else None

        def last(xs, key):
            vals = [s.get(key) for s in xs if s.get(key) is not None]
            return vals[-1] if vals else None

        stats = self.stats_samples
        mvs = self.mv_samples
        return {
            "backend":    self.fruc_backend,
            "fruc_size":  self.fruc_size,
            "display":    self.display_size,
            "n_stats":    len(stats),
            "n_mv":       len(mvs),
            "submit":     last(stats, "submit"),
            "skip":       last(stats, "skip"),
            "skip_ratio": last(stats, "skip_ratio"),
            "gap_avg_ms": avg([s.get("gap_avg") for s in stats]),
            "me_gpu_ms":     avg([s.get("me_gpu") for s in stats]),
            "median_gpu_ms": avg([s.get("median_gpu") for s in stats]),
            "warp_gpu_ms":   avg([s.get("warp_gpu") for s in stats]),
            "mv_avg_abs": avg([s.get("avg_abs") for s in mvs]),
            "mv_nz_ratio":avg([
                (s["nz"] / s["total"]) if s.get("total") else None
                for s in mvs
            ]),
        }


def parse_moonlight_log(path: Path) -> LogParse:
    p = LogParse()
    if not path.is_file():
        return p
    text = path.read_text(encoding="utf-8", errors="replace")
    for m in RE_FRUC_READY.finditer(text):
        p.fruc_size = (int(m["w"]), int(m["h"]))
        p.display_size = (int(m["dw"]), int(m["dh"]))
        if "NVIDIA Optical Flow ready" in m.group(0):
            p.fruc_backend = "nvidia_of"
        else:
            p.fruc_backend = "generic_cs"
    for m in RE_FRUC_STATS.finditer(text):
        d = m.groupdict()
        p.stats_samples.append({
            "submit":     int(d["submit"]),
            "skip":       int(d["skip"]),
            "skip_ratio": float(d["skip_ratio"]),
            "gap_avg":    int(d["gap_avg"]),
            "expected":   int(d["expected"]),
            "me_gpu":     float(d["me_gpu"]) if d["me_gpu"] else None,
            "median_gpu": float(d["median_gpu"]) if d.get("median_gpu") else None,
            "warp_gpu":   float(d["warp_gpu"]) if d["warp_gpu"] else None,
        })
    for m in RE_FRUC_MV.finditer(text):
        d = m.groupdict()
        p.mv_samples.append({
            "frame":   int(d["frame"]),
            "avg_abs": float(d["avg_abs"]),
            "nz":      int(d["nz"]),
            "total":   int(d["total"]),
        })
    return p


# ------------------------------------------------------------
# PresentMon CSV parsing — a subset of what test_fruc_30s.ps1 does,
# in Python so we can keep everything in one pipeline.
# ------------------------------------------------------------

def _percentile(sorted_vals: List[float], p: float) -> float:
    if not sorted_vals:
        return 0.0
    idx = int(len(sorted_vals) * p / 100)
    return sorted_vals[min(idx, len(sorted_vals) - 1)]


def parse_presentmon_csv(path: Path) -> Dict[str, Any]:
    if not path.is_file():
        return {"error": "no_csv"}
    import csv as csvlib
    rows = []
    with open(path, "r", newline="", encoding="utf-8", errors="replace") as f:
        reader = csvlib.DictReader(f)
        for r in reader:
            rows.append(r)
    if not rows:
        return {"error": "empty_csv"}
    cols = set(rows[0].keys())

    def pick(*names):
        for n in names:
            if n in cols:
                return n
        return None

    c_ft   = pick("MsBetweenPresents", "FrameTime", "msBetweenPresents")
    c_gpu  = pick("MsGPUActive", "GPUBusy", "msGPUActive")
    c_lat  = pick("DisplayLatency", "MsInPresentAPI")
    c_drop = pick("Dropped", "AllowsTearing", "FrameType")

    def series(col):
        out = []
        if not col:
            return out
        for r in rows:
            v = r.get(col)
            if v and v != "NA":
                try:
                    out.append(float(v))
                except ValueError:
                    pass
        return out

    ft = series(c_ft)
    gpu = series(c_gpu)
    lat = series(c_lat)

    def stats(xs):
        if not xs:
            return None
        s = sorted(xs)
        mean = sum(xs) / len(xs)
        return {
            "n":    len(xs),
            "mean": round(mean, 3),
            "p50":  round(_percentile(s, 50), 3),
            "p95":  round(_percentile(s, 95), 3),
            "p99":  round(_percentile(s, 99), 3),
            "p999": round(_percentile(s, 99.9), 3),
            "max":  round(s[-1], 3),
        }

    # Dropped frames: logic differs across PresentMon versions.
    dropped = 0
    if c_drop == "Dropped":
        dropped = sum(1 for r in rows if r.get("Dropped") in ("1", 1))
    elif c_drop == "FrameType":
        dropped = sum(
            1 for r in rows
            if r.get("FrameType") and r["FrameType"] not in ("", "Application")
        )

    ft_stats = stats(ft)
    fps = round(1000.0 / ft_stats["mean"], 2) if ft_stats else 0.0
    # Spike counts relative to mean — same semantics as test_fruc_30s.ps1.
    spikes = {"over_1_5x": 0, "over_2x": 0, "over_3x": 0}
    if ft_stats:
        m = ft_stats["mean"]
        for v in ft:
            if v > m * 1.5: spikes["over_1_5x"] += 1
            if v > m * 2.0: spikes["over_2x"]   += 1
            if v > m * 3.0: spikes["over_3x"]   += 1
    return {
        "rows":    len(rows),
        "fps":     fps,
        "dropped": dropped,
        "dropped_pct": round(100.0 * dropped / max(len(rows), 1), 2),
        "frame_time":  ft_stats,
        "gpu_busy":    stats(gpu),
        "display_lat": stats(lat),
        "spikes":      spikes,
    }


# ------------------------------------------------------------
# Registry helpers (Windows HKCU Moonlight settings)
# ------------------------------------------------------------

def reg_get(name: str) -> Optional[Any]:
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, REG_PATH, 0,
                            winreg.KEY_READ) as k:
            v, _ = winreg.QueryValueEx(k, name)
            return v
    except FileNotFoundError:
        return None


def reg_set(name: str, value: Any, dword: bool = False):
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, REG_PATH, 0,
                        winreg.KEY_WRITE) as k:
        if dword:
            winreg.SetValueEx(k, name, 0, winreg.REG_DWORD, int(value))
        else:
            winreg.SetValueEx(k, name, 0, winreg.REG_SZ, str(value))


# ------------------------------------------------------------
# Process helpers
# ------------------------------------------------------------

def stop_moonlight():
    subprocess.run(["taskkill", "/F", "/IM", "Moonlight.exe"],
                   capture_output=True, check=False)
    # Empirically 1 s isn't enough: PresentMon on the next iteration
    # sometimes fails to capture a CSV even though Moonlight is
    # running and FRUC is logging stats. Giving the OS / ETW layer
    # 3 s to fully clean up before we relaunch fixed it.
    time.sleep(3.0)


def _foreground_moonlight(pid: int) -> bool:
    """Push Moonlight's top-level window to the foreground after
    launch. Without this, Moonlight often comes up behind the
    terminal running the harness and you can't see whether the
    stream is actually live — which defeats the point of manual
    visual validation during an auto-tune sweep.

    Enumerates top-level windows, matches by the PID we launched,
    then ShowWindow(SW_RESTORE) + SetForegroundWindow. Win32's
    SetForegroundWindow can refuse focus changes when the
    foreground-lock rules aren't met, so we also try the alt-tab
    workaround (briefly attach input threads). Best-effort — a
    False return is non-fatal.
    """
    user32 = ctypes.windll.user32
    user32.ShowWindow.argtypes = [wintypes.HWND, ctypes.c_int]
    user32.SetForegroundWindow.argtypes = [wintypes.HWND]
    user32.GetWindowThreadProcessId.argtypes = [
        wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user32.AttachThreadInput.argtypes = [
        wintypes.DWORD, wintypes.DWORD, wintypes.BOOL]
    user32.AttachThreadInput.restype = wintypes.BOOL
    GetCurrentThreadId = ctypes.windll.kernel32.GetCurrentThreadId

    EnumWindowsProc = ctypes.WINFUNCTYPE(
        wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    found_hwnd = [None]

    def cb(hwnd, _):
        owner_pid = wintypes.DWORD(0)
        tid = user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner_pid))
        if owner_pid.value == pid and user32.IsWindowVisible(hwnd):
            found_hwnd[0] = (hwnd, tid)
            return False
        return True

    user32.EnumWindows(EnumWindowsProc(cb), 0)
    if not found_hwnd[0]:
        return False

    hwnd, target_tid = found_hwnd[0]
    user32.ShowWindow(hwnd, 9)  # SW_RESTORE
    # Attach this process's input thread so SetForegroundWindow
    # doesn't get denied by the foreground-lock.
    my_tid = GetCurrentThreadId()
    user32.AttachThreadInput(my_tid, target_tid, True)
    ok = bool(user32.SetForegroundWindow(hwnd))
    user32.AttachThreadInput(my_tid, target_tid, False)
    return ok


def _resolve_moonlight_pid(ml_proc: subprocess.Popen) -> Optional[int]:
    """Figure out which PID is the currently-streaming Moonlight.
    First choice: our own Popen's PID (when `Moonlight.exe stream …`
    runs, that exact process hosts the stream). Fallback: ask
    tasklist for Moonlight.exe — covers the rare case where
    Moonlight relaunches itself (auto-updater etc.) leaving us
    parented to a dead launcher."""
    # Cheap check — Popen.poll() == None means our child is alive.
    if ml_proc.poll() is None:
        return ml_proc.pid
    # Launcher died; look for a running Moonlight.exe.
    r = subprocess.run(
        ["tasklist", "/FI", "IMAGENAME eq Moonlight.exe", "/FO", "CSV", "/NH"],
        capture_output=True, text=True, check=False,
    )
    for line in r.stdout.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if len(parts) >= 2 and parts[0].startswith("Moonlight"):
            try:
                return int(parts[1])
            except ValueError:
                pass
    return None


def latest_moonlight_log(after_timestamp: float) -> Optional[Path]:
    """Pick the Moonlight-*.log file whose mtime is newer than the given
    wall-clock timestamp. Moonlight names each log by ms-since-epoch, so
    we just take the highest mtime that landed after our launch."""
    best: Optional[Path] = None
    best_mtime = 0.0
    for p in TEMP_DIR.glob("Moonlight-*.log"):
        try:
            mtime = p.stat().st_mtime
        except OSError:
            continue
        if mtime > after_timestamp and mtime > best_mtime:
            best = p
            best_mtime = mtime
    return best


def is_admin_or_perflog() -> bool:
    """PresentMon needs ETW access. Admin OR S-1-5-32-559."""
    try:
        if ctypes.windll.shell32.IsUserAnAdmin():
            return True
    except Exception:
        pass
    # Checking group membership via winapi from Python is messy — fall
    # back to just trying PresentMon and catching the access-denied exit.
    return True  # we'll let PresentMon tell us if it can't open ETW


# ------------------------------------------------------------
# One run (one config)
# ------------------------------------------------------------

@dataclass
class RunResult:
    config: str
    presentmon: Dict[str, Any]
    moonlight_log: Optional[str]
    fruc: Dict[str, Any]
    error: Optional[str] = None


def run_one(cfg: str, args: argparse.Namespace, out_dir: Path) -> RunResult:
    preset = PRESET_MAP[cfg]
    print(f"\n{'='*62}")
    print(f"  Config: {cfg}   (interp={preset['interp']}, "
          f"backend={preset['backend']}, quality={preset['quality']})")
    print(f"{'='*62}")

    reg_set("frameInterpolation", preset["interp"])
    reg_set("frucBackend", preset["backend"], dword=True)
    reg_set("frucQuality", preset["quality"], dword=True)

    stop_moonlight()

    t_launch = time.time()
    ml_args = [
        str(MOONLIGHT_EXE), "stream", args.host, args.app,
        "--fps", str(args.fps),
        "--resolution", f"{args.width}x{args.height}",
    ]
    if args.keep_windowed:
        ml_args += ["--display-mode", "windowed"]
    print(f"Launching: {' '.join(ml_args[1:])}")
    try:
        # Default creation flags — DETACHED_PROCESS was preventing
        # Moonlight from acquiring foreground focus and taking full
        # screen. A benchmark run is no use if we can't see whether
        # the stream actually came up.
        ml_proc = subprocess.Popen(ml_args)
    except OSError as e:
        return RunResult(cfg, {}, None, {}, f"launch_failed: {e}")

    print(f"Warming up {args.warmup}s...")
    time.sleep(args.warmup)
    _foreground_moonlight(ml_proc.pid)

    # Resolve Moonlight's actual PID. Popen.pid is our launched
    # process; in `Moonlight.exe stream ...` that IS the streaming
    # process (no fork), but be defensive — if the launcher has
    # already exited, fall back to tasklist to find the live one.
    ml_pid = _resolve_moonlight_pid(ml_proc)
    if ml_pid is None:
        return RunResult(cfg, {}, None, {}, "moonlight_exited_early")
    print(f"  Moonlight PID: {ml_pid}")

    csv_path = out_dir / f"presentmon_{cfg}.csv"
    pm_log = out_dir / f"presentmon_{cfg}.log"
    session_name = f"viple_autotune_{cfg}"
    print(f"Recording {args.seconds}s -> {csv_path.name}")
    # Clear stale ETW session name.
    subprocess.run(["logman", "stop", session_name, "-ets"],
                   capture_output=True, check=False)

    # Use --process_id instead of --process_name: without admin
    # elevation, PresentMon can't resolve the process name from
    # ETW events and --process_name silently matches zero frames
    # from the 2nd iteration onward. --process_id bypasses the
    # name lookup entirely and targets a specific PID directly —
    # which is also cheaper (no per-frame name compare).
    # --terminate_on_proc_exit so PresentMon shuts down if
    # Moonlight dies unexpectedly instead of hanging out the full
    # --timed window.
    pm_args = [
        str(PRESENTMON),
        "--process_id", str(ml_pid),
        "--output_file", str(csv_path),
        "--timed", str(args.seconds),
        "--terminate_after_timed",
        "--terminate_on_proc_exit",
        "--v2_metrics",
        "--no_console_stats",
        "--stop_existing_session",
        "--session_name", session_name,
    ]
    try:
        with open(pm_log, "w") as log_f:
            pm_proc = subprocess.run(
                pm_args, stdout=log_f, stderr=subprocess.STDOUT,
                timeout=args.seconds + 30, check=False
            )
    except subprocess.TimeoutExpired:
        return RunResult(cfg, {}, None, {}, "presentmon_timeout")
    if pm_proc.returncode != 0:
        print(f"  [WARN] PresentMon rc={pm_proc.returncode}")

    # Stop Moonlight AND snapshot its log. Moonlight keeps the log file
    # open for writes so we copy before it gets recycled by the next run.
    stop_moonlight()
    ml_log = latest_moonlight_log(t_launch)
    ml_log_copy: Optional[Path] = None
    if ml_log:
        ml_log_copy = out_dir / f"moonlight_{cfg}.log"
        try:
            shutil.copy2(ml_log, ml_log_copy)
        except OSError:
            ml_log_copy = None

    pm_metrics = parse_presentmon_csv(csv_path)
    fruc_metrics: Dict[str, Any] = {}
    if ml_log_copy:
        parsed = parse_moonlight_log(ml_log_copy)
        fruc_metrics = parsed.summary()

    # Console one-liner so the user sees progress without opening files.
    ft = pm_metrics.get("frame_time") or {}
    me = fruc_metrics.get("me_gpu_ms")
    med = fruc_metrics.get("median_gpu_ms")
    warp = fruc_metrics.get("warp_gpu_ms")
    skip = fruc_metrics.get("skip_ratio")
    print(f"  fps={pm_metrics.get('fps', 0):.2f}  "
          f"p95={ft.get('p95', 0):.2f}ms  "
          f"p99={ft.get('p99', 0):.2f}ms  "
          f"dropped={pm_metrics.get('dropped_pct', 0):.1f}%  "
          f"me={me or 0:.2f}ms  med={med or 0:.2f}ms  warp={warp or 0:.2f}ms  "
          f"skip={skip or 0:.1f}%")

    return RunResult(
        config=cfg,
        presentmon=pm_metrics,
        moonlight_log=str(ml_log_copy) if ml_log_copy else None,
        fruc=fruc_metrics,
    )


# ------------------------------------------------------------
# Aggregate report
# ------------------------------------------------------------

def write_report(results: List[RunResult], out_dir: Path, args: argparse.Namespace):
    summary = {
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "host": args.host,
        "app":  args.app,
        "resolution": f"{args.width}x{args.height}",
        "fps":  args.fps,
        "seconds": args.seconds,
        "warmup": args.warmup,
        "runs": [
            {
                "config":       r.config,
                "error":        r.error,
                "presentmon":   r.presentmon,
                "fruc":         r.fruc,
                "moonlight_log": r.moonlight_log,
            }
            for r in results
        ],
    }
    with open(out_dir / "summary.json", "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=2, default=str)

    lines = [
        "# VipleStream FRUC auto-tune run",
        "",
        f"- Host: {args.host} | App: {args.app} | {args.fps}fps "
        f"| {args.width}x{args.height} | {args.seconds}s+{args.warmup}s warmup",
        f"- Timestamp: {summary['timestamp']}",
        "",
        "## Frame pacing (PresentMon)",
        "",
        "| Config | FPS | p50 | p95 | p99 | p99.9 | "
        "max | dropped% | spikes>2x |",
        "|---|---|---|---|---|---|---|---|---|",
    ]
    for r in results:
        if r.error:
            lines.append(f"| {r.config} | _{r.error}_ |||||||||")
            continue
        ft = r.presentmon.get("frame_time") or {}
        spikes = r.presentmon.get("spikes") or {}
        total = r.presentmon.get("rows", 1) or 1
        pct2x = round(100.0 * spikes.get("over_2x", 0) / total, 2)
        lines.append(
            f"| {r.config} | {r.presentmon.get('fps',0):.2f} | "
            f"{ft.get('p50','-')} | {ft.get('p95','-')} | "
            f"{ft.get('p99','-')} | {ft.get('p999','-')} | "
            f"{ft.get('max','-')} | {r.presentmon.get('dropped_pct',0):.1f}% | "
            f"{pct2x}% |"
        )
    lines += ["", "## FRUC internals (Moonlight log)", "",
              "| Config | Backend | ME GPU | Median GPU | Warp GPU | "
              "Skip% | MV avgAbs | MV nz% |",
              "|---|---|---|---|---|---|---|---|"]
    for r in results:
        f = r.fruc or {}
        me = f.get("me_gpu_ms")
        med = f.get("median_gpu_ms")
        warp = f.get("warp_gpu_ms")
        skip = f.get("skip_ratio")
        mv = f.get("mv_avg_abs")
        nz = f.get("mv_nz_ratio")
        def num(x, fmt):
            return ("-" if x is None else format(x, fmt))
        lines.append(
            f"| {r.config} | {f.get('backend','-')} | "
            f"{num(me,'.2f')}ms | {num(med,'.2f')}ms | {num(warp,'.2f')}ms | "
            f"{num(skip,'.1f')}% | {num(mv,'.2f')} | "
            f"{'-' if nz is None else f'{100*nz:.0f}%'} |"
        )
    report_path = out_dir / "report.md"
    report_path.write_text("\n".join(lines), encoding="utf-8")
    print(f"\n[OK] Report written: {report_path}")
    print(f"[OK] Summary JSON:    {out_dir / 'summary.json'}")


# ------------------------------------------------------------
# Main
# ------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(
        description="Automated Moonlight launch + 30s capture + analysis "
                    "across FRUC configurations"
    )
    ap.add_argument("--host", default="<host-ip>")
    ap.add_argument("--app", default="Desktop")
    ap.add_argument("--fps", type=int, default=60)
    ap.add_argument("--width", type=int, default=1920)
    ap.add_argument("--height", type=int, default=1080)
    ap.add_argument("--seconds", type=int, default=30)
    ap.add_argument("--warmup", type=int, default=8)
    ap.add_argument("--configs", nargs="+",
                    default=["off", "quality", "balanced", "performance"],
                    choices=list(PRESET_MAP.keys()))
    ap.add_argument("--keep-windowed", action="store_true",
                    help="Stream in windowed mode (easier to watch in real time)")
    ap.add_argument("--moonlight-exe", type=Path, default=None,
                    help="Path to Moonlight.exe. Default: system install if "
                         "present, else newest release/VipleStream-Client-*.")
    ap.add_argument("--out-dir", type=Path, default=None,
                    help="Where to drop per-config CSV + report.md + summary.json. "
                         "Default: temp/fruc_autotune_<timestamp>/")
    args = ap.parse_args()

    global MOONLIGHT_EXE
    MOONLIGHT_EXE = args.moonlight_exe or find_moonlight()
    if not MOONLIGHT_EXE or not MOONLIGHT_EXE.is_file():
        print(f"[ERR] Moonlight.exe not found. Tried candidates + release/, or "
              f"pass --moonlight-exe explicitly.")
        sys.exit(1)
    print(f"Moonlight: {MOONLIGHT_EXE}")
    ensure_presentmon()

    out_dir = args.out_dir or (
        PROJECT_ROOT / "temp"
        / f"fruc_autotune_{time.strftime('%Y%m%d_%H%M%S')}"
    )
    out_dir.mkdir(parents=True, exist_ok=True)
    print(f"Output: {out_dir}")

    # Remember original registry values so we can restore afterward.
    orig = {
        "frameInterpolation": reg_get("frameInterpolation"),
        "frucBackend":        reg_get("frucBackend"),
        "frucQuality":        reg_get("frucQuality"),
    }
    print(f"Original registry: {orig}")

    results: List[RunResult] = []
    try:
        for cfg in args.configs:
            results.append(run_one(cfg, args, out_dir))
    finally:
        stop_moonlight()
        # Restore.
        if orig["frameInterpolation"] is not None:
            reg_set("frameInterpolation", orig["frameInterpolation"])
        if orig["frucBackend"] is not None:
            reg_set("frucBackend", orig["frucBackend"], dword=True)
        if orig["frucQuality"] is not None:
            reg_set("frucQuality", orig["frucQuality"], dword=True)

    write_report(results, out_dir, args)


if __name__ == "__main__":
    main()
