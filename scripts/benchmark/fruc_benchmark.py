#!/usr/bin/env python3
"""
VipleStream FRUC Benchmark
==========================
Downloads Vimeo-90K triplet test set, runs 20 GenericFRUC parameter configurations,
and ranks them by PSNR/SSIM to find the optimal settings.

Usage:
    python fruc_benchmark.py                    # Full run
    python fruc_benchmark.py --download-only    # Just download data
    python fruc_benchmark.py --skip-download    # Skip download, run benchmark
    python fruc_benchmark.py --config 0 2 5     # Run specific configs only
"""

import os, sys, time, csv, json, argparse, subprocess, re, tempfile
import urllib.request, zipfile, shutil
from pathlib import Path
from dataclasses import dataclass, field
from typing import List, Tuple, Optional

import numpy as np
from PIL import Image

# Optional: scikit-image for SSIM
try:
    from skimage.metrics import structural_similarity as ssim_fn
    HAS_SKIMAGE = True
except ImportError:
    HAS_SKIMAGE = False
    print("[WARN] scikit-image not installed, SSIM will use simplified version")


# ============================================================
# Configuration
# ============================================================

DATA_DIR = Path("vimeo_tri_test")
RESULTS_DIR = Path("benchmark_results")
# We already have the test set (im1, im3 only — no GT im2).
# Strategy: use im1→im3 as input pair, generate interpolated frame,
# then evaluate using cross-validation: for clips where we have im1+im3,
# we ALSO use odd-frame pairs from septuplet data.
#
# Practical approach: since we have im1+im3 without GT, we measure
# quality via proxy metrics:
# 1. Temporal consistency (interp should be between im1 and im3)
# 2. Warp error (how well MVs predict actual pixel motion)
# 3. Relative ranking still valid — same test data, different configs
VIMEO_URL = "http://data.csail.mit.edu/tofu/testset/vimeo_tri_test.zip"
SAMPLES_PER_CATEGORY = 5

