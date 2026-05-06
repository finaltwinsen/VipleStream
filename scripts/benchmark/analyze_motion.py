#!/usr/bin/env python3
"""
analyze_motion.py — VipleStream FRUC 補幀效果客觀分析（階段 2）

讀取 capture_fruc_frames.ps1 產生的 raw RGBA frame buffer，做：
  1. UFO trajectory tracking（template matching）— 衡量補幀後 UFO
     位置是否仍走線性軌跡 (R²)
  2. Frame-to-frame pixel-velocity standard deviation — 衡量 motion
     smoothness（理想：constant velocity → std-dev 接近 0）
  3. Optional: optical flow (Farneback) 整體 motion magnitude over
     time — 看 stutter 是否反映在 OF magnitude 跳動

衡量指標：
  - **trajectory R²**：UFO x 位置 vs frame 編號的線性回歸殘差。理想 1.0。
  - **velocity std-dev**：相鄰 frame UFO 位移的標準差，理想 0（等速）
  - **velocity outlier ratio**：> 2σ 的 frame 比例，越低越好

用法：
  python analyze_motion.py capture.bin [--label baseline] [--no-flow]
"""

import argparse
import json
import os
import statistics
import sys
from datetime import datetime
from pathlib import Path

import cv2
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
try:
    from skimage.metrics import structural_similarity as ssim
    HAS_SKIMAGE = True
except ImportError:
    HAS_SKIMAGE = False

# CJK font for figure labels
for _font in ["Microsoft JhengHei", "Microsoft YaHei", "MingLiU", "SimHei"]:
    try:
        from matplotlib import font_manager
        if any(f.name == _font for f in font_manager.fontManager.ttflist):
            matplotlib.rcParams["font.sans-serif"] = [_font, "DejaVu Sans"]
            matplotlib.rcParams["axes.unicode_minus"] = False
            break
    except Exception:
        pass


def load_frames(bin_path: Path, meta: dict) -> np.ndarray:
    """Load raw RGBA frames as numpy array (N, H, W, 4)."""
    w = meta["width"]
    h = meta["height"]
    bpf = w * h * 4  # bytes per frame
    size = bin_path.stat().st_size
    n_frames = size // bpf
    print(f"  loading {n_frames} frames × {w}×{h} from {bin_path.name}")
    raw = np.fromfile(bin_path, dtype=np.uint8, count=n_frames * bpf)
    return raw.reshape((n_frames, h, w, 4))


def to_gray(frames: np.ndarray) -> np.ndarray:
    """Convert RGBA → grayscale (N, H, W) uint8."""
    # ffmpeg writes RGBA byte order: byte 0=R, 1=G, 2=B, 3=A
    # cv2 uses BGR order; use weighted Rec.709-ish formula
    return (0.2126 * frames[..., 0] +
            0.7152 * frames[..., 1] +
            0.0722 * frames[..., 2]).astype(np.uint8)


def find_ufo_template(gray: np.ndarray, frame_idx: int = 30) -> tuple[np.ndarray, tuple[int, int]] | None:
    """
    Find a UFO-like high-contrast region in early frames to use as template.
    Returns (template, (x, y)) or None if no clear template found.

    Strategy: pick the 32×32 patch with highest contrast (std-dev) on
    a frame ~0.5s in (UFO has settled into linear motion by then).
    """
    if frame_idx >= gray.shape[0]:
        frame_idx = gray.shape[0] // 2
    f = gray[frame_idx]
    h, w = f.shape
    patch_size = 32
    if h < patch_size or w < patch_size:
        return None

    # Scan for max-stddev 32×32 patch
    best = (-1, 0, 0)
    step = 8
    for y in range(0, h - patch_size, step):
        for x in range(0, w - patch_size, step):
            patch = f[y:y+patch_size, x:x+patch_size]
            std = patch.std()
            if std > best[0]:
                best = (std, x, y)
    _, bx, by = best
    template = f[by:by+patch_size, bx:bx+patch_size].copy()
    print(f"  template: ({bx},{by}) {patch_size}×{patch_size} stddev={best[0]:.1f}")
    return template, (bx, by)


def track_ufo(gray: np.ndarray, template: np.ndarray, init_pos: tuple[int, int]) -> list[tuple[int, int]]:
    """
    For each frame, template-match to find UFO position.
    Returns list of (x, y) positions, length = N frames.

    Uses a search window around previous position to speed up.
    """
    n_frames = gray.shape[0]
    h, w = gray.shape[1:]
    th, tw = template.shape

    positions = []
    last = init_pos
    for i in range(n_frames):
        f = gray[i]
        # full-frame search (sub-region is small enough that this is fine)
        res = cv2.matchTemplate(f, template, cv2.TM_CCOEFF_NORMED)
        _, max_val, _, max_loc = cv2.minMaxLoc(res)
        if max_val < 0.4:
            # bad match — use last known position
            positions.append(last)
        else:
            positions.append(max_loc)
            last = max_loc
    return positions


