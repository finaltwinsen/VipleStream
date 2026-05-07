#!/usr/bin/env python
"""Analyze §B-DUMP fruc_compare output to gauge if FRUC is actually
interpolating, for each engine.

Method: gdigrab captures display at 60fps for 1s = 60 PNG.
For DUAL FRUC working at server fps 30:
  - frames 0,2,4,...  = real frames (server-delivered)
  - frames 1,3,5,...  = interp frames (FRUC-generated)
  - mid-frame diff |frame_i - mid(frame_(i-1), frame_(i+1))| should be SMALL
    if FRUC is producing genuine midpoints

For "no FRUC" baseline at server fps 60:
  - all frames are real, motion is uniform
  - mid-frame diff is approximately motion magnitude / 2 (= half of pairwise)

For "FRUC failed" (interp == real, just frame doubling):
  - alternating identical pairs: frame_0 = frame_1, frame_2 = frame_3, ...
  - pairwise(0,1) = 0 (identical)
  - pairwise(0,2) = full motion
  - mid-frame diff |frame_1 - mid(frame_0, frame_2)| = motion/2 (because frame_1 = frame_0)

Discriminator:
  - frame_doubling_score = mean( |frame_(2i) - frame_(2i+1)| ) / mean( |frame_(2i) - frame_(2i+2)| )
    - 0 = perfect doubling (FRUC failed)
    - 0.5 = ideal midpoint (FRUC working)
    - 1.0 = no relationship (random or sub-frame motion sampling)

  - midpoint_residual = mean( |frame_(2i+1) - mid(frame_(2i), frame_(2i+2))| ) / mean motion
    - 0 = perfect interpolation
    - 0.5 = essentially curr-copy (interp = real_N)
    - >1 = artifacts (extrapolation, reverse motion, etc.)
"""
import sys
from pathlib import Path
from PIL import Image
import numpy as np

if len(sys.argv) < 2:
    print("usage: analyze_fruc_compare.py <out_root>")
    sys.exit(1)

root = Path(sys.argv[1])
if not root.is_dir():
    print(f"directory not found: {root}")
    sys.exit(1)

def load(p):
    return np.asarray(Image.open(p).convert("RGB"), dtype=np.float32)

def analyze_engine(eng_dir):
    # Layout 2026-05-07: BOTH D3D11 and VkFruc engines now write a flat
    # directory of `frame_NNNN_real.bmp` / `frame_NNNN_interp.bmp` files
    # using the same forward-indexing convention:
    #   bmps[2N]=real_N, bmps[2N+1]=interp(real_N, real_N+1)
    # — VkFruc skips the very first interp dump (no prev available) so its
    # cycle 0 only produces real_0, lining up file 0 = real for both.
    #
    # Backward-compat: pre-2026-05-07 VkFruc dumps used `all/` + `real/`
    # subdirs with a different indexing.  We auto-detect and fall back so
    # old captures still parse.
    legacy_vkfruc = (eng_dir / 'all').is_dir() and (eng_dir / 'real').is_dir()
    if legacy_vkfruc:
        pngs = sorted((eng_dir / 'all').glob('frame_*.bmp'))
    else:
        pngs = sorted(eng_dir.glob('frame_*.bmp'))
    if not pngs:
        pngs = sorted(eng_dir.glob("frame_*.png"))
    if len(pngs) < 6:
        return None

    # Sample first 30 frames (or all if fewer).
    n = min(30, len(pngs))
    frames = [load(p) for p in pngs[:n]]

    # Compute pairwise diffs at adjacent (i, i+1) and skip-one (i, i+2).
    # Even-indexed pairs:  (0,1), (2,3), (4,5)... = "interp side" if FRUC active
    # Skip-one even pairs: (0,2), (2,4), (4,6)... = real-to-real

    pair_adj = []   # |frame_2i - frame_2i+1|
    pair_skip = []  # |frame_2i - frame_2i+2|
    pair_other_adj = []  # |frame_2i+1 - frame_2i+2|
    interp_resid = []    # how far interp is from its expected midpoint
    motion_overall = []  # |frame_2i - frame_2i+2|, same as pair_skip

    # For legacy VkFruc all/+real/ dumps: interp at slot 2N+1 = mid(N-1, N),
    # so we look BACKWARD:
    #   real_N-1 = bmps[2N-2], real_N = bmps[2N], interp = bmps[2N+1]
    # For D3D11 + new flat VkFruc dumps: interp at slot 2N+1 = mid(N, N+1),
    # look forward:
    #   real_N = bmps[2N], real_N+1 = bmps[2N+2], interp = bmps[2N+1]
    if legacy_vkfruc:
        # VkFruc backward indexing
        for i in range(2, n - 1, 2):
            real_prev = frames[i - 2]   # real_N-1
            interp    = frames[i + 1]   # interp(N-1, N) at chain-end of N+1
            real_curr = frames[i]       # real_N

            motion = float(np.mean(np.abs(real_prev - real_curr)))
            mid = (real_prev + real_curr) * 0.5
            pair_adj.append(float(np.mean(np.abs(real_prev - interp))))
            pair_skip.append(motion)
            pair_other_adj.append(float(np.mean(np.abs(interp - real_curr))))
            interp_resid.append(float(np.mean(np.abs(mid - interp))))
            motion_overall.append(motion)
    else:
        # D3D11 forward indexing
        for i in range(0, n - 2, 2):
            a = frames[i]      # real_N
            b = frames[i + 1]  # interp(N, N+1)
            c = frames[i + 2]  # real_N+1

            diff_ab = float(np.mean(np.abs(a - b)))
            diff_bc = float(np.mean(np.abs(b - c)))
            diff_ac = float(np.mean(np.abs(a - c)))

            pair_adj.append(diff_ab)
            pair_other_adj.append(diff_bc)
            pair_skip.append(diff_ac)

            mid = (a + c) * 0.5
            interp_resid.append(float(np.mean(np.abs(b - mid))))
            motion_overall.append(diff_ac)

    return {
        "n_pairs":           len(pair_adj),
        "mean_pair_adj":     np.mean(pair_adj),
        "mean_pair_skip":    np.mean(pair_skip),
        "mean_motion":       np.mean(motion_overall),
        "mean_interp_resid": np.mean(interp_resid),
        # Frame-doubling score: how close are adjacent pairs to identical?
        # 0 = identical (frame doubling), 0.5 = midpoint, 1.0 = full motion
        "doubling_score":    np.mean(pair_adj) / max(np.mean(pair_skip), 1e-6),
        # Interp residual ratio: how close is interp to true midpoint?
        # 0 = perfect midpoint, ~0.5 = curr-copy, > 0.5 = artifacts
        "interp_resid_ratio": np.mean(interp_resid) / max(np.mean(motion_overall), 1e-6),
    }

