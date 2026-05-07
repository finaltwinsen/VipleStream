#!/usr/bin/env python
"""Verify §B-DUMP captures: prove the interp frames are NOT identical to
the real frames they bracket.  Outputs diff stats + visual diff PNGs.

Layout (2026-05-07 flat, matches D3D11VARenderer + VkFrucRenderer):
  frame_NNNN_real.bmp     =  real frame at display index NNNN
  frame_NNNN_interp.bmp   =  interp at display index NNNN
                             (interp(real_prev, real_curr) where prev/curr
                             are the bracketing _real files in name-sort
                             order, i.e. file 2N+1 = mid(file 2N, file 2N+2))

Legacy fallback: pre-2026-05-07 VkFruc dumps had `real/` + `all/` subdirs
with different indexing — auto-detect below.

KEY INSIGHT: most of a typical streamed frame is static (cursor + small
moving regions).  Whole-frame mean diff is dominated by static pixels
where interp ~= real (correct behavior).  To see whether FRUC actually
interpolates, focus on MOVING regions — pixels where real_N differs from
real_N+1 — and compare interp against the midpoint(real_N-1, real_N).
"""
import sys
from pathlib import Path
from PIL import Image
import numpy as np

if len(sys.argv) < 2:
    print("usage: verify_dump_interp.py <dump_dir>")
    sys.exit(1)

root = Path(sys.argv[1])
real_dir = root / "real"
all_dir  = root / "all"

# Detect layout.  Flat = new (2026-05-07), subdir = legacy.
flat_reals   = sorted(root.glob("frame_*_real.bmp"))
flat_interps = sorted(root.glob("frame_*_interp.bmp"))
legacy = real_dir.is_dir() and all_dir.is_dir()

if flat_reals or flat_interps:
    layout = "flat"
    reals = flat_reals
    # In flat layout, the analyzer needs the real frames bracketing each
    # interp — we reconstruct an "alls"-equivalent list (alternating real
    # at even, interp at odd) so the rest of the script stays unchanged.
    # Sort all bmps together; their lex order = name index = display order.
    alls = sorted(root.glob("frame_*.bmp"))
elif legacy:
    layout = "legacy"
    reals = sorted(real_dir.glob("frame_*.bmp"))
    alls  = sorted(all_dir.glob("frame_*.bmp"))
else:
    print(f"no dump frames found in {root} (checked flat and legacy layouts)")
    sys.exit(1)

print(f"layout: {layout}  reals: {len(reals)}  alls: {len(alls)}")

if len(reals) < 3 or len(alls) < 6:
    print("not enough frames (need >= 3 real + >= 6 alls)")
    sys.exit(1)

def load(p):
    return np.asarray(Image.open(p).convert("RGB"), dtype=np.float32)

print()
print(f"{'pair':<54s}{'mean':>10s}{'mean(motion)':>14s}{'motion%':>10s}")
print("-" * 88)

# Definitions used below:
#   real_Nm1 = real frame N-1  (= prev at chain time of frame N)
#   real_N   = real frame N    (= curr at chain time of frame N)
#   interp   = warp output at frame N = mid(prev=N-1, curr=N)
#
# Verdict logic: in motion regions (where |real_Nm1 - real_N| > 8),
#   * if interp ~= real_N           --> FRUC NOT interpolating (just curr copy)
#   * if interp ~= real_Nm1          --> FRUC NOT interpolating (just prev copy)
#   * if interp ~= midpoint           --> FRUC genuinely interpolating [OK]

interp_works_count = 0
n_pairs = min(5, len(reals) - 2)
for n in range(1, 1 + n_pairs):
    real_Nm1  = load(reals[n - 1])
    real_N    = load(reals[n])
    # Pick interp bracketing real_(n-1) and real_n.
    # Legacy (all/+real/ subdir): alls[2n+1] = interp(N-1, N) (vkfruc backward)
    # Flat (2026-05-07+):           alls[2n-1] = interp(N-1, N) (forward, alls[2k] = real_k)
    if layout == "legacy":
        interp = load(alls[2 * n + 1])
    else:
        interp = load(alls[2 * n - 1])

    # Motion mask: pixels where curr differs from prev.
    motion_mask = np.any(np.abs(real_N - real_Nm1) > 8, axis=-1)
    motion_pct = 100.0 * motion_mask.sum() / motion_mask.size

    diff_to_curr  = np.abs(real_N - interp)
    diff_to_prev  = np.abs(real_Nm1 - interp)
    midpoint      = (real_N + real_Nm1) * 0.5
    diff_to_mid   = np.abs(midpoint - interp)
    diff_motion   = np.abs(real_N - real_Nm1)

    # Mean over MOTION region only (where there's actual motion to interpolate).
    if motion_mask.sum() > 0:
        mean_to_curr_mot = diff_to_curr[motion_mask].mean()
        mean_to_prev_mot = diff_to_prev[motion_mask].mean()
        mean_to_mid_mot  = diff_to_mid[motion_mask].mean()
        mean_motion_mag  = diff_motion[motion_mask].mean()
    else:
        mean_to_curr_mot = mean_to_prev_mot = mean_to_mid_mot = mean_motion_mag = 0

    print(f"\nframe N={n}  (motion_pct={motion_pct:.2f}%, mean motion magnitude={mean_motion_mag:.2f}):")
    print(f"  in motion region:  diff(real_N,    interp)  = {mean_to_curr_mot:8.3f}")
    print(f"  in motion region:  diff(real_N-1,  interp)  = {mean_to_prev_mot:8.3f}")
    print(f"  in motion region:  diff(midpoint,  interp)  = {mean_to_mid_mot:8.3f}  <- smallest = FRUC IS midpoint-interpolating")

    # Verdict for this pair: how close is interp to true midpoint?
    # Ideal midpoint produces diff(real_N, interp) ~= motion_magnitude / 2.
    # Pure curr-copy produces diff(real_N, interp) ~= 0.
    # Pure prev-copy produces diff(real_N, interp) ~= motion_magnitude.
    # Score: diff(real_N, interp) / (motion_magnitude / 2)
    #   0.0  = interp == real_N (no interpolation)
    #   1.0  = interp halfway (perfect midpoint)
    #   2.0  = interp == real_N-1 (full prev-copy)
    if motion_mask.sum() < 100:
        print(f"  VERDICT: insufficient motion to evaluate")
        continue
    if mean_motion_mag < 1:
        print(f"  VERDICT: motion too tiny to evaluate")
        continue
    score = mean_to_curr_mot / (mean_motion_mag / 2.0)
    if 0.7 <= score <= 1.3:
        print(f"  VERDICT: [OK] score={score:.2f} (~1.0 = perfect midpoint) --> FRUC interpolating")
        interp_works_count += 1
    elif score < 0.3:
        print(f"  VERDICT: [FAIL] score={score:.2f} (~0.0 = no interp) --> FRUC NOT interpolating, copying curr")
    elif score < 0.7:
        print(f"  VERDICT: [PARTIAL] score={score:.2f} --> partial interp, ME finding some motion but not all blocks")
        interp_works_count += 0.5
    else:
        print(f"  VERDICT: [FAIL] score={score:.2f} (>1.3) --> interp overshooting toward prev")

print()
print("=" * 88)
print(f"OVERALL: {interp_works_count}/{n_pairs} pairs show real interpolation")
if interp_works_count == n_pairs:
    print("-> FRUC is working correctly.")
elif interp_works_count == 0:
    print("-> FRUC NOT interpolating - just copying curr or prev.  Bug!")
else:
    print(f"-> FRUC works on some frames but not others - likely ME inconsistent.")
