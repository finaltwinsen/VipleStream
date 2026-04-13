#!/usr/bin/env python3
"""
VipleStream FRUC Client Metric Collector
=========================================
Captures Moonlight's FRUC debug output (OutputDebugString on Windows),
parses motion vector statistics, and generates a test report.

Usage:
    python client_collect.py                    # Capture until Ctrl+C, then report
    python client_collect.py --timeout 90       # Auto-stop after 90s
    python client_collect.py --report log.json  # Re-analyze a saved log

How it works:
    On Windows, SDL_Log() calls OutputDebugString(). This script hooks into the
    Windows debug output mechanism (DBWIN_BUFFER) to capture those messages in
    real-time — no changes to Moonlight required.
"""

import ctypes
import ctypes.wintypes as wt
import struct
import threading
import time
import json
import re
import argparse
import signal
import sys
import os
from datetime import datetime
from collections import defaultdict
from pathlib import Path

# ============================================================
# Windows OutputDebugString Capture
# ============================================================

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

INFINITE = 0xFFFFFFFF
WAIT_OBJECT_0 = 0
WAIT_TIMEOUT = 0x102
PAGE_READWRITE = 0x04
FILE_MAP_READ = 0x0004
EVENT_MODIFY_STATE = 0x0002
SYNCHRONIZE = 0x00100000
BUFFER_SIZE = 4096


class SECURITY_ATTRIBUTES(ctypes.Structure):
    _fields_ = [
        ("nLength", wt.DWORD),
        ("lpSecurityDescriptor", wt.LPVOID),
        ("bInheritHandle", wt.BOOL),
    ]


class DebugOutputCapture:
    """Captures OutputDebugString messages from all processes on the system."""

    def __init__(self):
        self._thread = None
        self._stop = threading.Event()
        self._callbacks = []

    def add_callback(self, fn):
        """fn(pid: int, message: str)"""
        self._callbacks.append(fn)

    def start(self):
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def stop(self):
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=3)

    def _run(self):
        # Set up function prototypes for safety
        kernel32.CreateEventW.argtypes = [ctypes.c_void_p, wt.BOOL, wt.BOOL, wt.LPCWSTR]
        kernel32.CreateEventW.restype = wt.HANDLE
        kernel32.CreateFileMappingW.argtypes = [
            wt.HANDLE, ctypes.c_void_p, wt.DWORD, wt.DWORD, wt.DWORD, wt.LPCWSTR
        ]
        kernel32.CreateFileMappingW.restype = wt.HANDLE
        kernel32.MapViewOfFile.argtypes = [wt.HANDLE, wt.DWORD, wt.DWORD, wt.DWORD, ctypes.c_size_t]
        kernel32.MapViewOfFile.restype = ctypes.c_void_p
        kernel32.SetEvent.argtypes = [wt.HANDLE]
        kernel32.SetEvent.restype = wt.BOOL
        kernel32.WaitForSingleObject.argtypes = [wt.HANDLE, wt.DWORD]
        kernel32.WaitForSingleObject.restype = wt.DWORD
        kernel32.UnmapViewOfFile.argtypes = [ctypes.c_void_p]
        kernel32.UnmapViewOfFile.restype = wt.BOOL
        kernel32.CloseHandle.argtypes = [wt.HANDLE]
        kernel32.CloseHandle.restype = wt.BOOL

        buffer_ready = None
        data_ready = None
        mapping = None
        view = None

        try:
            buffer_ready = kernel32.CreateEventW(None, False, False, "DBWIN_BUFFER_READY")
            if not buffer_ready:
                print("[ERROR] CreateEvent(DBWIN_BUFFER_READY) failed — is DebugView running?")
                return

            data_ready = kernel32.CreateEventW(None, False, False, "DBWIN_DATA_READY")
            if not data_ready:
                print("[ERROR] CreateEvent(DBWIN_DATA_READY) failed")
                return

            # INVALID_HANDLE_VALUE = -1 as HANDLE
            invalid_handle = wt.HANDLE(-1)
            mapping = kernel32.CreateFileMappingW(
                invalid_handle, None, PAGE_READWRITE, 0, BUFFER_SIZE, "DBWIN_BUFFER"
            )
            if not mapping:
                print("[ERROR] CreateFileMapping(DBWIN_BUFFER) failed — is DebugView running?")
                return

            view = kernel32.MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0)
            if not view:
                print("[ERROR] MapViewOfFile failed")
                return

            print("[Collector] DBWIN capture active")

            while not self._stop.is_set():
                kernel32.SetEvent(buffer_ready)

                ret = kernel32.WaitForSingleObject(data_ready, 1000)
                if ret == WAIT_TIMEOUT:
                    continue
                if ret != WAIT_OBJECT_0:
                    continue

                try:
                    pid = ctypes.c_ulong.from_address(view).value
                    msg_bytes = ctypes.string_at(view + 4, BUFFER_SIZE - 4)
                    msg = msg_bytes.split(b"\x00", 1)[0].decode("utf-8", errors="replace").strip()
                except Exception:
                    continue

                if msg:
                    for cb in self._callbacks:
                        try:
                            cb(pid, msg)
                        except Exception as e:
                            print(f"[WARN] Callback error: {e}")

        except Exception as e:
            print(f"[ERROR] DBWIN capture: {e}")
        finally:
            if view:
                kernel32.UnmapViewOfFile(view)
            if mapping:
                kernel32.CloseHandle(mapping)
            if data_ready:
                kernel32.CloseHandle(data_ready)
            if buffer_ready:
                kernel32.CloseHandle(buffer_ready)


