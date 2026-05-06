#!/usr/bin/env python3
"""
analyze_fruc_timing.py — VipleStream FRUC 補幀效果客觀分析（階段 1）

從 streaming log 解析 [VIPLE-VKFRUC-Stats] 跟 [VIPLE-VKFRUC-GPU-PROF] 行，
產生 frame timing histogram + GPU stage breakdown report。

用法：
    python analyze_fruc_timing.py [log_path]
    python analyze_fruc_timing.py --latest        # 自動找最新 log
    python analyze_fruc_timing.py --label baseline_b1b   # 加 label 區分版本

輸出：
    fruc_quality_<label_or_timestamp>.md          # markdown report
    fruc_quality_<label_or_timestamp>.png         # histogram

衡量指標：
    - frame interval mean / p50 / p95 / p99 / p99.9
    - **stutter score** = (p95 - p50) / p50  (越接近 0 越順)
    - GPU compute total / per-stage breakdown
    - cumul real / interp ratio (dual-present 應該 1:1)
"""

import argparse
import glob
import os
import re
import statistics
import sys
from datetime import datetime
from pathlib import Path

import matplotlib
matplotlib.use("Agg")  # non-interactive backend, file output only
import matplotlib.pyplot as plt
import numpy as np

# Try to find a CJK-capable font for the figure titles (Windows ships
# Microsoft JhengHei / SimSun for Chinese).  Falls back silently on
# Linux/macOS where DejaVu Sans is fine for the English-only labels.
for _font in ["Microsoft JhengHei", "Microsoft YaHei", "MingLiU", "SimHei", "Noto Sans CJK TC"]:
    try:
        from matplotlib import font_manager
        if any(f.name == _font for f in font_manager.fontManager.ttflist):
            matplotlib.rcParams["font.sans-serif"] = [_font, "DejaVu Sans"]
            matplotlib.rcParams["axes.unicode_minus"] = False
            break
    except Exception:
        pass


# 5-sec bucket Stats 行格式：
# [VIPLE-VKFRUC-Stats] dual-present n=151 fps=30.18 ft_mean=33.14ms p50=33.40 p95=34.21 p99=54.17 p99.9=65.08 (window 5.0s)
RE_STATS_TIMING = re.compile(
    r"\[VIPLE-VKFRUC-Stats\]\s+(?P<mode>single-present|dual-present)\s+"
    r"n=(?P<n>\d+)\s+fps=(?P<fps>[\d.]+)\s+ft_mean=(?P<ft_mean>[\d.]+)ms\s+"
    r"p50=(?P<p50>[\d.]+)\s+p95=(?P<p95>[\d.]+)\s+p99=(?P<p99>[\d.]+)\s+"
    r"p99\.9=(?P<p999>[\d.]+)"
)

# [VIPLE-VKFRUC-Stats] cumul real=158 interp=158 compute_gpu_total=1.062ms (n=150) (swMode=0 frucMode=1 dualMode=1)
RE_STATS_CUMUL = re.compile(
    r"\[VIPLE-VKFRUC-Stats\]\s+cumul\s+real=(?P<real>\d+)\s+interp=(?P<interp>\d+)\s+"
    r"compute_gpu_total=(?P<gpu_total>[\d.]+)ms\s+\(n=(?P<gpu_n>\d+)\)\s+"
    r"\(swMode=(?P<sw>\d)\s+frucMode=(?P<fruc>\d)\s+dualMode=(?P<dual>\d)\)"
)

# [VIPLE-VKFRUC-GPU-PROF] nv12rgb=461us me=243us median=5us warp=174us copy=179us (total=1062us, n=150)
RE_GPU_PROF = re.compile(
    r"\[VIPLE-VKFRUC-GPU-PROF\]\s+nv12rgb=(?P<nv12>\d+)us\s+me=(?P<me>\d+)us\s+"
    r"median=(?P<median>\d+)us\s+warp=(?P<warp>\d+)us\s+copy=(?P<copy>\d+)us\s+"
    r"\(total=(?P<total>\d+)us"
)

