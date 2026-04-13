#!/usr/bin/env python3
"""
VipleStream FRUC 20-Iteration Algorithm Benchmark
==================================================
Implements 20 algorithmic variants of GenericFRUC and evaluates them on Vimeo-90K.
Each variant is a progressive improvement targeting quality, stability, and low latency.

Metrics: Warp Accuracy PSNR (higher=better MV estimation), execution time.
"""

import os, sys, time, csv, json, argparse
from pathlib import Path
from dataclasses import dataclass
from typing import List, Tuple
import numpy as np
from PIL import Image

try:
    from skimage.metrics import structural_similarity as ssim_fn
    HAS_SKIMAGE = True
except ImportError:
    HAS_SKIMAGE = False

DATA_DIR = Path("vimeo_tri_test")
RESULTS_DIR = Path("iteration_results")
SAMPLES_PER_CATEGORY = 5


# ============================================================
# Utility Functions
# ============================================================

def to_gray(img):
    return np.dot(img[..., :3].astype(np.float32), [0.299, 0.587, 0.114])

def compute_psnr(a, b):
    mse = np.mean((a.astype(np.float64) - b.astype(np.float64))**2)
    return 10 * np.log10(255**2 / max(mse, 1e-10))

def compute_ssim(a, b):
    if HAS_SKIMAGE:
        return ssim_fn(a, b, channel_axis=2, data_range=255)
    mu1, mu2 = a.mean(), b.mean()
    s1, s2 = a.var(), b.var()
    s12 = np.mean((a.astype(float)-mu1)*(b.astype(float)-mu2))
    c1, c2 = (0.01*255)**2, (0.03*255)**2
    return float(((2*mu1*mu2+c1)*(2*s12+c2))/((mu1**2+mu2**2+c1)*(s1+s2+c2)))

def bilinear_interp_mv(mv_field, h, w, block_size):
    """Interpolate block-level MV field to per-pixel."""
    mv_h, mv_w = mv_field.shape[:2]
    yy, xx = np.mgrid[0:h, 0:w]
    mx = xx.astype(np.float32) / block_size - 0.5
    my = yy.astype(np.float32) / block_size - 0.5
    x0 = np.clip(np.floor(mx).astype(int), 0, mv_w-1)
    x1 = np.clip(x0+1, 0, mv_w-1)
    y0 = np.clip(np.floor(my).astype(int), 0, mv_h-1)
    y1 = np.clip(y0+1, 0, mv_h-1)
    fx = mx - np.floor(mx)
    fy = my - np.floor(my)
    mvx = np.zeros((h,w), np.float32)
    mvy = np.zeros((h,w), np.float32)
    for c, out in enumerate([mvx, mvy]):
        v00=mv_field[y0,x0,c]; v10=mv_field[y0,x1,c]
        v01=mv_field[y1,x0,c]; v11=mv_field[y1,x1,c]
        out[:] = (v00*(1-fx)+v10*fx)*(1-fy) + (v01*(1-fx)+v11*fx)*fy
    return mvx, mvy

def warp_frame(src, mvx, mvy):
    """Warp src by full MV: sample src at (x-mvx, y-mvy)."""
    h, w = src.shape[:2]
    yy, xx = np.mgrid[0:h, 0:w]
    sx = np.clip(xx.astype(np.float32) - mvx, 0, w-1.001)
    sy = np.clip(yy.astype(np.float32) - mvy, 0, h-1.001)
    ix0 = np.floor(sx).astype(int); ix1 = np.clip(ix0+1, 0, w-1)
    iy0 = np.floor(sy).astype(int); iy1 = np.clip(iy0+1, 0, h-1)
    fx = (sx-ix0)[...,None]; fy = (sy-iy0)[...,None]
    sf = src.astype(np.float32)
    top = sf[iy0,ix0]*(1-fx) + sf[iy0,ix1]*fx
    bot = sf[iy1,ix0]*(1-fx) + sf[iy1,ix1]*fx
    return np.clip(top*(1-fy)+bot*fy, 0, 255).astype(np.uint8)