# ============================================================
# FRUC Log Parser
# ============================================================

# Regex patterns for FRUC log lines
RE_FRUC_INIT = re.compile(
    r"\[VIPLE-FRUC(?:-Generic)?\]\s+(?:NVIDIA Optical Flow|Generic compute shader) ready:\s+(\d+)x(\d+)"
)
RE_FRUC_MV = re.compile(
    r"\[VIPLE-FRUC-Generic\]\s+frame=(\d+)\s+MV\[(\d+)x(\d+)\]:\s+"
    r"X\[(-?[\d.]+)\.\.(-?[\d.]+)\]\s+Y\[(-?[\d.]+)\.\.(-?[\d.]+)\]\s+"
    r"avgAbs=([\d.]+)\s+nonZero=(\d+)/(\d+)"
)
RE_FRUC_NVIDIA = re.compile(
    r"\[VIPLE-FRUC\]\s+frame=(\d+)\s+status=OK\s+repeat=(\d+)\s+outIdx=(\d+)"
)
RE_FRUC_TOGGLE = re.compile(
    r"\[VIPLE-FRUC\]\s+FRUC\s+(PAUSED|RESUMED)"
)
RE_FRUC_TEXTURE = re.compile(
    r"\[VIPLE-FRUC-Generic\]\s+Textures:\s+full\s+(\d+)x(\d+),\s+MV\s+(\d+)x(\d+)"
)
RE_FRUC_BACKEND = re.compile(
    r"\[VIPLE-FRUC\]\s+(NVIDIA Optical Flow|Generic compute shader) ready"
)
RE_FRUC_ANY = re.compile(r"\[VIPLE-FRUC")