# ctor: [VIPLE-VKFRUC] §J.3.e.2.i.2 ctor (pass=0, swMode=0, frucMode=1, dualMode=1, prefs=RS_VULKAN)
RE_CTOR = re.compile(
    r"\[VIPLE-VKFRUC\].*ctor.*swMode=(?P<sw>\d).*frucMode=(?P<fruc>\d).*dualMode=(?P<dual>\d)"
)


def find_latest_log() -> Path:
    """Find newest VipleStream-*.log in %TEMP%."""
    temp_dir = os.environ.get("LOCALAPPDATA", "")
    if temp_dir:
        temp_dir = Path(temp_dir) / "Temp"
    else:
        temp_dir = Path.home() / "AppData" / "Local" / "Temp"
    logs = sorted(temp_dir.glob("VipleStream-*.log"), key=os.path.getmtime, reverse=True)
    if not logs:
        sys.exit(f"No VipleStream-*.log found in {temp_dir}")
    return logs[0]


def parse_log(path: Path) -> dict:
    """Parse log lines, return dict of bucketed stats lists."""
    timing = []      # list of dict per 5s bucket
    cumul = []       # list of dict per 5s bucket
    gpu_prof = []    # list of dict per 5s bucket
    last_ctor = None  # mode info

    with path.open(encoding="utf-8", errors="replace") as f:
        for line in f:
            m = RE_CTOR.search(line)
            if m:
                last_ctor = {
                    "swMode": int(m.group("sw")),
                    "frucMode": int(m.group("fruc")),
                    "dualMode": int(m.group("dual")),
                }
                continue
            m = RE_STATS_TIMING.search(line)
            if m:
                timing.append({
                    "mode": m.group("mode"),
                    "n": int(m.group("n")),
                    "fps": float(m.group("fps")),
                    "ft_mean": float(m.group("ft_mean")),
                    "p50": float(m.group("p50")),
                    "p95": float(m.group("p95")),
                    "p99": float(m.group("p99")),
                    "p999": float(m.group("p999")),
                })
                continue
            m = RE_STATS_CUMUL.search(line)
            if m:
                cumul.append({
                    "real": int(m.group("real")),
                    "interp": int(m.group("interp")),
                    "gpu_total_ms": float(m.group("gpu_total")),
                    "gpu_n": int(m.group("gpu_n")),
                    "swMode": int(m.group("sw")),
                    "frucMode": int(m.group("fruc")),
                    "dualMode": int(m.group("dual")),
                })
                continue
            m = RE_GPU_PROF.search(line)
            if m:
                gpu_prof.append({
                    "nv12rgb": int(m.group("nv12")),
                    "me": int(m.group("me")),
                    "median": int(m.group("median")),
                    "warp": int(m.group("warp")),
                    "copy": int(m.group("copy")),
                    "total": int(m.group("total")),
                })

    return {
        "ctor": last_ctor,
        "timing": timing,
        "cumul": cumul,
        "gpu_prof": gpu_prof,
    }


def summarize_list(values: list[float]) -> dict:
    if not values:
        return {"n": 0, "mean": 0, "median": 0, "stddev": 0, "min": 0, "max": 0}
    return {
        "n": len(values),
        "mean": statistics.mean(values),
        "median": statistics.median(values),
        "stddev": statistics.stdev(values) if len(values) > 1 else 0,
        "min": min(values),
        "max": max(values),
    }