def linear_regression_r2(x: np.ndarray, y: np.ndarray) -> tuple[float, float, float]:
    """OLS linear fit y = m*x + b, return (m, b, R²)."""
    if len(x) < 2:
        return 0.0, 0.0, 0.0
    m, b = np.polyfit(x, y, 1)
    y_pred = m * x + b
    ss_res = np.sum((y - y_pred) ** 2)
    ss_tot = np.sum((y - np.mean(y)) ** 2)
    r2 = 1.0 - ss_res / ss_tot if ss_tot > 0 else 0.0
    return float(m), float(b), float(r2)


def compute_velocity_metrics(positions: list[tuple[int, int]], fps: int) -> dict:
    """
    From UFO position track, compute pixel-velocity per frame and stats.
    """
    if len(positions) < 2:
        return {}
    xs = np.array([p[0] for p in positions])
    ys = np.array([p[1] for p in positions])

    # Frame-to-frame displacement
    dx = np.diff(xs)
    dy = np.diff(ys)
    speed = np.sqrt(dx ** 2 + dy ** 2)  # pixels per frame

    # In testufo, motion is mostly horizontal — use dx as primary
    dx_mean = float(np.mean(dx))
    dx_std = float(np.std(dx))
    speed_mean = float(np.mean(speed))
    speed_std = float(np.std(speed))

    # Outliers: frame where speed deviates > 2σ from mean
    if speed_std > 0:
        outlier_mask = np.abs(speed - speed_mean) > 2 * speed_std
        outlier_ratio = float(np.sum(outlier_mask) / len(speed))
        # Worst outlier
        worst_idx = int(np.argmax(np.abs(speed - speed_mean)))
        worst_dev = float(np.abs(speed[worst_idx] - speed_mean))
    else:
        outlier_ratio = 0.0
        worst_idx = 0
        worst_dev = 0.0

    # Linear trajectory R²
    frame_idx = np.arange(len(xs))
    m_x, b_x, r2_x = linear_regression_r2(frame_idx.astype(float), xs.astype(float))
    m_y, b_y, r2_y = linear_regression_r2(frame_idx.astype(float), ys.astype(float))

    return {
        "n_frames": len(positions),
        "fps": fps,
        "dx_mean_px_per_frame": dx_mean,
        "dx_std_px_per_frame": dx_std,
        "speed_mean_px_per_frame": speed_mean,
        "speed_std_px_per_frame": speed_std,
        "outlier_ratio_2sigma": outlier_ratio,
        "worst_outlier_frame": worst_idx,
        "worst_outlier_deviation_px": worst_dev,
        "trajectory_x_slope_px_per_frame": m_x,
        "trajectory_x_r2": r2_x,
        "trajectory_y_slope_px_per_frame": m_y,
        "trajectory_y_r2": r2_y,
        # Convert to physical: speed × fps = px/sec
        "speed_mean_px_per_sec": speed_mean * fps,
    }


def compute_optical_flow_magnitude(gray: np.ndarray, sample_step: int = 6) -> list[float]:
    """
    Per-frame mean OF magnitude (Farneback).
    Sample every Nth frame for speed.
    """
    n = gray.shape[0]
    mags = []
    prev = None
    for i in range(0, n, sample_step):
        f = gray[i]
        if prev is None:
            prev = f
            continue
        flow = cv2.calcOpticalFlowFarneback(
            prev, f, None,
            pyr_scale=0.5, levels=3, winsize=15,
            iterations=2, poly_n=5, poly_sigma=1.1, flags=0
        )
        mag = np.sqrt(flow[..., 0] ** 2 + flow[..., 1] ** 2)
        mags.append(float(mag.mean()))
        prev = f
    return mags