class FRUCCollector:
    """Collects and analyzes FRUC metrics from debug output."""

    def __init__(self):
        self.raw_lines = []          # All captured FRUC lines
        self.mv_samples = []         # Parsed MV statistics
        self.init_info = {}          # Init parameters
        self.backend = None          # "NVIDIA" or "Generic"
        self.resolution = None       # (width, height)
        self.frame_count = 0
        self.start_time = time.time()
        self.moonlight_pid = None
        self.paused = False

    def on_debug_message(self, pid, msg):
        """Callback for DebugOutputCapture — filter FRUC messages."""
        if "[VIPLE-FRUC" not in msg:
            return

        # Track the PID of the process emitting FRUC messages
        if self.moonlight_pid is None:
            self.moonlight_pid = pid
            print(f"[Collector] Moonlight PID: {pid}")

        elapsed = time.time() - self.start_time
        entry = {
            "time": elapsed,
            "timestamp": datetime.now().isoformat(),
            "pid": pid,
            "msg": msg,
        }
        self.raw_lines.append(entry)

        # Parse init
        m = RE_FRUC_BACKEND.search(msg)
        if m:
            self.backend = "NVIDIA" if "NVIDIA" in m.group(1) else "Generic"
            print(f"[Collector] FRUC backend: {self.backend}")

        m = RE_FRUC_INIT.search(msg)
        if m:
            self.resolution = (int(m.group(1)), int(m.group(2)))
            print(f"[Collector] FRUC resolution: {self.resolution[0]}x{self.resolution[1]}")

        # Parse MV stats (GenericFRUC)
        m = RE_FRUC_MV.search(msg)
        if m:
            sample = {
                "time": elapsed,
                "frame": int(m.group(1)),
                "mv_grid": (int(m.group(2)), int(m.group(3))),
                "x_range": (float(m.group(4)), float(m.group(5))),
                "y_range": (float(m.group(6)), float(m.group(7))),
                "avg_abs": float(m.group(8)),
                "non_zero": int(m.group(9)),
                "total": int(m.group(10)),
            }
            self.mv_samples.append(sample)
            nz_pct = 100.0 * sample["non_zero"] / max(sample["total"], 1)
            print(f"  [MV] frame={sample['frame']} avgAbs={sample['avg_abs']:.1f} "
                  f"nonZero={nz_pct:.0f}% X[{sample['x_range'][0]}..{sample['x_range'][1]}] "
                  f"Y[{sample['y_range'][0]}..{sample['y_range'][1]}]")

        # Parse NVIDIA FRUC status
        m = RE_FRUC_NVIDIA.search(msg)
        if m:
            self.frame_count = int(m.group(1))

        # Parse toggle
        m = RE_FRUC_TOGGLE.search(msg)
        if m:
            self.paused = (m.group(1) == "PAUSED")
            print(f"[Collector] FRUC {'PAUSED' if self.paused else 'RESUMED'}")

        # Parse texture info
        m = RE_FRUC_TEXTURE.search(msg)
        if m:
            self.init_info["full_res"] = f"{m.group(1)}x{m.group(2)}"
            self.init_info["mv_grid"] = f"{m.group(3)}x{m.group(4)}"
            print(f"[Collector] Textures: full={self.init_info['full_res']}, MV={self.init_info['mv_grid']}")

    def generate_report(self):
        """Generate analysis report from collected data."""
        report = {
            "meta": {
                "generated": datetime.now().isoformat(),
                "duration_s": time.time() - self.start_time,
                "backend": self.backend,
                "resolution": self.resolution,
                "init_info": self.init_info,
                "total_fruc_lines": len(self.raw_lines),
                "total_mv_samples": len(self.mv_samples),
                "moonlight_pid": self.moonlight_pid,
            },
            "mv_analysis": self._analyze_mv(),
            "raw_lines": self.raw_lines,
            "mv_samples": self.mv_samples,
        }
        return report

    def _analyze_mv(self):
        """Compute aggregate MV statistics."""
        if not self.mv_samples:
            return {"status": "no_data", "message": "No MV samples collected"}

        avg_abs_values = [s["avg_abs"] for s in self.mv_samples]
        nz_ratios = [s["non_zero"] / max(s["total"], 1) for s in self.mv_samples]
        x_ranges = [(s["x_range"][1] - s["x_range"][0]) for s in self.mv_samples]
        y_ranges = [(s["y_range"][1] - s["y_range"][0]) for s in self.mv_samples]

        analysis = {
            "status": "ok",
            "sample_count": len(self.mv_samples),
            "avg_abs_mv": {
                "min": min(avg_abs_values),
                "max": max(avg_abs_values),
                "mean": sum(avg_abs_values) / len(avg_abs_values),
            },
            "non_zero_ratio": {
                "min": min(nz_ratios),
                "max": max(nz_ratios),
                "mean": sum(nz_ratios) / len(nz_ratios),
            },
            "x_spread": {
                "min": min(x_ranges),
                "max": max(x_ranges),
                "mean": sum(x_ranges) / len(x_ranges),
            },
            "y_spread": {
                "min": min(y_ranges),
                "max": max(y_ranges),
                "mean": sum(y_ranges) / len(y_ranges),
            },
        }

        # Quality indicators
        indicators = []

        # Check 1: Are MVs ever non-zero? (basic functionality)
        if analysis["non_zero_ratio"]["max"] > 0:
            indicators.append(("PASS", "Motion vectors are being computed (non-zero MVs detected)"))
        else:
            indicators.append(("FAIL", "All motion vectors are zero — motion estimation may be broken"))

        # Check 2: Is avgAbs reasonable? (not stuck at 0, not maxed out)
        mean_abs = analysis["avg_abs_mv"]["mean"]
        if 0 < mean_abs < 100:
            indicators.append(("PASS", f"Average MV magnitude ({mean_abs:.1f}) is in reasonable range"))
        elif mean_abs == 0:
            indicators.append(("WARN", "Average MV magnitude is 0 — may be static content only"))
        else:
            indicators.append(("WARN", f"Average MV magnitude ({mean_abs:.1f}) is very high — possible instability"))

        # Check 3: Do MVs vary over time? (not stuck)
        if len(avg_abs_values) >= 2:
            variance = sum((v - mean_abs) ** 2 for v in avg_abs_values) / len(avg_abs_values)
            if variance > 0.1:
                indicators.append(("PASS", f"MV values vary across frames (variance={variance:.2f})"))
            else:
                indicators.append(("WARN", f"MV values are very uniform (variance={variance:.2f}) — may be stuck"))

        analysis["indicators"] = indicators
        return analysis


