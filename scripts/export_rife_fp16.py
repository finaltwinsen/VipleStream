#!/usr/bin/env python3
"""
VipleStream G.2 path-alpha: re-export RIFE 4.25 lite from upstream PyTorch
checkpoint with native fp16, replacing the post-hoc onnxconverter_common
attempts that v1.2.61 + the v1.2.122 round all hit type-validation errors.

Pipeline:
  1. Load Practical-RIFE's IFNet from train_log/flownet.pkl
  2. Wrap in a FRUCWrapper that takes 7-channel concat input (matches the
     production C++ binding's `case 7:` branch in directmlfruc.cpp:1535)
  3. model.half() to convert ALL parameters + buffers to fp16 natively
  4. torch.onnx.export with opset 16 (matches existing fruc.onnx)

Native fp16 export differs from post-hoc convert because PyTorch's tracer
emits fp16 ops directly + can fuse Conv+Activation, producing fewer
total ONNX nodes (the v1.2.61 bottleneck on RTX A1000 was per-op kernel
launch overhead -- 25ms baseline regardless of compute -- so reducing
op count is what unlocks 1080p, not raw fp16 speedup).

Run from repo root with the dedicated rife venv:
    .venv-rife/Scripts/python.exe scripts/export_rife_fp16.py

Output: tools/fruc_fp16.onnx (~12 MB)
"""
import sys
import time
from pathlib import Path

REPO       = Path(__file__).resolve().parents[1]
RIFE_REPO  = REPO / "temp" / "Practical-RIFE"
RIFE_LITE  = REPO / "temp" / "rife-4.25-lite"
TRAIN_LOG  = RIFE_LITE / "train_log"
WEIGHTS    = TRAIN_LOG / "flownet.pkl"
DST        = REPO / "tools" / "fruc_fp16.onnx"

# Practical-RIFE expects `model/` and `train_log/` on PYTHONPATH (its
# inference scripts cd to the repo root and import accordingly).
sys.path.insert(0, str(RIFE_REPO))
sys.path.insert(0, str(RIFE_LITE))

if not WEIGHTS.exists():
    sys.exit(f"ERROR: weights not found at {WEIGHTS}\n"
             f"download RIFEv4.25lite_1018.zip and unzip into {RIFE_LITE}/")

import torch
import torch.nn as nn

print(f"PyTorch {torch.__version__}, CUDA available: {torch.cuda.is_available()}")
if not torch.cuda.is_available():
    print("WARNING: no CUDA -- export will run on CPU (slower but valid)")

device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
torch.set_grad_enabled(False)


# Load IFNet directly (skip the RIFE_HDv3.Model wrapper which has training
# hooks we don't want in the ONNX graph).
print(f"\nLoading IFNet from {WEIGHTS.relative_to(REPO)}…")
from train_log.IFNet_HDv3 import IFNet

flownet = IFNet().to(device)
state_dict = torch.load(str(WEIGHTS), map_location=device, weights_only=True)
# RIFE checkpoints are saved with a "module." prefix from DataParallel.  Strip
# it.  (Same logic as RIFE_HDv3.Model.load_model:36-49.)
clean_sd = {}
for k, v in state_dict.items():
    if k.startswith("module."):
        clean_sd[k[len("module."):]] = v
    else:
        clean_sd[k] = v
missing, unexpected = flownet.load_state_dict(clean_sd, strict=False)
print(f"  loaded; missing={len(missing)} unexpected={len(unexpected)}")
if missing:
    print(f"  missing keys (first 3): {missing[:3]}")
if unexpected:
    print(f"  unexpected keys (first 3): {unexpected[:3]}")