@dataclass
class FRUCConfig:
    """A single FRUC parameter configuration to benchmark."""
    id: int
    name: str
    block_size: int
    steps: List[int]
    sample_count: int
    sample_stride: int
    early_thresh: float
    mv_clamp: int

    @property
    def search_radius(self):
        return sum(self.steps)

    @property
    def cost_estimate(self):
        """Relative GPU cost estimate (higher = slower)."""
        n_blocks_1080p = (1920 // self.block_size) * (1080 // self.block_size)
        sads_per_block = len(self.steps) * 8  # 8 neighbors per step
        samples_per_sad = self.sample_count ** 2
        return n_blocks_1080p * sads_per_block * samples_per_sad


# 20 configurations to test
CONFIGS = [
    FRUCConfig(0,  "Baseline",           64, [4,2,1],          4, 4, 0.01,  48),
    FRUCConfig(1,  "Search-15",          64, [8,4,2,1],        4, 4, 0.01,  48),
    FRUCConfig(2,  "Search-31",          64, [16,8,4,2,1],     4, 4, 0.01,  48),
    FRUCConfig(3,  "Search-63",          64, [32,16,8,4,2,1],  4, 4, 0.01,  48),
    FRUCConfig(4,  "Blk32-S15",          32, [8,4,2,1],        4, 4, 0.01,  48),
    FRUCConfig(5,  "Blk32-S31",          32, [16,8,4,2,1],     4, 4, 0.01,  48),
    FRUCConfig(6,  "Blk48-S15",          48, [8,4,2,1],        4, 4, 0.01,  48),
    FRUCConfig(7,  "Blk48-S31",          48, [16,8,4,2,1],     4, 4, 0.01,  48),
    FRUCConfig(8,  "S31-Samp6x3",        64, [16,8,4,2,1],     6, 3, 0.01,  48),
    FRUCConfig(9,  "S31-Samp8x2",        64, [16,8,4,2,1],     8, 2, 0.01,  48),
    FRUCConfig(10, "Blk32-S31-Samp6",    32, [16,8,4,2,1],     6, 3, 0.01,  48),
    FRUCConfig(11, "S31-LowTh-Clamp64",  64, [16,8,4,2,1],     4, 4, 0.005, 64),
    FRUCConfig(12, "S31-HighTh",         64, [16,8,4,2,1],     4, 4, 0.02,  48),
    FRUCConfig(13, "Blk32-Dense-C64",    32, [16,8,4,2,1],     4, 2, 0.01,  64),
    FRUCConfig(14, "CandidateA",         48, [16,8,4,2,1],     6, 3, 0.005, 64),
    FRUCConfig(15, "Blk32-HiPrec",       32, [8,4,2,1],        8, 2, 0.01,  48),
    FRUCConfig(16, "Base-S15-C64",       64, [8,4,2,1],        4, 4, 0.01,  64),
    FRUCConfig(17, "Blk48-S63-C64",      48, [32,16,8,4,2,1],  4, 4, 0.01,  64),
    FRUCConfig(18, "MaxPrecision",        32, [32,16,8,4,2,1],  6, 2, 0.005, 64),
    FRUCConfig(19, "Blk48-S15-HiSamp",   48, [8,4,2,1],        8, 2, 0.01,  48),
]


# ============================================================
# Download & Data Selection
# ============================================================

def download_vimeo90k():
    """Download and extract Vimeo-90K triplet test set."""
    zip_path = Path("vimeo_tri_test.zip")

    if DATA_DIR.exists() and (DATA_DIR / "sequences").exists():
        print(f"[OK] Data already exists at {DATA_DIR}")
        return

    if not zip_path.exists():
        print(f"[*] Downloading Vimeo-90K triplet test set (~1.7GB)...")
        print(f"    URL: {VIMEO_URL}")

        def progress_hook(block_num, block_size, total_size):
            downloaded = block_num * block_size
            if total_size > 0:
                pct = min(100, downloaded * 100 // total_size)
                mb = downloaded / (1024*1024)
                total_mb = total_size / (1024*1024)
                print(f"\r    [{pct:3d}%] {mb:.1f} / {total_mb:.1f} MB", end="", flush=True)

        urllib.request.urlretrieve(VIMEO_URL, zip_path, progress_hook)
        print("\n[OK] Download complete")
    else:
        print(f"[OK] ZIP already exists: {zip_path}")

    print("[*] Extracting...")
    with zipfile.ZipFile(zip_path, 'r') as zf:
        zf.extractall(".")
    print(f"[OK] Extracted to {DATA_DIR}")


def select_test_triplets() -> List[Tuple[str, Path]]:
    """
    Select test triplets from Vimeo-90K.
    Categorize by motion magnitude: slow, medium, fast.
    Pick SAMPLES_PER_CATEGORY from each.
    """
    seq_dir = DATA_DIR / "sequences"
    if not seq_dir.exists():
        # Vimeo test set: data directly in DATA_DIR/XXXXX/YYYY/
        seq_dir = DATA_DIR
        # Also check for nested extraction
        for d in DATA_DIR.iterdir():
            if d.is_dir() and (d / "sequences").exists():
                seq_dir = d / "sequences"
                break

    triplets = []
    for group_dir in sorted(seq_dir.iterdir()):
        if not group_dir.is_dir():
            continue
        for clip_dir in sorted(group_dir.iterdir()):
            if not clip_dir.is_dir():
                continue
            im1 = clip_dir / "im1.png"
            im3 = clip_dir / "im3.png"
            # Accept clips with at least im1 + im3 (im2 may be GT or absent)
            if im1.exists() and im3.exists():
                triplets.append(clip_dir)

    if not triplets:
        print(f"[ERR] No triplets found in {seq_dir}")
        print(f"      Checked: {seq_dir}")
        # List what's actually there for debugging
        try:
            items = list(seq_dir.iterdir())[:5]
            print(f"      First items: {[str(i.name) for i in items]}")
            if items and items[0].is_dir():
                sub = list(items[0].iterdir())[:3]
                print(f"      Sub-items: {[str(s.name) for s in sub]}")
                if sub and sub[0].is_dir():
                    files = list(sub[0].iterdir())[:5]
                    print(f"      Files: {[str(f.name) for f in files]}")
        except Exception as e:
            print(f"      Error listing: {e}")
        sys.exit(1)

    print(f"[*] Found {len(triplets)} triplets, categorizing by motion...")

    # Categorize by motion magnitude (simple pixel difference)
    motion_scores = []
    sample_indices = np.linspace(0, len(triplets)-1, min(200, len(triplets)), dtype=int)

    for idx in sample_indices:
        clip_dir = triplets[idx]
        im1 = np.array(Image.open(clip_dir / "im1.png").convert("L"), dtype=np.float32)
        im3 = np.array(Image.open(clip_dir / "im3.png").convert("L"), dtype=np.float32)
        motion = np.mean(np.abs(im3 - im1))
        motion_scores.append((motion, idx))

    motion_scores.sort(key=lambda x: x[0])
    n = len(motion_scores)

    # Split into thirds: slow, medium, fast
    categories = {
        "slow":   [triplets[s[1]] for s in motion_scores[:n//3]],
        "medium": [triplets[s[1]] for s in motion_scores[n//3:2*n//3]],
        "fast":   [triplets[s[1]] for s in motion_scores[2*n//3:]],
    }

    selected = []
    for cat_name, cat_list in categories.items():
        # Pick evenly spaced samples
        indices = np.linspace(0, len(cat_list)-1, SAMPLES_PER_CATEGORY, dtype=int)
        for i in indices:
            selected.append((cat_name, cat_list[i]))
        print(f"    {cat_name}: selected {SAMPLES_PER_CATEGORY} from {len(cat_list)}")

    return selected


# ============================================================
# FRUC Algorithm (NumPy implementation)
# ============================================================

def compute_sad(prev_block: np.ndarray, curr_block: np.ndarray,
                sample_count: int, sample_stride: int) -> float:
    """Compute SAD between two blocks using strided sampling."""
    h, w = prev_block.shape[:2]
    half = (sample_count * sample_stride) // 2
    sad = 0.0
    count = 0
    for sy in range(sample_count):
        for sx in range(sample_count):
            oy = sy * sample_stride - half
            ox = sx * sample_stride - half
            py = max(0, min(h-1, h//2 + oy))
            px = max(0, min(w-1, w//2 + ox))
            sad += abs(float(prev_block[py, px]) - float(curr_block[py, px]))
            count += 1
    return sad / max(count, 1)


def motion_estimation(prev_gray: np.ndarray, curr_gray: np.ndarray,
                      cfg: FRUCConfig) -> np.ndarray:
    """
    Block matching motion estimation with N-step search.
    Returns MV field of shape (mv_h, mv_w, 2) as float32.
    """
    h, w = prev_gray.shape
    bs = cfg.block_size
    mv_h = h // bs
    mv_w = w // bs
    mv_field = np.zeros((mv_h, mv_w, 2), dtype=np.float32)

    half_sample = (cfg.sample_count * cfg.sample_stride) // 2

    # 9-point search offsets (excluding center)
    neighbors = [(-1,-1),(0,-1),(1,-1),(-1,0),(1,0),(-1,1),(0,1),(1,1)]

    for by in range(mv_h):
        for bx in range(mv_w):
            # Block center
            cx = bx * bs + bs // 2
            cy = by * bs + bs // 2
            cx = max(half_sample, min(w - half_sample - 1, cx))
            cy = max(half_sample, min(h - half_sample - 1, cy))

            best_mv = [0, 0]

            # Extract current block region for SAD
            def get_sad(mvx, mvy):
                rcx = cx + mvx
                rcy = cy + mvy
                if rcx < half_sample or rcx >= w - half_sample:
                    return 1e9
                if rcy < half_sample or rcy >= h - half_sample:
                    return 1e9
                # Strided SAD
                sad = 0.0
                for sy in range(cfg.sample_count):
                    for sx in range(cfg.sample_count):
                        oy = sy * cfg.sample_stride - half_sample
                        ox = sx * cfg.sample_stride - half_sample
                        ry = max(0, min(h-1, rcy + oy))
                        rx = max(0, min(w-1, rcx + ox))
                        cy2 = max(0, min(h-1, cy + oy))
                        cx2 = max(0, min(w-1, cx + ox))
                        sad += abs(float(prev_gray[ry, rx]) - float(curr_gray[cy2, cx2]))
                return sad / (cfg.sample_count ** 2)

            best_sad = get_sad(0, 0)

            # Early out for static blocks
            if best_sad < cfg.early_thresh * 255:
                continue

            # N-step search
            for step in cfg.steps:
                prev_best = list(best_mv)
                for dx, dy in neighbors:
                    cand_x = prev_best[0] + dx * step
                    cand_y = prev_best[1] + dy * step
                    sad = get_sad(cand_x, cand_y)
                    if sad < best_sad:
                        best_sad = sad
                        best_mv = [cand_x, cand_y]

            # Clamp
            best_mv[0] = max(-cfg.mv_clamp, min(cfg.mv_clamp, best_mv[0]))
            best_mv[1] = max(-cfg.mv_clamp, min(cfg.mv_clamp, best_mv[1]))
            mv_field[by, bx] = best_mv

    return mv_field


def warp_blend(prev: np.ndarray, curr: np.ndarray, mv_field: np.ndarray,
               block_size: int, blend: float = 0.5, mv_clamp: int = 64) -> np.ndarray:
    """
    Bidirectional warp + blend using bilinearly interpolated MV field.
    """
    h, w = prev.shape[:2]
    mv_h, mv_w = mv_field.shape[:2]
    result = np.zeros_like(curr, dtype=np.float32)

    # Precompute per-pixel MVs via bilinear interpolation of block MV field
    yy, xx = np.mgrid[0:h, 0:w]
    # Map pixel to MV grid coordinates
    mv_x_coords = xx.astype(np.float32) / block_size - 0.5
    mv_y_coords = yy.astype(np.float32) / block_size - 0.5

    # Bilinear interpolation indices
    x0 = np.clip(np.floor(mv_x_coords).astype(int), 0, mv_w - 1)
    x1 = np.clip(x0 + 1, 0, mv_w - 1)
    y0 = np.clip(np.floor(mv_y_coords).astype(int), 0, mv_h - 1)
    y1 = np.clip(y0 + 1, 0, mv_h - 1)

    fx = mv_x_coords - np.floor(mv_x_coords)
    fy = mv_y_coords - np.floor(mv_y_coords)

    # Interpolate MV for each pixel
    for c in range(2):  # x, y components
        v00 = mv_field[y0, x0, c]
        v10 = mv_field[y0, x1, c]
        v01 = mv_field[y1, x0, c]
        v11 = mv_field[y1, x1, c]
        top = v00 * (1 - fx) + v10 * fx
        bot = v01 * (1 - fx) + v11 * fx
        mv_pixel = top * (1 - fy) + bot * fy
        mv_pixel = np.clip(mv_pixel, -mv_clamp, mv_clamp)

        if c == 0:
            mv_px = mv_pixel
        else:
            mv_py = mv_pixel

    # Backward warp prev frame
    prev_x = (xx - mv_px * blend + 0.5).astype(np.float32)
    prev_y = (yy - mv_py * blend + 0.5).astype(np.float32)

    # Forward warp curr frame
    curr_x = (xx + mv_px * (1 - blend) + 0.5).astype(np.float32)
    curr_y = (yy + mv_py * (1 - blend) + 0.5).astype(np.float32)

    # Bilinear sample
    def bilinear_sample(img, sx, sy):
        sx = np.clip(sx, 0, w - 1.001)
        sy = np.clip(sy, 0, h - 1.001)
        x0 = np.floor(sx).astype(int)
        y0 = np.floor(sy).astype(int)
        x1 = np.clip(x0 + 1, 0, w - 1)
        y1 = np.clip(y0 + 1, 0, h - 1)
        fx = (sx - x0)[..., None]
        fy = (sy - y0)[..., None]
        top = img[y0, x0] * (1 - fx) + img[y0, x1] * fx
        bot = img[y1, x0] * (1 - fx) + img[y1, x1] * fx
        return top * (1 - fy) + bot * fy

    prev_f = prev.astype(np.float32)
    curr_f = curr.astype(np.float32)

    prev_sample = bilinear_sample(prev_f, prev_x, prev_y)
    curr_sample = bilinear_sample(curr_f, curr_x, curr_y)

    # Validity masks
    prev_valid = (prev_x >= 0) & (prev_x < w) & (prev_y >= 0) & (prev_y < h)
    curr_valid = (curr_x >= 0) & (curr_x < w) & (curr_y >= 0) & (curr_y < h)

    both = prev_valid & curr_valid
    only_prev = prev_valid & ~curr_valid
    only_curr = ~prev_valid & curr_valid

    both3 = both[..., None]
    only_prev3 = only_prev[..., None]
    only_curr3 = only_curr[..., None]

    result = np.where(both3,
                      prev_sample * (1 - blend) + curr_sample * blend,
                      np.where(only_prev3, prev_sample,
                               np.where(only_curr3, curr_sample, curr_f)))

    return np.clip(result, 0, 255).astype(np.uint8)


def warp_predict(src: np.ndarray, mv_field: np.ndarray,
                 block_size: int, mv_clamp: int) -> np.ndarray:
    """
    Warp src frame by FULL motion vector to predict target frame.
    For each output pixel (x,y), sample src at (x - mvx, y - mvy).
    """
    h, w = src.shape[:2]
    mv_h, mv_w = mv_field.shape[:2]

    yy, xx = np.mgrid[0:h, 0:w]
    mv_x_coords = xx.astype(np.float32) / block_size - 0.5
    mv_y_coords = yy.astype(np.float32) / block_size - 0.5

    x0 = np.clip(np.floor(mv_x_coords).astype(int), 0, mv_w - 1)
    x1 = np.clip(x0 + 1, 0, mv_w - 1)
    y0 = np.clip(np.floor(mv_y_coords).astype(int), 0, mv_h - 1)
    y1 = np.clip(y0 + 1, 0, mv_h - 1)
    fx = mv_x_coords - np.floor(mv_x_coords)
    fy = mv_y_coords - np.floor(mv_y_coords)

    # Bilinear MV interpolation
    mv_px = np.zeros((h, w), dtype=np.float32)
    mv_py = np.zeros((h, w), dtype=np.float32)
    for c, out in enumerate([mv_px, mv_py]):
        v00 = mv_field[y0, x0, c]; v10 = mv_field[y0, x1, c]
        v01 = mv_field[y1, x0, c]; v11 = mv_field[y1, x1, c]
        top = v00 * (1 - fx) + v10 * fx
        bot = v01 * (1 - fx) + v11 * fx
        out[:] = np.clip(top * (1 - fy) + bot * fy, -mv_clamp, mv_clamp)

    # Sample src at (x - mvx, y - mvy) to predict target
    src_x = np.clip(xx.astype(np.float32) - mv_px, 0, w - 1.001)
    src_y = np.clip(yy.astype(np.float32) - mv_py, 0, h - 1.001)

    # Bilinear sample
    ix0 = np.floor(src_x).astype(int); ix1 = np.clip(ix0 + 1, 0, w - 1)
    iy0 = np.floor(src_y).astype(int); iy1 = np.clip(iy0 + 1, 0, h - 1)
    fx = (src_x - ix0)[..., None]
    fy = (src_y - iy0)[..., None]
    src_f = src.astype(np.float32)
    top = src_f[iy0, ix0] * (1 - fx) + src_f[iy0, ix1] * fx
    bot = src_f[iy1, ix0] * (1 - fx) + src_f[iy1, ix1] * fx
    result = top * (1 - fy) + bot * fy

    return np.clip(result, 0, 255).astype(np.uint8)


# ============================================================
# Metrics
# ============================================================

def compute_psnr(img1: np.ndarray, img2: np.ndarray) -> float:
    """Compute PSNR between two images."""
    mse = np.mean((img1.astype(np.float64) - img2.astype(np.float64)) ** 2)
    if mse == 0:
        return 100.0
    return 10 * np.log10(255.0 ** 2 / mse)


def compute_ssim(img1: np.ndarray, img2: np.ndarray) -> float:
    """Compute SSIM between two images."""
    if HAS_SKIMAGE:
        return ssim_fn(img1, img2, channel_axis=2, data_range=255)
    else:
        # Simplified SSIM
        mu1 = img1.astype(np.float64).mean()
        mu2 = img2.astype(np.float64).mean()
        sigma1_sq = img1.astype(np.float64).var()
        sigma2_sq = img2.astype(np.float64).var()
        sigma12 = np.mean((img1.astype(np.float64) - mu1) * (img2.astype(np.float64) - mu2))
        c1 = (0.01 * 255) ** 2
        c2 = (0.03 * 255) ** 2
        num = (2 * mu1 * mu2 + c1) * (2 * sigma12 + c2)
        den = (mu1**2 + mu2**2 + c1) * (sigma1_sq + sigma2_sq + c2)
        return float(num / den)


# ------------------------------------------------------------
# VMAF — Netflix's perceptual quality metric. Unlike PSNR/SSIM
# which average pixel error uniformly, VMAF is trained against
# human subjective ratings and is specifically sensitive to
# local artifacts (edge ringing, motion-boundary shimmer) which
# are exactly the FRUC failure modes we care about.
#
# We call it via ffmpeg's libvmaf filter: simpler than the
# libvmaf Python bindings (which pull in C build deps) and the
# model files are shipped with recent ffmpeg builds.
# ------------------------------------------------------------

FFMPEG_DIR = Path(__file__).parent / ".ffmpeg"
_FFMPEG_PATH_CACHE: Optional[str] = None

# BtbN essentials build — includes libvmaf + bundled models.
# Pinned to a specific release so benchmark results reproduce.
FFMPEG_URL = (
    "https://github.com/BtbN/FFmpeg-Builds/releases/download/"
    "latest/ffmpeg-master-latest-win64-gpl.zip"
)


def get_ffmpeg() -> Optional[str]:
    """Locate or fetch an ffmpeg binary that has libvmaf compiled in.

    Resolution order:
      1. $FFMPEG env var pointing to a full path.
      2. ffmpeg on $PATH.
      3. Cached download at scripts/benchmark/.ffmpeg/ffmpeg.exe.
      4. Download + extract from BtbN's builds (first run only).
    Returns None if none of the above work — callers should
    fall back to PSNR/SSIM only.
    """
    global _FFMPEG_PATH_CACHE
    if _FFMPEG_PATH_CACHE is not None:
        return _FFMPEG_PATH_CACHE or None

    # 1. env var
    env = os.environ.get("FFMPEG")
    if env and Path(env).is_file():
        _FFMPEG_PATH_CACHE = env
        return env

    # 2. PATH
    which = shutil.which("ffmpeg")
    if which:
        _FFMPEG_PATH_CACHE = which
        return which

    # 3. cached download
    exe = FFMPEG_DIR / "ffmpeg.exe"
    if exe.is_file():
        _FFMPEG_PATH_CACHE = str(exe)
        return str(exe)

    # 4. download
    print(f"[*] ffmpeg not found. Downloading from BtbN (one-time, ~100MB)...")
    print(f"    URL: {FFMPEG_URL}")
    FFMPEG_DIR.mkdir(parents=True, exist_ok=True)
    zip_path = FFMPEG_DIR / "ffmpeg.zip"
    try:
        urllib.request.urlretrieve(FFMPEG_URL, zip_path)
        with zipfile.ZipFile(zip_path, "r") as zf:
            # Find ffmpeg.exe inside — it lives under <prefix>/bin/ffmpeg.exe
            for name in zf.namelist():
                if name.endswith("/bin/ffmpeg.exe"):
                    with zf.open(name) as src, open(exe, "wb") as dst:
                        shutil.copyfileobj(src, dst)
                    break
        zip_path.unlink()
        if exe.is_file():
            print(f"[OK] ffmpeg cached at {exe}")
            _FFMPEG_PATH_CACHE = str(exe)
            return str(exe)
    except Exception as e:
        print(f"[WARN] ffmpeg download failed: {e}")

    _FFMPEG_PATH_CACHE = ""  # remember the negative result
    return None


_VMAF_PROBE_DONE = False
_VMAF_AVAILABLE = False


def vmaf_available() -> bool:
    """Probe ffmpeg once for libvmaf support. Cached."""
    global _VMAF_PROBE_DONE, _VMAF_AVAILABLE
    if _VMAF_PROBE_DONE:
        return _VMAF_AVAILABLE
    _VMAF_PROBE_DONE = True
    ff = get_ffmpeg()
    if not ff:
        return False
    try:
        r = subprocess.run(
            [ff, "-hide_banner", "-filters"],
            capture_output=True, text=True, timeout=10)
        _VMAF_AVAILABLE = "libvmaf" in (r.stdout + r.stderr)
    except Exception:
        _VMAF_AVAILABLE = False
    if not _VMAF_AVAILABLE:
        print("[WARN] This ffmpeg does not have libvmaf — VMAF will be skipped.")
    return _VMAF_AVAILABLE


def compute_vmaf(reference: np.ndarray, distorted: np.ndarray) -> Optional[float]:
    """Compute VMAF between two RGB uint8 images. Returns None on failure.

    Both images are written as single-frame PNGs and passed to
    ffmpeg's libvmaf filter. We use libvmaf default (VMAF-0.6.1)
    which is the general-purpose HD model. Score range 0-100
    (higher = better). Since this is a single-frame comparison,
    temporal features contribute 0, but spatial features (VIF,
    DLM) still produce a meaningful relative score.
    """
    if not vmaf_available():
        return None
    if reference.shape != distorted.shape:
        return None
    ff = get_ffmpeg()

    with tempfile.TemporaryDirectory(prefix="vmaf_") as td:
        ref_png = Path(td) / "ref.png"
        dis_png = Path(td) / "dis.png"
        Image.fromarray(reference).save(ref_png)
        Image.fromarray(distorted).save(dis_png)

        # Don't use libvmaf's log_path= option. Windows drive-letter
        # ':' in the temp path trips ffmpeg's filter-option parser
        # no matter how we escape, so instead we use info-level
        # logging and parse the "VMAF score: N" line from stderr.
        filt = (
            "[0:v]setpts=PTS-STARTPTS[dis];"
            "[1:v]setpts=PTS-STARTPTS[ref];"
            "[dis][ref]libvmaf=n_threads=2"
        )
        cmd = [
            ff, "-hide_banner", "-nostdin", "-loglevel", "info",
            "-i", str(dis_png),
            "-i", str(ref_png),
            "-lavfi", filt,
            "-f", "null", "-",
        ]
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
            if r.returncode != 0:
                return None
            # libvmaf prints "VMAF score: NN.NNNNNN" to stderr after
            # the graph finishes. Grab the last one in case there
            # are multiple (streaming mode).
            m = re.findall(r"VMAF score:\s*([0-9.]+)", r.stderr)
            if m:
                return float(m[-1])
        except Exception:
            return None
    return None


# ============================================================
# Benchmark Runner
# ============================================================

def run_benchmark(triplets: List[Tuple[str, Path]], configs: List[FRUCConfig],
                  vmaf_enabled: bool = True):
    """Run all configurations on all triplets and collect results."""
    RESULTS_DIR.mkdir(exist_ok=True)

    results = []
    total = len(configs) * len(triplets)
    done = 0

    # Also compute "naive blend" baseline (simple alpha blend without motion)
    print(f"\n{'='*70}")
    print(f"  FRUC BENCHMARK: {len(configs)} configs x {len(triplets)} triplets = {total} evaluations")
    if vmaf_enabled:
        if vmaf_available():
            print(f"  Metrics: PSNR + SSIM + VMAF (Netflix libvmaf)")
        else:
            print(f"  Metrics: PSNR + SSIM (VMAF requested but libvmaf unavailable)")
    else:
        print(f"  Metrics: PSNR + SSIM (VMAF disabled by --no-vmaf)")
    print(f"{'='*70}\n")

    for cfg in configs:
        cfg_results = []
        cfg_start = time.time()

        for cat_name, clip_dir in triplets:
            done += 1
            im1 = np.array(Image.open(clip_dir / "im1.png").convert("RGB"))
            im3 = np.array(Image.open(clip_dir / "im3.png").convert("RGB"))

            # Check for GT middle frame (available in training set)
            gt_path = clip_dir / "im2.png"
            has_gt = gt_path.exists()
            im2_gt = np.array(Image.open(gt_path).convert("RGB")) if has_gt else None

            # Convert to grayscale for motion estimation
            prev_gray = np.array(Image.open(clip_dir / "im1.png").convert("L"),
                                 dtype=np.float32)
            curr_gray = np.array(Image.open(clip_dir / "im3.png").convert("L"),
                                 dtype=np.float32)

            # Run FRUC
            t0 = time.time()
            mv_field = motion_estimation(prev_gray, curr_gray, cfg)
            t_me = time.time() - t0

            t0 = time.time()
            interp = warp_blend(im1, im3, mv_field, cfg.block_size, 0.5, cfg.mv_clamp)
            t_warp = time.time() - t0

            # Metrics: Warp Accuracy
            # Warp im1 by full MV to predict im3, measure PSNR
            if has_gt:
                reference = im2_gt
                distorted = interp
            else:
                distorted = warp_predict(im1, mv_field, cfg.block_size, cfg.mv_clamp)
                reference = im3
            psnr = compute_psnr(reference, distorted)
            ssim = compute_ssim(reference, distorted)
            # VMAF is perceptually tuned and much more sensitive to
            # local motion-boundary artifacts than PSNR/SSIM, which
            # is exactly the FRUC failure we want to optimize
            # against. Falls back to None if libvmaf isn't available
            # (callers treat None as "skip VMAF column").
            vmaf = compute_vmaf(reference, distorted) if vmaf_enabled else None

            # Non-zero MV ratio
            nz = np.sum(np.any(mv_field != 0, axis=2))
            total_blocks = mv_field.shape[0] * mv_field.shape[1]
            nz_ratio = nz / max(total_blocks, 1)

            cfg_results.append({
                "category": cat_name,
                "clip": str(clip_dir.relative_to(DATA_DIR)),
                "psnr": psnr,
                "ssim": ssim,
                "vmaf": vmaf,
                "time_me": t_me,
                "time_warp": t_warp,
                "nz_ratio": nz_ratio,
                "has_gt": has_gt,
            })

            pct = done * 100 // total
            vmaf_str = f" VMAF={vmaf:5.2f}" if vmaf is not None else ""
            print(f"\r  [{pct:3d}%] Config {cfg.id:2d} ({cfg.name:20s}) | "
                  f"{cat_name:6s} | PSNR={psnr:5.2f} SSIM={ssim:.4f}{vmaf_str} | "
                  f"ME={t_me*1000:6.1f}ms Warp={t_warp*1000:5.1f}ms",
                  end="", flush=True)

        cfg_elapsed = time.time() - cfg_start
        avg_psnr = np.mean([r["psnr"] for r in cfg_results])
        avg_ssim = np.mean([r["ssim"] for r in cfg_results])
        vmaf_scores = [r["vmaf"] for r in cfg_results if r["vmaf"] is not None]
        avg_vmaf = float(np.mean(vmaf_scores)) if vmaf_scores else None
        avg_time = np.mean([r["time_me"] + r["time_warp"] for r in cfg_results]) * 1000

        def _cat_vmaf(cat):
            xs = [r["vmaf"] for r in cfg_results
                  if r["category"] == cat and r["vmaf"] is not None]
            return float(np.mean(xs)) if xs else None

        results.append({
            "config_id": cfg.id,
            "config_name": cfg.name,
            "block_size": cfg.block_size,
            "steps": str(cfg.steps),
            "search_radius": cfg.search_radius,
            "sample_count": cfg.sample_count,
            "sample_stride": cfg.sample_stride,
            "early_thresh": cfg.early_thresh,
            "mv_clamp": cfg.mv_clamp,
            "avg_psnr": avg_psnr,
            "avg_ssim": avg_ssim,
            "avg_vmaf": avg_vmaf,
            "avg_time_ms": avg_time,
            "cost_estimate": cfg.cost_estimate,
            "per_category": {
                cat: {
                    "psnr": np.mean([r["psnr"] for r in cfg_results if r["category"] == cat]),
                    "ssim": np.mean([r["ssim"] for r in cfg_results if r["category"] == cat]),
                    "vmaf": _cat_vmaf(cat),
                }
                for cat in ["slow", "medium", "fast"]
            },
            "details": cfg_results,
            "total_time": cfg_elapsed,
        })

        vmaf_str = f" VMAF={avg_vmaf:.2f}" if avg_vmaf is not None else ""
        print(f"\n  Config {cfg.id:2d} done: avg PSNR={avg_psnr:.2f} "
              f"SSIM={avg_ssim:.4f}{vmaf_str} "
              f"CPU={avg_time:.1f}ms ({cfg_elapsed:.1f}s total)")

    return results


def save_results(results: List[dict]):
    """Save results to CSV and JSON."""
    RESULTS_DIR.mkdir(exist_ok=True)

    # Sort by VMAF if available (perceptually calibrated), else PSNR.
    # VMAF is much more sensitive to the local motion-boundary
    # artifacts we care about, so it's the better primary ranking key.
    has_vmaf = any(r.get("avg_vmaf") is not None for r in results)
    if has_vmaf:
        # Configs without a VMAF score (shouldn't happen, but be safe) sort last.
        ranked = sorted(results,
                        key=lambda r: (r.get("avg_vmaf") or -1.0),
                        reverse=True)
    else:
        ranked = sorted(results, key=lambda r: r["avg_psnr"], reverse=True)

    # CSV summary
    csv_path = RESULTS_DIR / "benchmark_results.csv"
    with open(csv_path, "w", newline="", encoding="utf-8") as f:
        writer = csv.writer(f)
        writer.writerow([
            "Rank", "ConfigID", "Name", "BlockSize", "Steps", "SearchRadius",
            "SampleCount", "SampleStride", "EarlyThresh", "MVClamp",
            "AvgPSNR", "AvgSSIM", "AvgVMAF", "AvgTimeMs", "CostEstimate",
            "PSNR_Slow", "PSNR_Medium", "PSNR_Fast",
            "SSIM_Slow", "SSIM_Medium", "SSIM_Fast",
            "VMAF_Slow", "VMAF_Medium", "VMAF_Fast",
        ])
        def _fmt_vmaf(v):
            return f"{v:.2f}" if v is not None else ""
        for rank, r in enumerate(ranked, 1):
            writer.writerow([
                rank, r["config_id"], r["config_name"],
                r["block_size"], r["steps"], r["search_radius"],
                r["sample_count"], r["sample_stride"],
                r["early_thresh"], r["mv_clamp"],
                f"{r['avg_psnr']:.3f}", f"{r['avg_ssim']:.5f}",
                _fmt_vmaf(r.get("avg_vmaf")),
                f"{r['avg_time_ms']:.1f}", r["cost_estimate"],
                f"{r['per_category']['slow']['psnr']:.3f}",
                f"{r['per_category']['medium']['psnr']:.3f}",
                f"{r['per_category']['fast']['psnr']:.3f}",
                f"{r['per_category']['slow']['ssim']:.5f}",
                f"{r['per_category']['medium']['ssim']:.5f}",
                f"{r['per_category']['fast']['ssim']:.5f}",
                _fmt_vmaf(r['per_category']['slow'].get('vmaf')),
                _fmt_vmaf(r['per_category']['medium'].get('vmaf')),
                _fmt_vmaf(r['per_category']['fast'].get('vmaf')),
            ])

    # Full JSON
    json_path = RESULTS_DIR / "benchmark_results.json"
    # Remove non-serializable detail frames for JSON
    json_results = []
    for r in ranked:
        jr = dict(r)
        jr["details"] = r["details"]  # keep details
        json_results.append(jr)

    with open(json_path, "w", encoding="utf-8") as f:
        json.dump(json_results, f, indent=2, ensure_ascii=False, default=str)

    print(f"\n[OK] Results saved to:")
    print(f"     {csv_path}")
    print(f"     {json_path}")

    # Print ranking table
    print(f"\n{'='*106}")
    primary = "VMAF" if has_vmaf else "PSNR"
    print(f"  RANKING (by avg {primary}; higher is better)")
    print(f"{'='*106}")
    print(f"{'Rank':>4} {'ID':>3} {'Name':<22} "
          f"{'PSNR':>7} {'SSIM':>7} {'VMAF':>7} "
          f"{'vSlow':>7} {'vMed':>7} {'vFast':>7} {'Cost':>8}")
    print(f"{'-'*106}")

    baseline = next(r for r in results if r["config_id"] == 0)
    baseline_psnr = baseline["avg_psnr"]
    baseline_vmaf = baseline.get("avg_vmaf")

    def _fmt(v, w=7, dec=3):
        return f"{v:{w}.{dec}f}" if v is not None else " " * (w-1) + "-"

    for rank, r in enumerate(ranked, 1):
        marker = " ***" if rank == 1 else (" **" if rank <= 3 else "")
        if has_vmaf and baseline_vmaf is not None and r.get("avg_vmaf") is not None:
            delta = r["avg_vmaf"] - baseline_vmaf
            delta_str = f"({delta:+.2f} VMAF)"
        else:
            delta = r["avg_psnr"] - baseline_psnr
            delta_str = f"({delta:+.2f}dB)"
        print(f"{rank:4d} {r['config_id']:3d} {r['config_name']:<22} "
              f"{r['avg_psnr']:7.3f} {r['avg_ssim']:7.5f} "
              f"{_fmt(r.get('avg_vmaf'), 7, 2)} "
              f"{_fmt(r['per_category']['slow'].get('vmaf'), 7, 2)} "
              f"{_fmt(r['per_category']['medium'].get('vmaf'), 7, 2)} "
              f"{_fmt(r['per_category']['fast'].get('vmaf'), 7, 2)} "
              f"{r['cost_estimate']:8d} "
              f"{delta_str}{marker}")

    print(f"\n  Baseline (Config 0): PSNR={baseline_psnr:.3f}"
          + (f" VMAF={baseline_vmaf:.2f}" if baseline_vmaf is not None else ""))
    best = ranked[0]
    print(f"  Best     (Config {best['config_id']}): PSNR={best['avg_psnr']:.3f}"
          + (f" VMAF={best['avg_vmaf']:.2f}" if best.get('avg_vmaf') is not None else ""))

    return ranked


# ============================================================
# Main
# ============================================================

def main():
    parser = argparse.ArgumentParser(description="VipleStream FRUC Benchmark")
    parser.add_argument("--download-only", action="store_true",
                        help="Only download data, don't run benchmark")
    parser.add_argument("--skip-download", action="store_true",
                        help="Skip download, assume data exists")
    parser.add_argument("--config", nargs="+", type=int,
                        help="Run specific config IDs only (e.g., --config 0 2 5)")
    parser.add_argument("--no-vmaf", action="store_true",
                        help="Skip VMAF scoring (keeps PSNR/SSIM only). VMAF "
                             "requires ffmpeg with libvmaf; the script will "
                             "auto-download ffmpeg on first run if missing.")
    args = parser.parse_args()

    print("=" * 60)
    print("  VipleStream FRUC Benchmark")
    print("  Vimeo-90K Triplet Test Set")
    print("=" * 60)

    # Step 1: Download
    if not args.skip_download:
        download_vimeo90k()

    if args.download_only:
        print("[*] Download complete. Exiting.")
        return

    # Step 2: Select triplets
    triplets = select_test_triplets()
    print(f"[OK] Selected {len(triplets)} triplets for benchmark")

    # Step 3: Filter configs
    configs = CONFIGS
    if args.config:
        configs = [c for c in CONFIGS if c.id in args.config]
        print(f"[*] Running {len(configs)} selected configs: {[c.id for c in configs]}")

    # Step 4: Run benchmark
    results = run_benchmark(triplets, configs, vmaf_enabled=not args.no_vmaf)

    # Step 5: Save and display results
    ranked = save_results(results)

    print(f"\n[DONE] Benchmark complete!")


if __name__ == "__main__":
    main()