# ============================================================
# Report Printer
# ============================================================

def print_report(report):
    """Pretty-print the test report."""
    meta = report["meta"]
    mv = report["mv_analysis"]

    print()
    print("=" * 70)
    print("  VipleStream FRUC Test Report")
    print("=" * 70)
    print(f"  Generated:   {meta['generated']}")
    print(f"  Duration:    {meta['duration_s']:.0f}s")
    print(f"  Backend:     {meta['backend'] or 'Unknown'}")
    print(f"  Resolution:  {meta['resolution'] or 'Unknown'}")
    print(f"  FRUC lines:  {meta['total_fruc_lines']}")
    print(f"  MV samples:  {meta['total_mv_samples']}")
    print()

    if mv["status"] == "no_data":
        print("  [!] No MV data collected.")
        print("      - Is FRUC enabled in settings?")
        print("      - Is resolution >= 1920x1080?")
        print("      - Was a streaming session active?")
        print()
        return

    print("  Motion Vector Statistics")
    print("  " + "-" * 40)
    print(f"  Samples:     {mv['sample_count']}")
    print(f"  Avg |MV|:    {mv['avg_abs_mv']['min']:.1f} / {mv['avg_abs_mv']['mean']:.1f} / {mv['avg_abs_mv']['max']:.1f}  (min/mean/max)")
    print(f"  NonZero %%:   {mv['non_zero_ratio']['min']*100:.0f}% / {mv['non_zero_ratio']['mean']*100:.0f}% / {mv['non_zero_ratio']['max']*100:.0f}%")
    print(f"  X spread:    {mv['x_spread']['min']} / {mv['x_spread']['mean']:.0f} / {mv['x_spread']['max']}")
    print(f"  Y spread:    {mv['y_spread']['min']} / {mv['y_spread']['mean']:.0f} / {mv['y_spread']['max']}")
    print()

    print("  Quality Indicators")
    print("  " + "-" * 40)
    for status, msg in mv.get("indicators", []):
        icon = {"PASS": "+", "FAIL": "x", "WARN": "!"}[status]
        print(f"  [{icon}] {status}: {msg}")
    print()

    # Per-sample details (first 10)
    samples = report.get("mv_samples", [])
    if samples:
        print("  MV Sample Details (first 10)")
        print("  " + "-" * 60)
        print(f"  {'Frame':>6}  {'Time':>6}  {'AvgAbs':>7}  {'NZ%':>5}  {'X range':>12}  {'Y range':>12}")
        for s in samples[:10]:
            nz_pct = 100.0 * s["non_zero"] / max(s["total"], 1)
            print(f"  {s['frame']:>6}  {s['time']:>5.1f}s  {s['avg_abs']:>7.1f}  {nz_pct:>4.0f}%  "
                  f"{s['x_range'][0]:>5}..{s['x_range'][1]:<5}  "
                  f"{s['y_range'][0]:>5}..{s['y_range'][1]:<5}")
        if len(samples) > 10:
            print(f"  ... ({len(samples) - 10} more samples)")
    print()
    print("=" * 70)


# ============================================================
# Log File Tail (reliable alternative to DBWIN)
# ============================================================