def build_report(metrics: dict, of_mags: list[float] | None,
                 capture_path: Path, label: str, meta: dict) -> str:
    lines = []
    lines.append(f"# FRUC motion analysis — `{label}`")
    lines.append("")
    lines.append(f"- **Capture**: `{capture_path}`")
    lines.append(f"- **Generated**: {datetime.now().isoformat(timespec='seconds')}")
    lines.append(f"- **Region**: {meta['width']}×{meta['height']} @ ({meta['x']},{meta['y']}) "
                 f"@ {meta['fps']}fps × {meta['seconds']}s")
    lines.append(f"- **Frames analyzed**: {metrics.get('n_frames', 0)}")
    lines.append("")

    if not metrics:
        lines.append("⚠️ No motion data extracted (template not found or too few frames).")
        return "\n".join(lines)

    # ---- Trajectory linearity ----
    lines.append("## Trajectory linearity (UFO x vs frame#)")
    lines.append("")
    lines.append("理想：補幀後 UFO 應該完美線性軌跡 (R²=1.0)。每偏離 1.0 越遠表示")
    lines.append("補幀的中間 frame 位置偏離理想位置。")
    lines.append("")
    lines.append("| 軸 | slope (px/frame) | R² | 解讀 |")
    lines.append("|---|---|---|---|")
    r2_x = metrics["trajectory_x_r2"]
    r2_y = metrics["trajectory_y_r2"]
    lines.append(f"| x | {metrics['trajectory_x_slope_px_per_frame']:.3f} | {r2_x:.4f} | "
                 f"{'✅ 完美線性' if r2_x > 0.999 else '⚠️ 線性偏差' if r2_x > 0.99 else '❌ 不線性'} |")
    lines.append(f"| y | {metrics['trajectory_y_slope_px_per_frame']:.3f} | {r2_y:.4f} | "
                 f"{'✅ 完美水平' if r2_y > 0.99 or abs(metrics['trajectory_y_slope_px_per_frame']) < 0.1 else '⚠️ y 軸 drift'} |")
    lines.append("")
    lines.append(f"- speed_mean = {metrics['speed_mean_px_per_frame']:.3f} px/frame "
                 f"= {metrics['speed_mean_px_per_sec']:.1f} px/sec")
    lines.append("")

    # ---- Velocity smoothness ----
    lines.append("## Velocity smoothness (frame-to-frame displacement)")
    lines.append("")
    lines.append("理想：等速移動 → speed_std 接近 0、outlier_ratio 接近 0%。")
    lines.append("")
    lines.append("| 指標 | 值 | 解讀 |")
    lines.append("|---|---|---|")
    sd = metrics["speed_std_px_per_frame"]
    sm = metrics["speed_mean_px_per_frame"]
    cv = sd / sm if sm > 0 else 0
    lines.append(f"| speed_std | {sd:.3f} px/frame | "
                 f"{'✅ 平滑' if cv < 0.1 else '⚠️ 中等' if cv < 0.3 else '❌ 跳動'} (cv={cv:.3f}) |")
    or_pct = metrics['outlier_ratio_2sigma'] * 100
    lines.append(f"| outlier_ratio (>2σ) | {or_pct:.2f}% | "
                 f"{'✅ 無 outlier' if or_pct < 1 else '⚠️ 有 outlier' if or_pct < 5 else '❌ 嚴重'} |")
    lines.append(f"| worst_outlier | frame #{metrics['worst_outlier_frame']} ({metrics['worst_outlier_deviation_px']:.1f} px deviation) | — |")
    lines.append("")

    # ---- OF magnitude (optional) ----
    if of_mags:
        of_arr = np.array(of_mags)
        lines.append("## Optical flow magnitude (sampled)")
        lines.append("")
        lines.append("整體 frame-to-frame motion magnitude — 應該大致 constant，spike 對應 stutter。")
        lines.append("")
        lines.append(f"- mean = {of_arr.mean():.3f}")
        lines.append(f"- std-dev = {of_arr.std():.3f}")
        lines.append(f"- cv (std/mean) = {of_arr.std()/of_arr.mean() if of_arr.mean() > 0 else 0:.3f}")
        lines.append("")

    return "\n".join(lines)


