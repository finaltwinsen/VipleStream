#!/usr/bin/env python3
"""
VipleStream G.3: export IFRNet-S (CVPR 2022 small variant) to ONNX
fp16 with the same 7-channel concat I/O contract as the existing
fruc.onnx / fruc_fp16.onnx so the C++ binding can pick it up via the
cascade without changes.

Why try IFRNet-S
- RIFE 4.25 lite has ~9.5M params and ~456-529 ONNX ops.  On A1000
  C++ DML pipeline runs fp32 1080p in 107 ms (10x over budget),
  fp16 in 132 ms (mostly hardware floor of ~25 ms launch overhead).
- IFRNet-S is ~2.4M params, single encoder-decoder (no 5-block
  pyramid), expected ~30-40% RIFE op count.  Smaller op count means
  fewer kernel launches -> directly attacks A1000's bottleneck.
- Same 2-image + scalar-timestep contract as RIFE, so wrapper is
  trivial.

Output: tools/fruc_ifrnet_s.onnx (fp32 boundary, fp16 internal).

Run from repo root with the rife venv:
    .venv-rife/Scripts/python.exe scripts/export_ifrnet_s.py
"""
import sys
import time
from pathlib import Path

REPO        = Path(__file__).resolve().parents[1]
IFRNET_REPO = REPO / "temp" / "IFRNet"
WEIGHTS     = REPO / "temp" / "ifrnet-weights" / "checkpoints" / "IFRNet_small" / "IFRNet_S_Vimeo90K.pth"
DST         = REPO / "tools" / "fruc_ifrnet_s.onnx"

# IFRNet repo expects to import `models.IFRNet_S` and `utils` from its root.
sys.path.insert(0, str(IFRNET_REPO))

if not WEIGHTS.exists():
    sys.exit(f"ERROR: weights not found at {WEIGHTS}")

import torch
import torch.nn as nn

print(f"PyTorch {torch.__version__}, CUDA: {torch.cuda.is_available()}")
device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
torch.set_grad_enabled(False)

# Model has training-only loss attributes; load_state_dict with strict=False.
print(f"\nLoading IFRNet-S from {WEIGHTS.relative_to(REPO)}…")
from models.IFRNet_S import Model as IFRNetS

model = IFRNetS().to(device).eval()
state_dict = torch.load(str(WEIGHTS), map_location=device, weights_only=True)
clean_sd = {k.removeprefix("module."): v for k, v in state_dict.items()}
missing, unexpected = model.load_state_dict(clean_sd, strict=False)
print(f"  loaded; missing={len(missing)} unexpected={len(unexpected)}")
if missing:
    print(f"  missing (first 3): {missing[:3]}")


# Wrapper: 7-channel concat input → 3-channel interpolated frame.
# Matches the contract used by tools/fruc.onnx and tools/fruc_fp16.onnx
# so the C++ binding's `case 7:` branch in directmlfruc.cpp:1535 can
# load this model with no special-casing.  The cascade in d3d11va.cpp
# only varies the model FILENAME, not the contract.
class FRUCWrapperIFRNet(nn.Module):
    """fp32 I/O at the boundary, fp16 internal."""
    def __init__(self, ifrnet):
        super().__init__()
        self.ifrnet = ifrnet  # fp16 weights

    def forward(self, x_fp32):  # x: [B, 7, H, W] fp32
        # Pad to next multiple of 16 (encoder is 4 stride-2 levels).
        # Use 128 for safety against internal Pad/Resize paths -- same
        # as the RIFE wrapper -- so any future model swap drop-in works.
        _, _, H, W = x_fp32.shape
        Hp = ((H + 127) // 128) * 128
        Wp = ((W + 127) // 128) * 128
        pad_b = Hp - H
        pad_r = Wp - W
        if pad_b > 0 or pad_r > 0:
            x_fp32 = torch.nn.functional.pad(
                x_fp32, (0, pad_r, 0, pad_b), mode='replicate')

        x = x_fp32.half()
        img0 = x[:, 0:3]
        img1 = x[:, 3:6]
        embt = x[:, 6:7, 0:1, 0:1]  # [B, 1, 1, 1]
        out = self.ifrnet.inference(img0, img1, embt, scale_factor=1.0)
        out = out.float()
        if pad_b > 0 or pad_r > 0:
            out = out[:, :, :H, :W]
        return out


print("\nConverting IFRNet-S weights to fp16…")
model = model.half()
wrapper = FRUCWrapperIFRNet(model).to(device).eval()


# Trace at production resolution (NOT a 128 multiple) so pad/crop
# emit dynamic shape ops (same trick as the RIFE export).
EXPORT_W, EXPORT_H = 1920, 1080
print(f"\nExporting ONNX with dummy [{EXPORT_W}x{EXPORT_H}] fp32 input…")
dummy = torch.randn(1, 7, EXPORT_H, EXPORT_W, dtype=torch.float32, device=device)

DST.parent.mkdir(parents=True, exist_ok=True)
t0 = time.perf_counter()
torch.onnx.export(
    wrapper,
    (dummy,),
    str(DST),
    input_names=["input"],
    output_names=["output"],
    dynamic_axes={
        "input":  {0: "batch", 2: "height", 3: "width"},
        "output": {0: "batch", 2: "height", 3: "width"},
    },
    opset_version=16,
    do_constant_folding=True,
    verbose=False,
)
print(f"  exported in {time.perf_counter() - t0:.1f}s")

size_mb = DST.stat().st_size / 1e6
print(f"\nSaved {DST.relative_to(REPO)} ({size_mb:.1f} MB)")


# Op count comparison against the existing RIFE fp32 + fp16 models.
import onnx
from collections import Counter
m_new   = onnx.load(str(DST))
m_rfp32 = onnx.load(str(REPO / "tools" / "fruc.onnx"))
m_rfp16 = onnx.load(str(REPO / "tools" / "fruc_fp16.onnx"))

new_ops   = Counter(n.op_type for n in m_new.graph.node)
rfp32_ops = Counter(n.op_type for n in m_rfp32.graph.node)
rfp16_ops = Counter(n.op_type for n in m_rfp16.graph.node)
all_ops   = sorted(set(new_ops) | set(rfp32_ops) | set(rfp16_ops))

print(f"\nOp-count comparison ({DST.name} vs RIFE):")
print(f"  {'op':<24} {'IFRNet-S':>10} {'RIFE fp32':>12} {'RIFE fp16':>12}")
for op in all_ops:
    n, r32, r16 = new_ops.get(op, 0), rfp32_ops.get(op, 0), rfp16_ops.get(op, 0)
    if n + r32 + r16 == 0:
        continue
    print(f"  {op:<24} {n:>10} {r32:>12} {r16:>12}")
print(f"  {'TOTAL':<24} {sum(new_ops.values()):>10} "
      f"{sum(rfp32_ops.values()):>12} {sum(rfp16_ops.values()):>12}")

print("\nNext: run scripts/fruc_dml_bench.py with model paths to compare A1000 latency.")
