#!/usr/bin/env python
"""Check whether interp's motion direction is forward or reversed.

For each pixel where motion exists between real_N-1 and real_N:
  - "Forward correct":  interp pixel ~= midpoint(real_N-1[pp], real_N[pp])
  - "Reversed":         interp pixel ~= what's at FUTURE position in real_N
                        (e.g. if motion is right, interp shows pixel from RIGHT
                        of where it should be -> appears "ahead" of real motion)

We do this by checking optical-flow-like correspondence with a tiny window.
For each "moving" pixel pp, find which neighbor in real_N most closely
matches what interp shows at pp.  If that neighbor is in motion direction
(prev->curr), interp is correct.  If opposite, reversed.
"""
import sys
from pathlib import Path
from PIL import Image
import numpy as np

if len(sys.argv) < 2:
    print("usage: check_motion_direction.py <dump_dir>")
    sys.exit(1)

root = Path(sys.argv[1])

# Detect dump layout (flat new vs legacy real/+all/ subdirs).  See
# verify_dump_interp.py for the same auto-detect.
# 2026-05-08: also accept .png (Android FrucDumpWriter port).
flat_reals   = sorted(list(root.glob("frame_*_real.bmp"))
                    + list(root.glob("frame_*_real.png")))
flat_interps = sorted(list(root.glob("frame_*_interp.bmp"))
                    + list(root.glob("frame_*_interp.png")))
legacy_real_dir = root / "real"
legacy_all_dir  = root / "all"

if flat_reals or flat_interps:
    layout = "flat"
    reals = flat_reals
    alls  = sorted(list(root.glob("frame_*.bmp"))
                 + list(root.glob("frame_*.png")))
elif legacy_real_dir.is_dir() and legacy_all_dir.is_dir():
    layout = "legacy"
    reals = sorted(legacy_real_dir.glob("frame_*.bmp"))
    alls  = sorted(legacy_all_dir.glob("frame_*.bmp"))
else:
    print(f"no dump frames found in {root}")
    sys.exit(1)

if len(reals) < 3 or len(alls) < 6:
    print(f"not enough frames (layout={layout}, reals={len(reals)}, alls={len(alls)})")
    sys.exit(1)

def load(p):
    return np.asarray(Image.open(p).convert("RGB"), dtype=np.float32)

# Take frame N=1 (we have real_N-1 + real_N + interp bracketing them).
#   Legacy VkFruc: alls[2N+1] = interp(N-1, N), so for N=1: alls[3]
#   Flat (2026-05-07+): alls[2N-1] = interp(N-1, N), so for N=1: alls[1]
# Both give us interp(real_0, real_1) bracketed by real_0 = reals[0] and
# real_1 = reals[1].
real_Nm1 = load(reals[0])
real_N   = load(reals[1])
if layout == "legacy":
    interp = load(alls[3])
else:
    interp = load(alls[1])

H, W, _ = real_N.shape

# Estimate motion direction at each pixel by simple cross-correlation
# in horizontal axis (assuming motion is mostly horizontal).
# Simple heuristic: which side does interp pixel match better?
#   If interp[pp] ≈ real_N[pp - shift] for shift in motion direction → forward
#   If interp[pp] ≈ real_N[pp + shift] → reversed (interp appears ahead)

# Compute pixel-wise "shift" by sliding interp against real_N along x:
# For each pp, find best shift in [-15, +15] minimizing |real_N[pp+shift] - interp[pp]|

# Sample a sparse grid for speed (every 8x8).
step = 8
ys = np.arange(0, H, step)
xs = np.arange(0, W, step)

# Only consider locations with significant motion (real_N differs from real_Nm1).
motion_mask = np.any(np.abs(real_N - real_Nm1) > 16, axis=-1)

shifts = []
shift_range = list(range(-20, 21))
for yi, y in enumerate(ys):
    for xi, x in enumerate(xs):
        if not motion_mask[y, x]:
            continue
        # Try shifts in x direction
        target = interp[y, x]
        best_shift = 0
        best_err = 1e9
        for s in shift_range:
            xs_target = x + s
            if 0 <= xs_target < W:
                err = float(np.sum(np.abs(real_N[y, xs_target] - target)))
                if err < best_err:
                    best_err = err
                    best_shift = s
        shifts.append(best_shift)

shifts = np.array(shifts)
print(f"Sampled {len(shifts)} motion pixels")
print(f"Shift stats:  mean={shifts.mean():.2f}  median={np.median(shifts):.2f}")
print(f"  Shift > 0  (interp matches real_N at LATER x):  {(shifts>0).sum()}  ({100*(shifts>0).mean():.1f}%)")
print(f"  Shift < 0  (interp matches real_N at EARLIER x): {(shifts<0).sum()}  ({100*(shifts<0).mean():.1f}%)")
print(f"  Shift = 0  (no offset, interp ~= real_N at pp):  {(shifts==0).sum()}  ({100*(shifts==0).mean():.1f}%)")

# Compare with motion direction between real_N-1 and real_N
shifts_real = []
for yi, y in enumerate(ys):
    for xi, x in enumerate(xs):
        if not motion_mask[y, x]:
            continue
        target = real_N[y, x]
        best_shift = 0
        best_err = 1e9
        for s in shift_range:
            xs_target = x + s
            if 0 <= xs_target < W:
                err = float(np.sum(np.abs(real_Nm1[y, xs_target] - target)))
                if err < best_err:
                    best_err = err
                    best_shift = s
        shifts_real.append(best_shift)

shifts_real = np.array(shifts_real)
print()
print(f"Reference: motion from real_N-1 to real_N (where N's pixel came from in N-1):")
print(f"Shift stats:  mean={shifts_real.mean():.2f}  median={np.median(shifts_real):.2f}")

print()
print("DIAGNOSIS:")
print(f"  real_N -> real_N-1 mean shift   = {shifts_real.mean():.2f}  (= -motion direction)")
print(f"  real_N -> interp   mean shift   = {shifts.mean():.2f}  (~= -motion/2 if midpoint correct)")
print()
if abs(shifts.mean()) < 0.5:
    print(" WARNING: interp shift near zero -> interp ~= real_N (no interpolation)")
elif np.sign(shifts.mean()) != np.sign(shifts_real.mean()):
    print(" *** MOTION REVERSED *** interp's apparent motion is OPPOSITE of real motion!")
    print("    -> warp shader sign convention bug")
elif abs(shifts.mean()) < abs(shifts_real.mean()) * 0.3:
    print("  PARTIAL: interp moves same direction but smaller magnitude than midpoint")
elif abs(shifts.mean() - shifts_real.mean() * 0.5) < abs(shifts_real.mean()) * 0.3:
    print(" [OK] interp shift ~= half of real motion -> midpoint interpolation correct")
else:
    print(f"  shift magnitude unusual: interp shift={shifts.mean():.2f} vs expected midpoint={shifts_real.mean()/2:.2f}")