def make_figure(positions: list, metrics: dict, of_mags: list[float] | None,
                output: Path, label: str):
    n = len(positions)
    if n < 2:
        return

    xs = np.array([p[0] for p in positions])
    ys = np.array([p[1] for p in positions])
    frame_idx = np.arange(n)

    fig, axes = plt.subplots(2, 2, figsize=(14, 10))
    fig.suptitle(f"FRUC motion analysis — {label}", fontsize=14)

    # Panel 1: x trajectory + linear fit
    ax = axes[0, 0]
    ax.plot(frame_idx, xs, "b.", markersize=2, alpha=0.5, label="UFO x position")
    m = metrics["trajectory_x_slope_px_per_frame"]
    b = xs[0] - m * 0  # rough intercept
    fit_x = m * frame_idx + b
    ax.plot(frame_idx, fit_x, "r-", linewidth=1, alpha=0.8, label=f"linear fit (R²={metrics['trajectory_x_r2']:.4f})")
    ax.set_xlabel("frame #")
    ax.set_ylabel("x position (px)")
    ax.set_title("Trajectory: UFO x position over frames")
    ax.legend()
    ax.grid(True, alpha=0.3)

    # Panel 2: residual from linear fit (deviation from ideal)
    ax = axes[0, 1]
    residuals = xs - fit_x
    ax.plot(frame_idx, residuals, "g.", markersize=2)
    ax.axhline(0, color="black", linewidth=0.5)
    ax.set_xlabel("frame #")
    ax.set_ylabel("x residual from linear fit (px)")
    ax.set_title("Trajectory deviation (lower abs = smoother)")
    ax.grid(True, alpha=0.3)

    # Panel 3: per-frame velocity histogram
    ax = axes[1, 0]
    if n >= 2:
        dx = np.diff(xs)
        ax.hist(dx, bins=50, color="purple", alpha=0.7, edgecolor="black")
        mean_dx = metrics["dx_mean_px_per_frame"]
        std_dx = metrics["dx_std_px_per_frame"]
        ax.axvline(mean_dx, color="red", linestyle="--", label=f"mean={mean_dx:.2f}")
        ax.axvline(mean_dx + 2*std_dx, color="orange", linestyle="--", label=f"+2σ")
        ax.axvline(mean_dx - 2*std_dx, color="orange", linestyle="--", label=f"-2σ")
        ax.set_xlabel("dx (px/frame)")
        ax.set_ylabel("frame count")
        ax.set_title(f"Velocity histogram (std={std_dx:.2f} px/frame)")
        ax.legend()
        ax.grid(True, alpha=0.3)

    # Panel 4: optical flow magnitude over time
    ax = axes[1, 1]
    if of_mags:
        ax.plot(np.arange(len(of_mags)), of_mags, "navy", linewidth=0.8)
        ax.set_xlabel("sampled frame #")
        ax.set_ylabel("mean OF magnitude")
        ax.set_title("Optical flow magnitude (motion ‘pulse’)")
        ax.grid(True, alpha=0.3)
    else:
        ax.text(0.5, 0.5, "OF skipped (--no-flow)", ha="center", va="center", transform=ax.transAxes)

    plt.tight_layout()
    plt.savefig(output, dpi=120, bbox_inches="tight")
    plt.close(fig)


# =============================================================================
# Video mode (PotPlayer / natural video) — 4 indicators
# =============================================================================