# Iterate engine subdirs.
results = []
for sub in sorted(root.iterdir()):
    if not sub.is_dir(): continue
    res = analyze_engine(sub)
    if res:
        res["engine"] = sub.name
        results.append(res)
    else:
        print(f"  skipped {sub.name} (insufficient frames)")

if not results:
    print("No engines with enough frames.")
    sys.exit(1)

# Print table
print()
print(f"{'engine':<26s}{'pairs':>6}{'mean_motion':>13}{'pair_adj':>10}{'pair_skip':>11}{'interp_resid':>14}{'doubling':>11}{'interp_r':>11}  verdict")
print("-" * 132)

for r in results:
    # Verdict heuristic:
    #   no_motion: motion < 3
    #   frame_doubling: pair_adj is much smaller than pair_skip (= frames really repeated)
    #   real_interp: interp_resid_ratio < 0.3 (interp close to midpoint)
    #   artifacts: interp_resid_ratio > 0.7 (interp far from midpoint, possibly reversed)
    #   weak_interp: 0.3 <= interp_resid_ratio < 0.7 (some interp, partial)
    motion = r["mean_motion"]
    if motion < 3:
        verdict = "STATIC (no motion to evaluate)"
    elif r["doubling_score"] < 0.15:
        verdict = "[FAIL] FRAME DOUBLING (interp = real, FRUC not working)"
    elif r["interp_resid_ratio"] < 0.25:
        verdict = "[OK] TRUE INTERPOLATION (interp ~ midpoint)"
    elif r["interp_resid_ratio"] < 0.5:
        verdict = "[PARTIAL] interp partial - some midpoint, some fallback"
    else:
        verdict = "[FAIL/ARTIFACT] interp far from midpoint (possible reverse-motion artifacts)"

    print(f"{r['engine']:<26s}{r['n_pairs']:>6}{motion:>13.2f}{r['mean_pair_adj']:>10.2f}{r['mean_pair_skip']:>11.2f}{r['mean_interp_resid']:>14.2f}{r['doubling_score']:>11.3f}{r['interp_resid_ratio']:>11.3f}  {verdict}")

print()
print("Notes:")
print("  pairs            = number of (real, interp, real) triples sampled")
print("  mean_motion      = mean abs diff between adjacent supposed-real frames (real-to-real)")
print("  pair_adj         = mean |frame_(2i) - frame_(2i+1)|     (real -> supposed-interp)")
print("  pair_skip        = mean |frame_(2i) - frame_(2i+2)|     (real -> next real)")
print("  interp_resid     = mean |frame_(2i+1) - midpoint|       (how far interp is from midpoint)")
print("  doubling         = pair_adj / pair_skip")
print("                       0.0 = frames i+1 identical to i  (frame doubling, FRUC off/failed)")
print("                       0.5 = midpoint behavior")
print("                       1.0 = full motion stride 1 (no FRUC, server fps = display fps)")
print("  interp_r         = interp_resid / motion")
print("                       0.0 = perfect midpoint, FRUC working")
print("                       0.5 = interp == real_N (curr copy fallback)")
print("                       >0.7 = interp at wrong position (artifacts)")