# ============================================================
# Census Transform
# ============================================================

def census_transform(gray, radius=3):
    """Compute Census Transform: each pixel → bit pattern of neighbor comparisons."""
    h, w = gray.shape
    r = radius
    ct = np.zeros((h, w), dtype=np.int64)
    center = gray[r:h-r, r:w-r]
    bit = 0
    for dy in range(-r, r+1):
        for dx in range(-r, r+1):
            if dy == 0 and dx == 0:
                continue
            neighbor = gray[r+dy:h-r+dy, r+dx:w-r+dx]
            ct[r:h-r, r:w-r] |= ((neighbor < center).astype(np.int64) << bit)
            bit += 1
    return ct

def hamming_distance(a, b):
    """Hamming distance between two census bit patterns."""
    xor = np.bitwise_xor(a.astype(np.int64), b.astype(np.int64))
    # Popcount via lookup
    count = np.zeros_like(xor)
    while np.any(xor > 0):
        count += xor & 1
        xor >>= 1
    return count.astype(np.float32)


# ============================================================
# Bilateral MV Filter
# ============================================================

def bilateral_mv_filter(mv_field, guide_gray, block_size, sigma_s=1.5, sigma_r=15.0):
    """Bilateral filter on MV field using image guidance for edge preservation."""
    mv_h, mv_w = mv_field.shape[:2]
    result = np.copy(mv_field)
    r = 1  # 3x3 kernel
    for by in range(mv_h):
        for bx in range(mv_w):
            cx = min(bx * block_size + block_size//2, guide_gray.shape[1]-1)
            cy = min(by * block_size + block_size//2, guide_gray.shape[0]-1)
            center_val = guide_gray[cy, cx]
            w_sum = 0.0
            mv_acc = np.zeros(2, np.float64)
            for dy in range(-r, r+1):
                for dx in range(-r, r+1):
                    ny, nx = by+dy, bx+dx
                    if ny < 0 or ny >= mv_h or nx < 0 or nx >= mv_w:
                        continue
                    ncx = min(nx * block_size + block_size//2, guide_gray.shape[1]-1)
                    ncy = min(ny * block_size + block_size//2, guide_gray.shape[0]-1)
                    n_val = guide_gray[ncy, ncx]
                    ws = np.exp(-(dx**2 + dy**2) / (2 * sigma_s**2))
                    wr = np.exp(-((float(center_val) - float(n_val))**2) / (2 * sigma_r**2))
                    w = ws * wr
                    w_sum += w
                    mv_acc += w * mv_field[ny, nx]
            if w_sum > 0:
                result[by, bx] = mv_acc / w_sum
    return result


# ============================================================
# Base FRUC class and 20 variants
# ============================================================

class BaseFRUC:
    """Base class for FRUC variants."""
    name = "BaseFRUC"
    version = "v00"

    def __init__(self):
        self.block_size = 64
        self.steps = [4, 2, 1]
        self.sample_count = 4
        self.sample_stride = 4
        self.early_thresh = 0.01
        self.mv_clamp = 48
        self.use_census = False
        self.use_bilateral_mv = False
        self.use_fwd_bwd_check = False
        self.pyramid_levels = 1
        self.use_subpixel = False
        self.use_diamond = False
        self.use_temporal_mv = False
        self.prev_mv_field = None

    @property
    def cost_estimate(self):
        h, w = 1080, 1920
        n_blocks = (w // self.block_size) * (h // self.block_size)
        sads = len(self.steps) * 8
        samples = self.sample_count ** 2
        pyramid_mult = sum(1/4**i for i in range(self.pyramid_levels))
        bilateral_cost = n_blocks * 9 if self.use_bilateral_mv else 0
        return int(n_blocks * sads * samples * pyramid_mult + bilateral_cost)

    def _compute_match_cost(self, prev_gray, curr_gray, cx, cy, mvx, mvy, ct_prev=None, ct_curr=None):
        """Compute match cost at a candidate MV position."""
        h, w = prev_gray.shape
        hs = (self.sample_count * self.sample_stride) // 2
        rcx, rcy = cx + mvx, cy + mvy
        if rcx < hs or rcx >= w-hs or rcy < hs or rcy >= h-hs:
            return 1e9

        if self.use_census and ct_prev is not None:
            # Census: Hamming distance
            cost = 0.0
            for sy in range(self.sample_count):
                for sx in range(self.sample_count):
                    oy = sy * self.sample_stride - hs
                    ox = sx * self.sample_stride - hs
                    ry = max(0, min(h-1, rcy+oy))
                    rx = max(0, min(w-1, rcx+ox))
                    cy2 = max(0, min(h-1, cy+oy))
                    cx2 = max(0, min(w-1, cx+ox))
                    xor_val = int(ct_prev[ry, rx]) ^ int(ct_curr[cy2, cx2])
                    cost += bin(xor_val).count('1')
            return cost / (self.sample_count ** 2)
        else:
            # SAD
            cost = 0.0
            for sy in range(self.sample_count):
                for sx in range(self.sample_count):
                    oy = sy * self.sample_stride - hs
                    ox = sx * self.sample_stride - hs
                    ry = max(0, min(h-1, rcy+oy))
                    rx = max(0, min(w-1, rcx+ox))
                    cy2 = max(0, min(h-1, cy+oy))
                    cx2 = max(0, min(w-1, cx+ox))
                    cost += abs(float(prev_gray[ry,rx]) - float(curr_gray[cy2,cx2]))
            return cost / (self.sample_count ** 2)

    def estimate_motion(self, prev_gray, curr_gray):
        """Motion estimation with configured search strategy."""
        h, w = prev_gray.shape
        bs = self.block_size
        mv_h, mv_w = h // bs, w // bs
        mv_field = np.zeros((mv_h, mv_w, 2), np.float32)

        ct_prev = census_transform(prev_gray) if self.use_census else None
        ct_curr = census_transform(curr_gray) if self.use_census else None

        hs = (self.sample_count * self.sample_stride) // 2

        if self.use_diamond:
            neighbors = [(0,-1),(0,1),(-1,0),(1,0)]  # Diamond: 4-point
        else:
            neighbors = [(-1,-1),(0,-1),(1,-1),(-1,0),(1,0),(-1,1),(0,1),(1,1)]

        for by in range(mv_h):
            for bx in range(mv_w):
                cx = min(bx*bs+bs//2, w-hs-1)
                cy = min(by*bs+bs//2, h-hs-1)
                cx = max(hs, cx); cy = max(hs, cy)

                # Initial predictor from temporal MV
                if self.use_temporal_mv and self.prev_mv_field is not None:
                    init_mv = self.prev_mv_field[
                        min(by, self.prev_mv_field.shape[0]-1),
                        min(bx, self.prev_mv_field.shape[1]-1)
                    ].copy()
                else:
                    init_mv = np.array([0.0, 0.0])

                best_mv = init_mv.copy()
                best_cost = self._compute_match_cost(
                    prev_gray, curr_gray, cx, cy, int(best_mv[0]), int(best_mv[1]),
                    ct_prev, ct_curr)

                if best_cost < self.early_thresh * (48 if self.use_census else 255):
                    mv_field[by, bx] = [0, 0]
                    continue

                for step in self.steps:
                    prev_best = best_mv.copy()
                    for dx, dy in neighbors:
                        cand = [int(prev_best[0] + dx*step), int(prev_best[1] + dy*step)]
                        cost = self._compute_match_cost(
                            prev_gray, curr_gray, cx, cy, cand[0], cand[1],
                            ct_prev, ct_curr)
                        if cost < best_cost:
                            best_cost = cost
                            best_mv = np.array(cand, dtype=np.float32)

                # Sub-pixel refinement via parabolic fitting
                if self.use_subpixel:
                    for dim in range(2):
                        offsets = [-1, 0, 1]
                        costs = []
                        for off in offsets:
                            test_mv = best_mv.copy()
                            test_mv[dim] += off
                            costs.append(self._compute_match_cost(
                                prev_gray, curr_gray, cx, cy,
                                int(test_mv[0]), int(test_mv[1]), ct_prev, ct_curr))
                        if costs[0] + costs[2] - 2*costs[1] > 1e-6:
                            delta = (costs[0] - costs[2]) / (2 * (costs[0] + costs[2] - 2*costs[1]))
                            best_mv[dim] += np.clip(delta, -0.5, 0.5)

                best_mv = np.clip(best_mv, -self.mv_clamp, self.mv_clamp)
                mv_field[by, bx] = best_mv

        # Bilateral MV filtering
        if self.use_bilateral_mv:
            mv_field = bilateral_mv_filter(mv_field, curr_gray, bs)

        # Forward-backward consistency check
        if self.use_fwd_bwd_check:
            # Compute backward MVs (curr→prev)
            bwd_field = np.zeros_like(mv_field)
            for by in range(mv_h):
                for bx in range(mv_w):
                    cx = min(bx*bs+bs//2, w-hs-1); cy = min(by*bs+bs//2, h-hs-1)
                    cx = max(hs, cx); cy = max(hs, cy)
                    best_mv2 = np.array([0.0, 0.0])
                    best_cost2 = self._compute_match_cost(
                        curr_gray, prev_gray, cx, cy, 0, 0, ct_curr, ct_prev)
                    for step in self.steps[:2]:  # Fewer steps for speed
                        prev_b = best_mv2.copy()
                        for dx, dy in neighbors:
                            cand = [int(prev_b[0]+dx*step), int(prev_b[1]+dy*step)]
                            cost = self._compute_match_cost(
                                curr_gray, prev_gray, cx, cy, cand[0], cand[1],
                                ct_curr, ct_prev)
                            if cost < best_cost2:
                                best_cost2 = cost
                                best_mv2 = np.array(cand, dtype=np.float32)
                    bwd_field[by, bx] = best_mv2

            # Check consistency: fwd + bwd should cancel
            for by in range(mv_h):
                for bx in range(mv_w):
                    fwd = mv_field[by, bx]
                    # Where does fwd point to?
                    tgt_bx = int(round(bx + fwd[0] / bs))
                    tgt_by = int(round(by + fwd[1] / bs))
                    tgt_bx = max(0, min(mv_w-1, tgt_bx))
                    tgt_by = max(0, min(mv_h-1, tgt_by))
                    bwd = bwd_field[tgt_by, tgt_bx]
                    # Consistency: ||fwd + bwd|| should be small
                    err = np.sqrt((fwd[0]+bwd[0])**2 + (fwd[1]+bwd[1])**2)
                    if err > bs * 0.3:  # Inconsistent → likely occlusion, zero out
                        mv_field[by, bx] = [0, 0]

        # Save for temporal predictor
        if self.use_temporal_mv:
            self.prev_mv_field = mv_field.copy()

        return mv_field

    def run(self, prev_rgb, curr_rgb):
        """Full pipeline: estimate MV → warp predict."""
        prev_gray = to_gray(prev_rgb)
        curr_gray = to_gray(curr_rgb)

        if self.pyramid_levels > 1:
            mv_field = self._pyramid_search(prev_gray, curr_gray)
        else:
            mv_field = self.estimate_motion(prev_gray, curr_gray)

        mvx, mvy = bilinear_interp_mv(mv_field, prev_rgb.shape[0], prev_rgb.shape[1], self.block_size)
        predicted = warp_frame(prev_rgb, mvx, mvy)
        return predicted, mv_field

    def _pyramid_search(self, prev_gray, curr_gray):
        """Hierarchical coarse-to-fine motion estimation."""
        from PIL import Image as PILImage

        levels = self.pyramid_levels
        prev_pyr = [prev_gray]
        curr_pyr = [curr_gray]
        for _ in range(1, levels):
            ph, pw = prev_pyr[-1].shape
            prev_down = np.array(PILImage.fromarray(prev_pyr[-1].astype(np.uint8)).resize(
                (pw//2, ph//2), PILImage.BILINEAR), dtype=np.float32)
            curr_down = np.array(PILImage.fromarray(curr_pyr[-1].astype(np.uint8)).resize(
                (pw//2, ph//2), PILImage.BILINEAR), dtype=np.float32)
            prev_pyr.append(prev_down)
            curr_pyr.append(curr_down)

        # Coarsest level: full search
        coarse_prev = prev_pyr[-1]
        coarse_curr = curr_pyr[-1]
        scale = 2 ** (levels - 1)
        orig_bs = self.block_size
        self.block_size = max(8, orig_bs // scale)
        mv_field = self.estimate_motion(coarse_prev, coarse_curr)

        # Refine at each finer level
        for lev in range(levels-2, -1, -1):
            fine_prev = prev_pyr[lev]
            fine_curr = curr_pyr[lev]
            fh, fw = fine_prev.shape
            scale = 2 ** lev
            self.block_size = max(8, orig_bs // scale) if lev > 0 else orig_bs

            # Upsample MV field × 2
            mv_h_new = fh // self.block_size
            mv_w_new = fw // self.block_size
            from scipy.ndimage import zoom as ndzoom
            upsampled = np.zeros((mv_h_new, mv_w_new, 2), np.float32)
            for c in range(2):
                upsampled[:,:,c] = ndzoom(mv_field[:,:,c],
                    (mv_h_new/mv_field.shape[0], mv_w_new/mv_field.shape[1]),
                    order=1) * 2  # Scale MVs by 2

            # Refine with small search around predicted position
            old_steps = self.steps
            self.steps = [2, 1]  # Small refinement steps
            old_temporal = self.use_temporal_mv
            self.use_temporal_mv = True
            self.prev_mv_field = upsampled
            mv_field = self.estimate_motion(fine_prev, fine_curr)
            self.steps = old_steps
            self.use_temporal_mv = old_temporal

        self.block_size = orig_bs
        return mv_field


# --- 20 Concrete Variants ---

class V00_Baseline(BaseFRUC):
    name = "Baseline"; version = "v00"

class V01_BilateralMV(BaseFRUC):
    name = "BilateralMV"; version = "v01"
    def __init__(self): super().__init__(); self.use_bilateral_mv = True

class V02_Census(BaseFRUC):
    name = "Census"; version = "v02"
    def __init__(self): super().__init__(); self.use_census = True

class V03_Census_BilMV(BaseFRUC):
    name = "Census+BilMV"; version = "v03"
    def __init__(self): super().__init__(); self.use_census = True; self.use_bilateral_mv = True

class V04_FwdBwdCheck(BaseFRUC):
    name = "Census+BilMV+FwdBwd"; version = "v04"
    def __init__(self): super().__init__(); self.use_census = True; self.use_bilateral_mv = True; self.use_fwd_bwd_check = True

class V05_Pyramid2L(BaseFRUC):
    name = "Pyramid2L"; version = "v05"
    def __init__(self): super().__init__(); self.pyramid_levels = 2

class V06_Pyramid2L_Census(BaseFRUC):
    name = "Pyr2L+Census"; version = "v06"
    def __init__(self): super().__init__(); self.pyramid_levels = 2; self.use_census = True

class V07_Diamond(BaseFRUC):
    name = "DiamondSearch"; version = "v07"
    def __init__(self): super().__init__(); self.use_diamond = True

class V08_SubPixel(BaseFRUC):
    name = "Pyr2L+Census+SubPx"; version = "v08"
    def __init__(self): super().__init__(); self.pyramid_levels = 2; self.use_census = True; self.use_subpixel = True

class V09_AdaptiveBlend(BaseFRUC):
    name = "Census+AdaptBlend"; version = "v09"
    def __init__(self): super().__init__(); self.use_census = True
    # AdaptiveBlend only affects warp, not ME — same MV quality, so evaluated identically

class V10_Pyramid3L(BaseFRUC):
    name = "Pyramid3L"; version = "v10"
    def __init__(self): super().__init__(); self.pyramid_levels = 3

class V11_Pyr3L_Census_Bil(BaseFRUC):
    name = "Pyr3L+Census+BilMV"; version = "v11"
    def __init__(self): super().__init__(); self.pyramid_levels = 3; self.use_census = True; self.use_bilateral_mv = True

class V12_TemporalMV(BaseFRUC):
    name = "TemporalMV"; version = "v12"
    def __init__(self): super().__init__(); self.use_temporal_mv = True

class V13_Census_Temporal(BaseFRUC):
    name = "Census+TemporalMV"; version = "v13"
    def __init__(self): super().__init__(); self.use_census = True; self.use_temporal_mv = True

class V14_Pyr3L_Census_FwdBwd(BaseFRUC):
    name = "Pyr3L+Census+FwdBwd"; version = "v14"
    def __init__(self): super().__init__(); self.pyramid_levels = 3; self.use_census = True; self.use_fwd_bwd_check = True

class V15_Fast(BaseFRUC):
    name = "Fast-Census+BilMV+Diamond"; version = "v15"
    def __init__(self): super().__init__(); self.use_census = True; self.use_bilateral_mv = True; self.use_diamond = True

class V16_Quality(BaseFRUC):
    name = "Quality-Pyr3L+Census+SubPx+BilMV"; version = "v16"
    def __init__(self): super().__init__(); self.pyramid_levels = 3; self.use_census = True; self.use_subpixel = True; self.use_bilateral_mv = True

class V17_Balanced(BaseFRUC):
    name = "Balanced-Pyr2L+Census+BilMV+FwdBwd"; version = "v17"
    def __init__(self): super().__init__(); self.pyramid_levels = 2; self.use_census = True; self.use_bilateral_mv = True; self.use_fwd_bwd_check = True

class V18_Temporal_Combined(BaseFRUC):
    name = "TempCombined-Pyr2L+Census+BilMV+Temp"; version = "v18"
    def __init__(self): super().__init__(); self.pyramid_levels = 2; self.use_census = True; self.use_bilateral_mv = True; self.use_temporal_mv = True

class V19_FinalBest(BaseFRUC):
    name = "FinalBest-AutoSelect"; version = "v19"
    # This will be configured after evaluating v00-v18


ALL_VARIANTS = [
    V00_Baseline, V01_BilateralMV, V02_Census, V03_Census_BilMV,
    V04_FwdBwdCheck, V05_Pyramid2L, V06_Pyramid2L_Census, V07_Diamond,
    V08_SubPixel, V09_AdaptiveBlend, V10_Pyramid3L, V11_Pyr3L_Census_Bil,
    V12_TemporalMV, V13_Census_Temporal, V14_Pyr3L_Census_FwdBwd,
    V15_Fast, V16_Quality, V17_Balanced, V18_Temporal_Combined, V19_FinalBest
]


# ============================================================
# Test Data Selection
# ============================================================

def select_triplets():
    seq_dir = DATA_DIR
    for d in DATA_DIR.iterdir():
        if d.is_dir() and (d / "sequences").exists():
            seq_dir = d / "sequences"; break

    triplets = []
    for g in sorted(seq_dir.iterdir()):
        if not g.is_dir(): continue
        for c in sorted(g.iterdir()):
            if not c.is_dir(): continue
            if (c/"im1.png").exists() and (c/"im3.png").exists():
                triplets.append(c)

    if not triplets:
        print(f"[ERR] No triplets in {seq_dir}"); sys.exit(1)

    # Categorize by motion
    scores = []
    indices = np.linspace(0, len(triplets)-1, min(200, len(triplets)), dtype=int)
    for idx in indices:
        im1 = np.array(Image.open(triplets[idx]/"im1.png").convert("L"), dtype=np.float32)
        im3 = np.array(Image.open(triplets[idx]/"im3.png").convert("L"), dtype=np.float32)
        scores.append((np.mean(np.abs(im3-im1)), idx))

    scores.sort(key=lambda x: x[0])
    n = len(scores)
    cats = {"slow": scores[:n//3], "medium": scores[n//3:2*n//3], "fast": scores[2*n//3:]}

    selected = []
    for cat, items in cats.items():
        idxs = np.linspace(0, len(items)-1, SAMPLES_PER_CATEGORY, dtype=int)
        for i in idxs:
            selected.append((cat, triplets[items[i][1]]))
        print(f"  {cat}: {SAMPLES_PER_CATEGORY} clips")
    return selected


# ============================================================
# Benchmark Runner
# ============================================================

def run_all(triplets, variant_classes):
    RESULTS_DIR.mkdir(exist_ok=True)
    results = []
    total = len(variant_classes) * len(triplets)
    done = 0

    print(f"\n{'='*80}")
    print(f"  FRUC 20-Iteration Benchmark: {len(variant_classes)} variants x {len(triplets)} clips")
    print(f"{'='*80}\n")

    for VCls in variant_classes:
        v = VCls()
        v_results = []
        t_start = time.time()

        for cat, clip in triplets:
            done += 1
            im1 = np.array(Image.open(clip/"im1.png").convert("RGB"))
            im3 = np.array(Image.open(clip/"im3.png").convert("RGB"))

            t0 = time.time()
            predicted, mv_field = v.run(im1, im3)
            elapsed = time.time() - t0

            psnr = compute_psnr(im3, predicted)
            ssim = compute_ssim(im3, predicted)
            nz = np.sum(np.any(mv_field != 0, axis=2)) / max(1, mv_field.shape[0]*mv_field.shape[1])

            v_results.append({"cat": cat, "psnr": psnr, "ssim": ssim, "time": elapsed, "nz": nz})

            pct = done * 100 // total
            print(f"\r  [{pct:3d}%] {v.version} {v.name[:30]:<30} | {cat:6} | "
                  f"PSNR={psnr:6.2f} SSIM={ssim:.4f} {elapsed*1000:7.1f}ms", end="", flush=True)

        total_time = time.time() - t_start
        avg_psnr = np.mean([r["psnr"] for r in v_results])
        avg_ssim = np.mean([r["ssim"] for r in v_results])
        avg_time = np.mean([r["time"] for r in v_results]) * 1000

        per_cat = {}
        for cat in ["slow", "medium", "fast"]:
            cat_items = [r for r in v_results if r["cat"] == cat]
            if cat_items:
                per_cat[cat] = {"psnr": np.mean([r["psnr"] for r in cat_items]),
                                "ssim": np.mean([r["ssim"] for r in cat_items])}

        results.append({
            "version": v.version, "name": v.name, "avg_psnr": avg_psnr,
            "avg_ssim": avg_ssim, "avg_time_ms": avg_time, "cost": v.cost_estimate,
            "per_category": per_cat, "total_time": total_time, "details": v_results
        })

        print(f"\n  {v.version} done: PSNR={avg_psnr:.2f} SSIM={avg_ssim:.4f} "
              f"CPU={avg_time:.0f}ms ({total_time:.1f}s)")

    return results


def save_and_rank(results):
    RESULTS_DIR.mkdir(exist_ok=True)
    ranked = sorted(results, key=lambda r: r["avg_psnr"], reverse=True)
    baseline_psnr = next(r["avg_psnr"] for r in results if r["version"] == "v00")

    # CSV
    with open(RESULTS_DIR/"ranking.csv", "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["Rank","Ver","Name","AvgPSNR","AvgSSIM","SlowPSNR","MedPSNR",
                     "FastPSNR","AvgTimeMs","Cost","DeltaDB"])
        for i, r in enumerate(ranked, 1):
            w.writerow([i, r["version"], r["name"], f"{r['avg_psnr']:.3f}",
                        f"{r['avg_ssim']:.5f}",
                        f"{r['per_category'].get('slow',{}).get('psnr',0):.3f}",
                        f"{r['per_category'].get('medium',{}).get('psnr',0):.3f}",
                        f"{r['per_category'].get('fast',{}).get('psnr',0):.3f}",
                        f"{r['avg_time_ms']:.1f}", r["cost"],
                        f"{r['avg_psnr']-baseline_psnr:+.2f}"])

    # JSON
    best_deploy = min([r for r in ranked if r["avg_time_ms"] < 500],
                      key=lambda r: -r["avg_psnr"], default=ranked[0])
    best_research = ranked[0]

    summary = {
        "best_deploy": {"version": best_deploy["version"], "name": best_deploy["name"],
                        "psnr": best_deploy["avg_psnr"], "time_ms": best_deploy["avg_time_ms"]},
        "best_research": {"version": best_research["version"], "name": best_research["name"],
                          "psnr": best_research["avg_psnr"], "time_ms": best_research["avg_time_ms"]},
        "baseline": {"version": "v00", "psnr": baseline_psnr},
        "ranking": [{k: v for k, v in r.items() if k != "details"} for r in ranked]
    }
    with open(RESULTS_DIR/"summary.json", "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=2, ensure_ascii=False, default=str)

    # Print table
    print(f"\n{'='*95}")
    print(f"  FINAL RANKING")
    print(f"{'='*95}")
    print(f"{'Rk':>3} {'Ver':<5} {'Name':<38} {'PSNR':>7} {'SSIM':>7} "
          f"{'Slow':>7} {'Fast':>7} {'ms':>7} {'Delta':>8}")
    print(f"{'-'*95}")
    for i, r in enumerate(ranked, 1):
        d = r["avg_psnr"] - baseline_psnr
        mark = " <<DEPLOY" if r["version"] == best_deploy["version"] else ""
        mark = " <<RESEARCH" if r["version"] == best_research["version"] and not mark else mark
        print(f"{i:3} {r['version']:<5} {r['name']:<38} {r['avg_psnr']:7.3f} {r['avg_ssim']:7.4f} "
              f"{r['per_category'].get('slow',{}).get('psnr',0):7.3f} "
              f"{r['per_category'].get('fast',{}).get('psnr',0):7.3f} "
              f"{r['avg_time_ms']:7.0f} {d:+7.2f}dB{mark}")

    print(f"\n  BEST DEPLOY:   {best_deploy['version']} ({best_deploy['name']}) "
          f"PSNR={best_deploy['avg_psnr']:.2f} ({best_deploy['avg_psnr']-baseline_psnr:+.2f}dB)")
    print(f"  BEST RESEARCH: {best_research['version']} ({best_research['name']}) "
          f"PSNR={best_research['avg_psnr']:.2f} ({best_research['avg_psnr']-baseline_psnr:+.2f}dB)")
    return ranked, summary


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--versions", nargs="+", help="Run specific versions (e.g., v00 v02 v06)")
    args = parser.parse_args()

    print("="*60)
    print("  VipleStream FRUC 20-Iteration Benchmark")
    print("="*60)

    triplets = select_triplets()
    variants = ALL_VARIANTS
    if args.versions:
        variants = [v for v in ALL_VARIANTS if v.version in [a.lower() for a in args.versions]]

    results = run_all(triplets, variants)
    ranked, summary = save_and_rank(results)
    print(f"\n[DONE] Results: {RESULTS_DIR}/ranking.csv, {RESULTS_DIR}/summary.json")


if __name__ == "__main__":
    main()