class LogFileTailer:
    """Tail Moonlight's log file in real-time.
    Moonlight writes to %TEMP%/Moonlight-<epoch>.log.
    This is more reliable than DBWIN capture on Windows."""

    def __init__(self, log_dir=None):
        self._log_dir = log_dir or os.path.join(os.environ.get("LOCALAPPDATA", ""), "Temp")
        self._stop = threading.Event()
        self._thread = None
        self._callbacks = []
        self.log_path = None

    def add_callback(self, fn):
        """fn(pid: int, message: str) — pid is always 0 for file-based capture."""
        self._callbacks.append(fn)

    def start(self):
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def stop(self):
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=3)

    def _find_latest_log(self):
        """Find the most recently modified Moonlight-*.log file."""
        import glob
        pattern = os.path.join(self._log_dir, "Moonlight-*.log")
        files = glob.glob(pattern)
        if not files:
            return None
        return max(files, key=os.path.getmtime)

    def _run(self):
        # Wait for a log file to appear
        self.log_path = None
        for _ in range(60):  # Wait up to 60s
            if self._stop.is_set():
                return
            self.log_path = self._find_latest_log()
            if self.log_path:
                break
            time.sleep(1)

        if not self.log_path:
            print("[Tailer] ERROR: No Moonlight log file found")
            return

        print(f"[Tailer] Tailing: {self.log_path}")

        # Remember the initial file to detect new files
        initial_path = self.log_path
        last_size = 0

        with open(self.log_path, "r", encoding="utf-8", errors="replace") as f:
            # Seek to end — we only want new lines from this session
            f.seek(0, 2)
            last_size = f.tell()

            while not self._stop.is_set():
                # Check if a newer log file appeared (new Moonlight instance)
                newest = self._find_latest_log()
                if newest and newest != self.log_path:
                    print(f"[Tailer] New log detected: {newest}")
                    self.log_path = newest
                    f.close()
                    # Re-open new file
                    try:
                        f_new = open(self.log_path, "r", encoding="utf-8", errors="replace")
                    except OSError:
                        time.sleep(1)
                        continue
                    f = f_new  # noqa — intentional reassignment for file switch
                    last_size = 0

                line = f.readline()
                if line:
                    line = line.strip()
                    if line:
                        for cb in self._callbacks:
                            try:
                                cb(0, line)
                            except Exception as e:
                                print(f"[WARN] Callback error: {e}")
                else:
                    time.sleep(0.2)


# ============================================================
# Main
# ============================================================

def main():
    parser = argparse.ArgumentParser(description="VipleStream FRUC Metric Collector")
    parser.add_argument("--timeout", type=int, default=0, help="Auto-stop after N seconds (0=manual Ctrl+C)")
    parser.add_argument("--report", type=str, help="Re-analyze a saved JSON log instead of capturing")
    parser.add_argument("--output", type=str, default="fruc_test_report.json", help="Output JSON path")
    parser.add_argument("--log-file", type=str, help="Directly parse a Moonlight log file instead of live capture")
    args = parser.parse_args()

    # Re-analyze mode (JSON report)
    if args.report:
        with open(args.report) as f:
            report = json.load(f)
        print_report(report)
        return

    # Offline log analysis mode
    if args.log_file:
        print(f"[Collector] Parsing log file: {args.log_file}")
        collector = FRUCCollector()
        with open(args.log_file, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                collector.on_debug_message(0, line.strip())
        report = collector.generate_report()
        output_path = args.output
        with open(output_path, "w") as f:
            json.dump(report, f, indent=2, default=str)
        print(f"[Collector] Report saved to {output_path}")
        print_report(report)
        return

    # Live capture mode — tail Moonlight log file
    print("=" * 60)
    print("  VipleStream FRUC Metric Collector")
    print("=" * 60)
    print()
    print("  Tailing Moonlight log file in real-time...")
    print(f"  Log dir: {os.path.join(os.environ.get('LOCALAPPDATA', ''), 'Temp')}")
    print()

    collector = FRUCCollector()
    tailer = LogFileTailer()
    tailer.add_callback(collector.on_debug_message)
    tailer.start()

    print("[Collector] Waiting for Moonlight log output...")
    print()

    try:
        if args.timeout > 0:
            print(f"[Collector] Will auto-stop in {args.timeout}s")
            elapsed = 0
            while elapsed < args.timeout:
                time.sleep(5)
                elapsed += 5
                n_lines = len(collector.raw_lines)
                n_mv = len(collector.mv_samples)
                backend = collector.backend or "waiting..."
                print(f"  [{elapsed}s/{args.timeout}s] FRUC lines={n_lines} MV samples={n_mv} backend={backend}")
        else:
            while True:
                time.sleep(1)
    except KeyboardInterrupt:
        print()
        print("[Collector] Stopping capture...")

    tailer.stop()

    # Generate report
    report = collector.generate_report()

    # Save JSON
    output_path = args.output
    with open(output_path, "w") as f:
        json.dump(report, f, indent=2, default=str)
    print(f"[Collector] Report saved to {output_path}")

    # Print report
    print_report(report)


if __name__ == "__main__":
    main()