# ── Inference wrapper ────────────────────────────────────────────────────
# The C++ binding (directmlfruc.cpp `case 7:` branch) feeds a 7-channel
# concatenated tensor: prev_RGB(3) + curr_RGB(3) + timestep_plane(1).
# Wrapper splits, calls flownet, returns merged[4] (the final interpolated
# frame).  Matches the schema of the existing tools/fruc.onnx so we can
# swap models without touching the C++ binding code.
class FRUCWrapper(nn.Module):
    """
    Production-shaped wrapper: fp32 I/O at the boundary, fp16 internals.

    Mirrors onnxconverter_common's `keep_io_types=True` semantics but
    produced cleanly via PyTorch's native exporter.  The C++ binding
    (directmlfruc.cpp:1556) hard-rejects models whose I/O tensors aren't
    ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT (=fp32), and reworking that
    binding to track per-model element size everywhere (8 fp32 sites +
    HLSL shader format swap from R32_FLOAT to R16_FLOAT, etc.) is
    multi-day.  Keeping fp32 I/O makes the model a drop-in swap.
    """
    def __init__(self, flownet):
        super().__init__()
        self.flownet = flownet  # fp16 weights

    def forward(self, x_fp32):  # x: [B, 7, H, W] fp32 (boundary)
        # IFNet block[0] uses scale=32 + two stride-2 convs in conv0,
        # so H/W must be divisible by 128 for the F.interpolate(scale=32)
        # round-trip to recover the original shape exactly.  When the
        # client streams at 1920x1080, H=1080 is NOT a multiple of 128
        # (1080/128 = 8.4375) and the internal pyramid produces
        # mismatched widths/heights at different paths -- e.g. one
        # branch rounds to 1024, the timestep Tile stays at 1080,
        # then Concat_12 (the 7-input mid-block concat) sees mixed
        # shapes and DML's fp16 kernel rejects with E_INVALIDARG.
        #
        # Fix: F.pad input to next multiple of 128 here, run flownet,
        # then crop output back to the original dims.  This mirrors
        # what the older fruc.onnx (with its visible Padoutput_dim_*
        # wrapper) does.  Padding is "replicate" so edge pixels don't
        # introduce sharp discontinuities the warp would amplify.
        _, _, H, W = x_fp32.shape
        Hp = ((H + 127) // 128) * 128
        Wp = ((W + 127) // 128) * 128
        pad_b = Hp - H
        pad_r = Wp - W
        if pad_b > 0 or pad_r > 0:
            x_fp32 = torch.nn.functional.pad(x_fp32, (0, pad_r, 0, pad_b), mode='replicate')

        x = x_fp32.half()
        img0 = x[:, 0:3]
        img1 = x[:, 3:6]
        # Channel 6 is a uniform-value timestep plane (C++ binding fills
        # every pixel with the same scalar -- typically 0.5 for midpoint
        # interpolation).  IFNet's `else` branch expects timestep as
        # [B, 1, 1, 1] then broadcast-repeats it internally.  Passing
        # the full plane causes `.repeat(1, 1, H, W)` to allocate
        # H*H * W*W elements (an 8 TB request at 1080p).  Sample one px.
        timestep = x[:, 6:7, 0:1, 0:1]
        imgs = torch.cat((img0, img1), dim=1)
        scale_list = [32.0, 16.0, 8.0, 4.0, 1.0]
        flow_list, mask, merged = self.flownet(
            imgs, timestep, scale_list, training=False, fastmode=True
        )
        # Cast back to fp32 at output boundary AND crop padding off.
        out = merged[4].float()
        if pad_b > 0 or pad_r > 0:
            out = out[:, :, :H, :W]
        return out


# Convert ONLY the inner flownet to fp16, leave the wrapper itself fp32
# so its forward() emits boundary Cast nodes.
print("\nConverting flownet weights to fp16 (wrapper boundary stays fp32)…")
flownet = flownet.half()
wrapper = FRUCWrapper(flownet).to(device).eval()


# ── ONNX export ──────────────────────────────────────────────────────────
# Trace with the actual production stream resolution (1920x1080, NOT a
# multiple of 128).  Tracing with a 128-multiple input would short-circuit
# the wrapper's pad/crop logic at trace time, baking it OUT of the graph
# -- defeating the whole point of having that code.  Tracing with 1080
# means the trace evaluates pad_b > 0 to True and emits torch.nn.functional.pad
# + slice as graph ops that run dynamically per inference.
EXPORT_W, EXPORT_H = 1920, 1080
print(f"\nExporting ONNX with dummy [{EXPORT_W}x{EXPORT_H}] fp32 input (model casts to fp16 internally)…")
dummy = torch.randn(1, 7, EXPORT_H, EXPORT_W, dtype=torch.float32, device=device)

DST.parent.mkdir(parents=True, exist_ok=True)
t0 = time.perf_counter()
# Use the legacy TorchScript-based exporter (dynamo=False).  PyTorch
# 2.6's new dynamo path failed on IFNet's `1./scale` symbolic float
# arithmetic (DispatchError: No ONNX function for sym_float).  Fixing
# would require replacing scalar / sym_float ops with tensor-typed
# equivalents throughout IFNet -- non-trivial upstream surgery.  The
# legacy exporter handles these via TorchScript tracing fine.
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


# ── Structural sanity vs existing fp32 model ────────────────────────────
import onnx
m_new = onnx.load(str(DST))
m_old = onnx.load(str(REPO / "tools" / "fruc.onnx"))
print(f"\nNode-count comparison:")
from collections import Counter
old_ops = Counter(n.op_type for n in m_old.graph.node)
new_ops = Counter(n.op_type for n in m_new.graph.node)
all_op_types = sorted(set(old_ops) | set(new_ops))
print(f"  {'op_type':<24} {'old fp32':>10} {'new fp16':>10}")
for op in all_op_types:
    print(f"  {op:<24} {old_ops.get(op, 0):>10} {new_ops.get(op, 0):>10}")
print(f"  {'TOTAL':<24} {sum(old_ops.values()):>10} {sum(new_ops.values()):>10}")

print()
print("Run scripts/fruc_dml_bench.py next to compare DML latency.")