def build_report(parsed: dict, log_path: Path, label: str) -> str:
    timing = parsed["timing"]
    cumul = parsed["cumul"]
    gpu = parsed["gpu_prof"]
    ctor = parsed["ctor"] or {}

    lines = []
    lines.append(f"# FRUC 補幀效果分析 — `{label}`")
    lines.append("")
    lines.append(f"- **Log file**: `{log_path}`")
    lines.append(f"- **Generated**: {datetime.now().isoformat(timespec='seconds')}")
    lines.append(f"- **VkFruc mode**: swMode={ctor.get('swMode', '?')} "
                 f"frucMode={ctor.get('frucMode', '?')} dualMode={ctor.get('dualMode', '?')}")
    lines.append(f"- **Buckets**: timing={len(timing)} cumul={len(cumul)} gpu_prof={len(gpu)}")
    lines.append("")

    if not timing:
        lines.append("⚠️ No `[VIPLE-VKFRUC-Stats]` timing lines found — streaming might not have started.")
        return "\n".join(lines)

    # ---- Frame timing summary (across buckets) ----
    lines.append("## Frame timing (per-bucket samples，5 秒 1 個)")
    lines.append("")
    lines.append("| 指標 | 值 |")
    lines.append("|------|------|")
    means = [b["ft_mean"] for b in timing]
    p50s = [b["p50"] for b in timing]
    p95s = [b["p95"] for b in timing]
    p99s = [b["p99"] for b in timing]
    p999s = [b["p999"] for b in timing]
    fpses = [b["fps"] for b in timing]
    s_mean = summarize_list(means)
    s_p50 = summarize_list(p50s)
    s_p95 = summarize_list(p95s)
    s_p99 = summarize_list(p99s)
    s_p999 = summarize_list(p999s)
    s_fps = summarize_list(fpses)

    lines.append(f"| ft_mean | {s_mean['mean']:.2f}ms (range {s_mean['min']:.2f}–{s_mean['max']:.2f}) |")
    lines.append(f"| p50     | {s_p50['mean']:.2f}ms (range {s_p50['min']:.2f}–{s_p50['max']:.2f}) |")
    lines.append(f"| p95     | {s_p95['mean']:.2f}ms (range {s_p95['min']:.2f}–{s_p95['max']:.2f}) |")
    lines.append(f"| p99     | {s_p99['mean']:.2f}ms (range {s_p99['min']:.2f}–{s_p99['max']:.2f}) |")
    lines.append(f"| p99.9   | {s_p999['mean']:.2f}ms (range {s_p999['min']:.2f}–{s_p999['max']:.2f}) |")
    lines.append(f"| fps     | {s_fps['mean']:.2f} (range {s_fps['min']:.2f}–{s_fps['max']:.2f}) |")
    lines.append("")

    # ---- Stutter scores ----
    # stutter = (p95 - p50) / p50
    # 越接近 0 越順、≥ 0.5 表示有 visible jitter
    stutters = [(b["p95"] - b["p50"]) / b["p50"] for b in timing if b["p50"] > 0]
    p99_outliers = [(b["p99"] - b["p50"]) / b["p50"] for b in timing if b["p50"] > 0]
    s_stutter = summarize_list(stutters)
    s_p99o = summarize_list(p99_outliers)

    lines.append("## Stutter score（越接近 0 越順）")
    lines.append("")
    lines.append("| 指標 | 值 | 解讀 |")
    lines.append("|------|------|------|")
    lines.append(f"| **(p95-p50)/p50** | {s_stutter['mean']:.3f} (median {s_stutter['median']:.3f}) | "
                 f"{'✅ 順' if s_stutter['mean'] < 0.10 else '⚠️ 中等' if s_stutter['mean'] < 0.30 else '❌ 不順'} |")
    lines.append(f"| **(p99-p50)/p50** | {s_p99o['mean']:.3f} (median {s_p99o['median']:.3f}) | "
                 f"{'✅ 無 outlier' if s_p99o['mean'] < 0.20 else '⚠️ 有 outlier' if s_p99o['mean'] < 0.60 else '❌ 嚴重 outlier'} |")
    lines.append("")

    # ---- Real / interp ratio ----
    if cumul:
        last_cumul = cumul[-1]
        if last_cumul["real"] > 0:
            interp_ratio = last_cumul["interp"] / last_cumul["real"]
            lines.append("## Real / Interp 比例")
            lines.append("")
            lines.append(f"- cumul real={last_cumul['real']} interp={last_cumul['interp']}")
            lines.append(f"- ratio = {interp_ratio:.3f} "
                         f"({'✅ 1:1 (正常 dual-present)' if 0.95 < interp_ratio < 1.05 else '⚠️ 比例異常'})")
            lines.append("")

    # ---- GPU compute breakdown ----
    if gpu:
        nv12s = [g["nv12rgb"] for g in gpu]
        mes = [g["me"] for g in gpu]
        medians = [g["median"] for g in gpu]
        warps = [g["warp"] for g in gpu]
        copies = [g["copy"] for g in gpu]
        totals = [g["total"] for g in gpu]

        lines.append("## GPU compute chain (us per source frame)")
        lines.append("")
        lines.append("| Stage | mean | min | max | std | 占比 |")
        lines.append("|-------|------|-----|-----|-----|------|")
        avg_total = sum(totals) / len(totals) if totals else 1
        for name, vals in [
            ("nv12rgb", nv12s),
            ("me", mes),
            ("median", medians),
            ("warp", warps),
            ("copy", copies),
            ("**total**", totals),
        ]:
            s = summarize_list(vals)
            pct = (s['mean'] / avg_total * 100) if avg_total > 0 and "total" not in name else 100
            lines.append(f"| {name} | {s['mean']:.0f} | {s['min']:.0f} | {s['max']:.0f} | "
                         f"{s['stddev']:.0f} | {pct:.1f}% |")
        lines.append("")

    # ---- TRIPLE budget projection ----
    if gpu:
        # DUAL: 1 ME + 1 warp + 1 nv12 + 1 copy
        # TRIPLE: 1 ME + 2 warps + 1 nv12 + 1 copy (extra warp for second interp)
        avg_warp = sum(warps) / len(warps)
        avg_total_dual = sum(totals) / len(totals)
        avg_total_triple = avg_total_dual + avg_warp  # 加 1 warp
        budget_180fps = 5555  # 5.55 ms = 1/180 sec
        budget_120fps = 8333

        lines.append("## TRIPLE 預算估算")
        lines.append("")
        lines.append(f"- DUAL total = {avg_total_dual:.0f}us")
        lines.append(f"- TRIPLE estimate (= DUAL + 1× warp) = {avg_total_triple:.0f}us")
        lines.append(f"- 180fps budget = {budget_180fps}us → "
                     f"餘裕 {budget_180fps - avg_total_triple:.0f}us "
                     f"({(1 - avg_total_triple/budget_180fps)*100:.1f}%)")
        lines.append("")

    return "\n".join(lines)