def detect_scene_cuts(gray: np.ndarray, ssim_threshold: float = 0.30) -> np.ndarray:
    """
    Returns boolean array (length N): True = frame i is a scene-cut frame
    relative to frame i-1.  i==0 always False.

    Strategy: SSIM(t, t-1) below threshold = cut.  Falls back to mean abs
    diff > 60 (out of 255) if skimage missing.
    """
    n = gray.shape[0]
    cuts = np.zeros(n, dtype=bool)
    if n < 2:
        return cuts

    # Downsample for speed (SSIM on 320×180 is OK but faster on 80×45)
    h, w = gray.shape[1:]
    ds_h = max(45, h // 4)
    ds_w = max(80, w // 4)
    downs = np.zeros((n, ds_h, ds_w), dtype=np.uint8)
    for i in range(n):
        downs[i] = cv2.resize(gray[i], (ds_w, ds_h), interpolation=cv2.INTER_AREA)

    if HAS_SKIMAGE:
        for i in range(1, n):
            s = ssim(downs[i-1], downs[i], data_range=255)
            if s < ssim_threshold:
                cuts[i] = True
    else:
        # fallback — mean absolute diff
        for i in range(1, n):
            d = float(np.mean(np.abs(downs[i].astype(np.int16) - downs[i-1].astype(np.int16))))
            if d > 60:  # large jump = cut
                cuts[i] = True

    return cuts


def compute_ssim_series(gray: np.ndarray) -> np.ndarray:
    """Per-frame SSIM(t, t-1).  ssim[0] = NaN."""
    n = gray.shape[0]
    out = np.full(n, np.nan, dtype=np.float64)
    if not HAS_SKIMAGE or n < 2:
        return out
    h, w = gray.shape[1:]
    # SSIM on full sub-region resolution (320×180 is small)
    for i in range(1, n):
        out[i] = ssim(gray[i-1], gray[i], data_range=255)
    return out


def compute_of_magnitude_series(gray: np.ndarray) -> np.ndarray:
    """Per-frame mean optical flow magnitude (Farneback).  out[0] = NaN."""
    n = gray.shape[0]
    out = np.full(n, np.nan, dtype=np.float64)
    if n < 2:
        return out
    for i in range(1, n):
        flow = cv2.calcOpticalFlowFarneback(
            gray[i-1], gray[i], None,
            pyr_scale=0.5, levels=2, winsize=15,
            iterations=2, poly_n=5, poly_sigma=1.1, flags=0,
        )
        out[i] = float(np.sqrt(flow[..., 0] ** 2 + flow[..., 1] ** 2).mean())
    return out


def block_outlier_ratio_series(gray: np.ndarray, block: int = 16) -> np.ndarray:
    """
    For each frame pair (t-1, t): per-block OF magnitude, count blocks
    deviating > 2σ from frame median, return outlier ratio per frame.
    out[0] = NaN.
    """
    n = gray.shape[0]
    out = np.full(n, np.nan, dtype=np.float64)
    if n < 2:
        return out
    h, w = gray.shape[1:]
    nby = h // block
    nbx = w // block
    if nby < 2 or nbx < 2:
        return out

    for i in range(1, n):
        flow = cv2.calcOpticalFlowFarneback(
            gray[i-1], gray[i], None,
            pyr_scale=0.5, levels=2, winsize=15,
            iterations=2, poly_n=5, poly_sigma=1.1, flags=0,
        )
        mag = np.sqrt(flow[..., 0] ** 2 + flow[..., 1] ** 2)
        # per-block mean
        block_means = np.zeros(nby * nbx)
        for by in range(nby):
            for bx in range(nbx):
                block_means[by * nbx + bx] = mag[
                    by*block:(by+1)*block,
                    bx*block:(bx+1)*block,
                ].mean()
        med = np.median(block_means)
        std = np.std(block_means)
        if std > 0:
            outliers = np.abs(block_means - med) > 2 * std
            out[i] = float(outliers.sum() / len(block_means))
        else:
            out[i] = 0.0
    return out


def fft_alternation_power(series: np.ndarray, fps: int, target_freq_hz: float = 30.0,
                           band_hz: float = 1.0) -> dict:
    """
    Compute FFT of time series, return power around target_freq_hz vs total.
    Higher target peak ratio = stronger alternation (= worse interp quality).

    For 60fps capture with dual-present (real, interp, real, interp ...),
    if interp differs from real, frame-to-frame metric oscillates at 30 Hz.
    """
    s = series[~np.isnan(series)]
    s = s - np.mean(s)  # detrend
    if len(s) < 8:
        return {"target_power_ratio": 0.0, "target_freq_hz": target_freq_hz,
                "peak_freq_hz": 0.0, "peak_power_ratio": 0.0}

    n = len(s)
    fft = np.fft.rfft(s * np.hanning(n))
    freqs = np.fft.rfftfreq(n, d=1.0/fps)
    power = np.abs(fft) ** 2
    total = power.sum() if power.sum() > 0 else 1.0

    # target band
    in_band = (freqs >= target_freq_hz - band_hz) & (freqs <= target_freq_hz + band_hz)
    target_power = float(power[in_band].sum() / total) if in_band.any() else 0.0

    # peak (excluding DC)
    if len(power) > 1:
        peak_idx = 1 + int(np.argmax(power[1:]))
        peak_freq = float(freqs[peak_idx])
        peak_power_ratio = float(power[peak_idx] / total)
    else:
        peak_freq = 0.0
        peak_power_ratio = 0.0

    return {
        "target_freq_hz": target_freq_hz,
        "target_power_ratio": target_power,
        "peak_freq_hz": peak_freq,
        "peak_power_ratio": peak_power_ratio,
    }


def filter_series_by_cuts(series: np.ndarray, cuts: np.ndarray, buffer: int = 1) -> np.ndarray:
    """Mask out scene-cut frames + buffer neighbours, return NaN-filled copy."""
    n = len(series)
    mask = np.zeros(n, dtype=bool)
    cut_idx = np.where(cuts)[0]
    for ci in cut_idx:
        for j in range(max(0, ci - buffer), min(n, ci + buffer + 1)):
            mask[j] = True
    out = series.copy()
    out[mask] = np.nan
    return out


def video_mode_analyze(frames: np.ndarray, gray: np.ndarray, fps: int) -> tuple[dict, dict]:
    """
    4-indicator pipeline for natural video content (PotPlayer / general).
    Returns (metrics_dict, series_dict for plotting).
    """
    print("  detecting scene cuts …")
    cuts = detect_scene_cuts(gray)
    n_cuts = int(cuts.sum())
    print(f"    found {n_cuts} scene cuts")

    print("  computing per-frame SSIM(t, t-1) …")
    ssim_series = compute_ssim_series(gray)

    print("  computing per-frame OF magnitude …")
    of_series = compute_of_magnitude_series(gray)

    print("  computing per-frame block-OF outlier ratio …")
    block_series = block_outlier_ratio_series(gray)

    # Apply scene-cut filter
    of_filt = filter_series_by_cuts(of_series, cuts, buffer=1)
    ssim_filt = filter_series_by_cuts(ssim_series, cuts, buffer=1)
    block_filt = filter_series_by_cuts(block_series, cuts, buffer=1)

    def _stats(arr):
        a = arr[~np.isnan(arr)]
        if len(a) == 0:
            return {"n": 0, "mean": 0, "std": 0, "cv": 0,
                    "outlier_2s_ratio": 0.0, "min": 0, "max": 0}
        m = float(np.mean(a))
        s = float(np.std(a))
        outl = float(np.sum(np.abs(a - m) > 2 * s) / len(a)) if s > 0 else 0.0
        return {"n": int(len(a)), "mean": m, "std": s,
                "cv": s / m if m > 0 else 0,
                "outlier_2s_ratio": outl,
                "min": float(a.min()), "max": float(a.max())}

    of_stats = _stats(of_filt)
    ssim_stats = _stats(ssim_filt)
    block_stats = _stats(block_filt)

    # FFT alternation analysis (use unfiltered series; FFT itself handles spikes)
    of_fft = fft_alternation_power(of_series, fps)
    ssim_fft = fft_alternation_power(ssim_series, fps)

    metrics = {
        "n_frames": int(gray.shape[0]),
        "n_scene_cuts": n_cuts,
        "scene_cut_rate_pct": float(n_cuts / gray.shape[0] * 100),
        # Indicator 1: OF magnitude smoothness
        "of_magnitude": of_stats,
        # Indicator 2: OF FFT alternation
        "of_fft": of_fft,
        # Indicator 3: SSIM continuity
        "ssim": ssim_stats,
        "ssim_fft": ssim_fft,
        # Indicator 4: block OF outlier
        "block_outlier_ratio": block_stats,
    }
    series = {
        "scene_cuts": cuts.tolist(),
        "of": of_series.tolist(),
        "of_filt": of_filt.tolist(),
        "ssim": ssim_series.tolist(),
        "ssim_filt": ssim_filt.tolist(),
        "block_outlier": block_series.tolist(),
    }
    return metrics, series


def build_video_report(metrics: dict, capture_path: Path, label: str, meta: dict) -> str:
    lines = []
    lines.append(f"# FRUC video-mode quality — `{label}`")
    lines.append("")
    lines.append(f"- **Capture**: `{capture_path}`")
    lines.append(f"- **Generated**: {datetime.now().isoformat(timespec='seconds')}")
    lines.append(f"- **Region**: {meta['width']}×{meta['height']} @ ({meta['x']},{meta['y']}) "
                 f"@ {meta['fps']}fps × {meta['seconds']}s")
    lines.append(f"- **Frames analyzed**: {metrics['n_frames']}")
    lines.append(f"- **Scene cuts detected**: {metrics['n_scene_cuts']} "
                 f"({metrics['scene_cut_rate_pct']:.1f}%)")
    if metrics['n_scene_cuts'] > 0:
        lines.append(f"  - cut frames + 1-frame buffer excluded from smoothness metrics")
    lines.append("")

    lines.append("## Indicator 1 — OF magnitude smoothness（filtered, 越平滑越好）")
    lines.append("")
    s = metrics["of_magnitude"]
    cv_verdict = "✅ 平滑" if s["cv"] < 0.3 else ("⚠️ 中等" if s["cv"] < 0.6 else "❌ 不穩")
    outlier_verdict = "✅" if s["outlier_2s_ratio"] < 0.05 else ("⚠️" if s["outlier_2s_ratio"] < 0.15 else "❌")
    lines.append(f"- mean = {s['mean']:.3f} px/frame   range [{s['min']:.3f}, {s['max']:.3f}]")
    lines.append(f"- std-dev = {s['std']:.3f}   **cv = {s['cv']:.3f}** ({cv_verdict})")
    lines.append(f"- 2σ outlier ratio = {s['outlier_2s_ratio']*100:.2f}% ({outlier_verdict})")
    lines.append("")

    lines.append("## Indicator 2 — OF FFT alternation @ 30Hz（補幀差會出現此 spike）")
    lines.append("")
    f = metrics["of_fft"]
    of_30hz = f["target_power_ratio"] * 100
    of_30hz_verdict = "✅ 無 30Hz spike" if of_30hz < 1 else ("⚠️ 中等" if of_30hz < 3 else "❌ 強 alternation")
    lines.append(f"- 30Hz band power / total = **{of_30hz:.3f}%** ({of_30hz_verdict})")
    lines.append(f"- overall peak: {f['peak_freq_hz']:.2f}Hz @ {f['peak_power_ratio']*100:.3f}% of total")
    lines.append("")

    lines.append("## Indicator 3 — SSIM(t, t-1) frame continuity")
    lines.append("")
    s = metrics["ssim"]
    ssim_verdict = "✅ 高連續性" if s["mean"] > 0.95 else ("⚠️ 中等" if s["mean"] > 0.85 else "❌ 低連續性")
    lines.append(f"- mean SSIM = **{s['mean']:.4f}**   range [{s['min']:.4f}, {s['max']:.4f}] ({ssim_verdict})")
    lines.append(f"- std-dev = {s['std']:.4f}")
    lines.append(f"- 2σ outlier ratio = {s['outlier_2s_ratio']*100:.2f}%")
    lines.append("")

    lines.append("## Indicator 3b — SSIM FFT @ 30Hz（同樣，補幀差會 alternate）")
    lines.append("")
    f = metrics["ssim_fft"]
    sf_30hz = f["target_power_ratio"] * 100
    sf_30hz_verdict = "✅ 無" if sf_30hz < 1 else ("⚠️" if sf_30hz < 3 else "❌")
    lines.append(f"- 30Hz band power / total = **{sf_30hz:.3f}%** ({sf_30hz_verdict})")
    lines.append(f"- overall peak: {f['peak_freq_hz']:.2f}Hz @ {f['peak_power_ratio']*100:.3f}%")
    lines.append("")

    lines.append("## Indicator 4 — Block-level OF outlier ratio")
    lines.append("")
    b = metrics["block_outlier_ratio"]
    block_verdict = "✅ 一致 motion" if b["mean"] < 0.10 else ("⚠️" if b["mean"] < 0.20 else "❌ 嚴重")
    lines.append(f"- mean outlier block ratio = **{b['mean']*100:.2f}%** ({block_verdict})")
    lines.append(f"- std-dev = {b['std']*100:.2f}%   range [{b['min']*100:.2f}%, {b['max']*100:.2f}%]")
    lines.append("")

    lines.append("## Verdict 概覽（4 個指標的綜合判定）")
    lines.append("")
    of_smooth_pass = metrics["of_magnitude"]["cv"] < 0.6
    of_fft_pass = metrics["of_fft"]["target_power_ratio"] < 0.03  # < 3% at 30Hz
    ssim_pass = metrics["ssim"]["mean"] > 0.85
    block_pass = metrics["block_outlier_ratio"]["mean"] < 0.20
    passes = sum([of_smooth_pass, of_fft_pass, ssim_pass, block_pass])
    lines.append(f"| 指標 | 判定 |")
    lines.append(f"|---|---|")
    lines.append(f"| OF smoothness (cv < 0.6) | {'✅' if of_smooth_pass else '❌'} |")
    lines.append(f"| OF 30Hz alt. (< 3%) | {'✅' if of_fft_pass else '❌'} |")
    lines.append(f"| SSIM continuity (> 0.85) | {'✅' if ssim_pass else '❌'} |")
    lines.append(f"| Block outlier (< 20%) | {'✅' if block_pass else '❌'} |")
    lines.append(f"| **總分** | **{passes} / 4** |")
    lines.append("")

    return "\n".join(lines)


def make_video_figure(series: dict, metrics: dict, output: Path, label: str, fps: int):
    cuts = np.array(series["scene_cuts"])
    of = np.array(series["of"])
    ssim_s = np.array(series["ssim"])
    block_s = np.array(series["block_outlier"])
    n = len(of)
    idx = np.arange(n)

    fig, axes = plt.subplots(2, 2, figsize=(14, 10))
    fig.suptitle(f"FRUC video-mode quality — {label}", fontsize=14)

    # Panel 1: OF magnitude time series
    ax = axes[0, 0]
    ax.plot(idx, of, "navy", linewidth=0.6, alpha=0.7, label="raw")
    ax.plot(idx, np.array(series["of_filt"]), "blue", linewidth=0.9, label="filtered")
    cut_idx = np.where(cuts)[0]
    for ci in cut_idx:
        ax.axvline(ci, color="red", alpha=0.3, linewidth=0.5)
    ax.set_xlabel("frame #")
    ax.set_ylabel("mean OF magnitude")
    ax.set_title(f"OF magnitude over time (red = scene cut, n={int(cuts.sum())})")
    ax.legend()
    ax.grid(True, alpha=0.3)

    # Panel 2: SSIM time series
    ax = axes[0, 1]
    ax.plot(idx, ssim_s, "green", linewidth=0.6, alpha=0.7, label="raw")
    ax.plot(idx, np.array(series["ssim_filt"]), "darkgreen", linewidth=0.9, label="filtered")
    for ci in cut_idx:
        ax.axvline(ci, color="red", alpha=0.3, linewidth=0.5)
    ax.set_xlabel("frame #")
    ax.set_ylabel("SSIM(t, t-1)")
    ax.set_title("SSIM frame continuity")
    ax.legend()
    ax.grid(True, alpha=0.3)

    # Panel 3: FFT power spectrum (OF)
    ax = axes[1, 0]
    of_clean = of[~np.isnan(of)]
    if len(of_clean) >= 8:
        s = of_clean - np.mean(of_clean)
        fft = np.abs(np.fft.rfft(s * np.hanning(len(s)))) ** 2
        freqs = np.fft.rfftfreq(len(s), d=1.0/fps)
        ax.semilogy(freqs[1:], fft[1:], "purple", linewidth=0.8)
        ax.axvline(30, color="red", linestyle="--", alpha=0.6, label="30Hz (real/interp alt.)")
        ax.set_xlabel("freq (Hz)")
        ax.set_ylabel("power (log)")
        ax.set_title(f"OF magnitude FFT — 30Hz band {metrics['of_fft']['target_power_ratio']*100:.2f}%")
        ax.legend()
        ax.grid(True, alpha=0.3, which="both")

    # Panel 4: Block outlier ratio
    ax = axes[1, 1]
    ax.plot(idx, block_s, "orange", linewidth=0.7, alpha=0.7)
    for ci in cut_idx:
        ax.axvline(ci, color="red", alpha=0.3, linewidth=0.5)
    mean_b = metrics["block_outlier_ratio"]["mean"]
    ax.axhline(mean_b, color="darkorange", linestyle="--", alpha=0.7,
               label=f"mean {mean_b*100:.1f}%")
    ax.set_xlabel("frame #")
    ax.set_ylabel("outlier block ratio (16×16 grid)")
    ax.set_title("Block-level OF outlier ratio")
    ax.legend()
    ax.grid(True, alpha=0.3)

    plt.tight_layout()
    plt.savefig(output, dpi=120, bbox_inches="tight")
    plt.close(fig)


# =============================================================================


def main():
    parser = argparse.ArgumentParser(description="FRUC motion analysis (Stage 2)")
    parser.add_argument("capture", help="Raw RGBA capture file (.bin)")
    parser.add_argument("--label", default=None)
    parser.add_argument("--mode", choices=["ufo", "video"], default="video",
                        help="ufo: testufo trajectory R²; video: 4 indicators for natural video (default)")
    parser.add_argument("--no-flow", action="store_true",
                        help="(ufo mode only) Skip optical flow magnitude")
    parser.add_argument("--out-dir", default="temp/fruc_quality")
    args = parser.parse_args()

    cap_path = Path(args.capture)
    meta_path = cap_path.with_suffix(".json")
    if not cap_path.exists():
        sys.exit(f"Capture not found: {cap_path}")
    if not meta_path.exists():
        sys.exit(f"Metadata not found: {meta_path}")

    meta = json.loads(meta_path.read_text(encoding="utf-8"))
    label = args.label or cap_path.stem

    print(f"Analyzing {cap_path} (mode={args.mode})")
    frames = load_frames(cap_path, meta)
    gray = to_gray(frames)
    print(f"  shape: {gray.shape}")

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    md_out = out_dir / f"motion_{label}.md"
    png_out = out_dir / f"motion_{label}.png"
    json_out = out_dir / f"motion_{label}.json"

    if args.mode == "ufo":
        # ===== UFO trajectory mode (testufo content) =====
        tpl_result = find_ufo_template(gray)
        if tpl_result is None:
            sys.exit("Could not find UFO template (frames too small or empty).")
        template, init_pos = tpl_result

        print("  tracking UFO …")
        positions = track_ufo(gray, template, init_pos)
        metrics = compute_velocity_metrics(positions, meta["fps"])

        of_mags = None
        if not args.no_flow:
            print("  computing optical flow magnitude (sampled) …")
            of_mags = compute_optical_flow_magnitude(gray)

        report = build_report(metrics, of_mags, cap_path, label, meta)
        md_out.write_text(report, encoding="utf-8")
        print(f"  -> {md_out}")

        make_figure(positions, metrics, of_mags, png_out, label)
        print(f"  -> {png_out}")

        json_out.write_text(json.dumps({
            "mode": "ufo",
            "metrics": metrics,
            "of_magnitude_samples": of_mags,
        }, indent=2), encoding="utf-8")
        print(f"  -> {json_out}")

        print(f"\nSUMMARY  R^2_x={metrics['trajectory_x_r2']:.4f}  "
              f"speed_std={metrics['speed_std_px_per_frame']:.3f}  "
              f"outlier_2sigma={metrics['outlier_ratio_2sigma']*100:.2f}%")
        return

    # ===== video mode (natural video / PotPlayer / general) =====
    metrics, series = video_mode_analyze(frames, gray, meta["fps"])

    report = build_video_report(metrics, cap_path, label, meta)
    md_out.write_text(report, encoding="utf-8")
    print(f"  -> {md_out}")

    make_video_figure(series, metrics, png_out, label, meta["fps"])
    print(f"  -> {png_out}")

    # JSON without the full series (too large) — keep just metrics
    json_out.write_text(json.dumps({"mode": "video", "metrics": metrics}, indent=2),
                        encoding="utf-8")
    print(f"  -> {json_out}")

    of_30hz = metrics["of_fft"]["target_power_ratio"] * 100
    print(f"\nSUMMARY  ssim_mean={metrics['ssim']['mean']:.4f}  "
          f"of_cv={metrics['of_magnitude']['cv']:.3f}  "
          f"of_30Hz={of_30hz:.2f}%  "
          f"block_outlier={metrics['block_outlier_ratio']['mean']*100:.1f}%  "
          f"cuts={metrics['n_scene_cuts']}")


if __name__ == "__main__":
    main()