def make_histogram(parsed: dict, output: Path, label: str):
    """Create 4-panel figure: timing / GPU breakdown / stutter / per-bucket trend."""
    timing = parsed["timing"]
    gpu = parsed["gpu_prof"]
    if not timing:
        print("No timing data, skipping histogram", file=sys.stderr)
        return

    fig, axes = plt.subplots(2, 2, figsize=(14, 10))
    fig.suptitle(f"FRUC quality — {label}", fontsize=14)

    # Panel 1: percentile timeline
    ax = axes[0, 0]
    bucket_idx = list(range(len(timing)))
    ax.plot(bucket_idx, [b["p50"] for b in timing], "b-o", label="p50", markersize=4)
    ax.plot(bucket_idx, [b["p95"] for b in timing], "g-s", label="p95", markersize=4)
    ax.plot(bucket_idx, [b["p99"] for b in timing], "orange", marker="^", label="p99", markersize=4)
    ax.plot(bucket_idx, [b["p999"] for b in timing], "r-x", label="p99.9", markersize=4)
    ax.set_xlabel("5-sec bucket #")
    ax.set_ylabel("frame interval (ms)")
    ax.set_title("Frame interval percentiles over time")
    ax.legend()
    ax.grid(True, alpha=0.3)

    # Panel 2: stutter score over time
    ax = axes[0, 1]
    stutters = [(b["p95"] - b["p50"]) / b["p50"] for b in timing if b["p50"] > 0]
    p99o = [(b["p99"] - b["p50"]) / b["p50"] for b in timing if b["p50"] > 0]
    ax.plot(bucket_idx[:len(stutters)], stutters, "g-o", label="(p95-p50)/p50", markersize=4)
    ax.plot(bucket_idx[:len(p99o)], p99o, "r-x", label="(p99-p50)/p50", markersize=4)
    ax.axhline(0.1, color="green", linestyle="--", alpha=0.4, label="threshold 0.1 (順)")
    ax.axhline(0.3, color="orange", linestyle="--", alpha=0.4, label="threshold 0.3 (中)")
    ax.set_xlabel("5-sec bucket #")
    ax.set_ylabel("stutter score")
    ax.set_title("Stutter score over time (lower=better)")
    ax.legend()
    ax.grid(True, alpha=0.3)

    # Panel 3: GPU stage breakdown
    ax = axes[1, 0]
    if gpu:
        stages = ["nv12rgb", "me", "median", "warp", "copy"]
        means = [
            sum(g["nv12rgb"] for g in gpu) / len(gpu),
            sum(g["me"] for g in gpu) / len(gpu),
            sum(g["median"] for g in gpu) / len(gpu),
            sum(g["warp"] for g in gpu) / len(gpu),
            sum(g["copy"] for g in gpu) / len(gpu),
        ]
        colors = ["#4287f5", "#42c5f5", "#f5a142", "#f54242", "#a142f5"]
        bars = ax.bar(stages, means, color=colors)
        for bar, val in zip(bars, means):
            ax.text(bar.get_x() + bar.get_width()/2, bar.get_height(),
                    f"{val:.0f}us", ha="center", va="bottom", fontsize=9)
        total = sum(means)
        ax.set_ylabel("microseconds")
        ax.set_title(f"GPU compute chain breakdown (total {total:.0f}us)")
        ax.grid(True, alpha=0.3, axis="y")
    else:
        ax.text(0.5, 0.5, "no GPU-PROF data", ha="center", va="center", transform=ax.transAxes)

    # Panel 4: GPU stage timeline
    ax = axes[1, 1]
    if gpu:
        gpu_idx = list(range(len(gpu)))
        for stage, color in zip(["nv12rgb", "me", "median", "warp", "copy"],
                                 ["#4287f5", "#42c5f5", "#f5a142", "#f54242", "#a142f5"]):
            ax.plot(gpu_idx, [g[stage] for g in gpu], "-o", color=color, label=stage, markersize=3)
        ax.set_xlabel("5-sec bucket #")
        ax.set_ylabel("microseconds")
        ax.set_title("GPU stage timing over time")
        ax.legend(fontsize=9)
        ax.grid(True, alpha=0.3)
    else:
        ax.text(0.5, 0.5, "no GPU-PROF data", ha="center", va="center", transform=ax.transAxes)

    plt.tight_layout()
    plt.savefig(output, dpi=120, bbox_inches="tight")
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(description="VipleStream FRUC timing analyzer (Stage 1)")
    parser.add_argument("log", nargs="?", help="Log file path (default: --latest)")
    parser.add_argument("--latest", action="store_true", help="Auto-find newest VipleStream-*.log")
    parser.add_argument("--label", default=None, help="Label for output file names (default: timestamp)")
    parser.add_argument("--out-dir", default="temp/fruc_quality",
                        help="Output directory (default: temp/fruc_quality)")
    args = parser.parse_args()

    if args.latest or not args.log:
        log_path = find_latest_log()
    else:
        log_path = Path(args.log)
        if not log_path.exists():
            sys.exit(f"Log file not found: {log_path}")

    label = args.label or datetime.now().strftime("%Y%m%d_%H%M%S")
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    md_out = out_dir / f"fruc_quality_{label}.md"
    png_out = out_dir / f"fruc_quality_{label}.png"

    print(f"Parsing {log_path} …")
    parsed = parse_log(log_path)
    print(f"  buckets: timing={len(parsed['timing'])} "
          f"cumul={len(parsed['cumul'])} gpu_prof={len(parsed['gpu_prof'])}")

    report = build_report(parsed, log_path, label)
    md_out.write_text(report, encoding="utf-8")
    print(f"  → {md_out}")

    make_histogram(parsed, png_out, label)
    print(f"  → {png_out}")

    # Print one-line summary to stdout for quick CI consumption
    if parsed["timing"]:
        means = [b["ft_mean"] for b in parsed["timing"]]
        p95s = [b["p95"] for b in parsed["timing"]]
        p50s = [b["p50"] for b in parsed["timing"]]
        avg_stutter = sum((p95s[i] - p50s[i]) / p50s[i] for i in range(len(p50s)) if p50s[i] > 0) / len(p50s)
        print(f"\nSUMMARY  ft_mean={sum(means)/len(means):.2f}ms  "
              f"stutter={avg_stutter:.3f}  "
              f"buckets={len(parsed['timing'])}")


if __name__ == "__main__":
    main()
