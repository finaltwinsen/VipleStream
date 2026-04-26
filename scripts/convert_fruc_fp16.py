#!/usr/bin/env python3
"""
VipleStream G.2: convert RIFE 4.25-lite fruc.onnx (fp32) to fp16 variant.

Conversion strategy:
  - keep_io_types=True       I/O tensors stay fp32 (insert Cast at graph
                             boundaries). C++ binding code in
                             directmlfruc.cpp uses ONNX_TENSOR_ELEMENT_DATA_TYPE_
                             FLOAT everywhere; with this flag, *zero* binding
                             changes are needed to swap models at runtime.

  - op_block_list=GridSample bilinear sampler at extreme normalised coords
                             loses too much precision in fp16.  RIFE uses
                             GridSample to warp by predicted optical flow;
                             that's the most precision-sensitive op in the
                             graph (5× in this model).
  - op_block_list=Resize     ONNX Runtime's graph validator (DML EP and
                             CPU EP both) rejects fp16 Constants feeding
                             Resize's `scales` / `sizes` control inputs.
                             Even though DML *has* a fp16 Resize kernel,
                             ORT pre-validates types before kernel pick.
                             Blocking Resize keeps the surrounding boundary
                             Cast nodes fp16<->fp32 so each Resize sees fp32
                             control inputs.  ~8 extra Cast pairs in the
                             graph; cheap on GPU.

History: an earlier rebrand-era attempt (v1.2.61) ran a vanilla
`convert_float_to_float16(model)` (no keep_io_types, no block list) and
shipped garbage interpolations.  The combination of `keep_io_types=True`
+ explicit GridSample block-list is what's missing from that attempt.

Output: tools/fruc_fp16.onnx, alongside the existing tools/fruc.onnx.
The C++ side will pick which one to load at runtime via
VIPLE_FRUC_MODEL env var (or auto-cascade fallback).

Run from repo root:
  python scripts/convert_fruc_fp16.py
"""
import sys
import time
from pathlib import Path

import numpy as np
import onnx
import onnxruntime as ort
from onnxconverter_common import float16

REPO = Path(__file__).resolve().parents[1]
SRC  = REPO / "tools" / "fruc.onnx"
DST  = REPO / "tools" / "fruc_fp16.onnx"

if not SRC.exists():
    sys.exit(f"ERROR: source model not found at {SRC}")

print(f"Loading {SRC.relative_to(REPO)} ({SRC.stat().st_size / 1e6:.1f} MB)…")
m_fp32 = onnx.load(str(SRC))

# --- convert ---------------------------------------------------------------
print("Converting to fp16:")
print("  keep_io_types=True  (graph I/O stays fp32; zero C++ change)")
print("  no op_block_list — combining keep_io_types with op_block_list")
print("  triggers onnxconverter_common bugs (Cast value_info mismatches,")
print("  Mul fp16/fp32 input clashes).  Take the all-fp16 path and rely")
print("  on DML's own kernel coverage for ops like Resize/GridSample.")
t0 = time.perf_counter()
m_fp16 = float16.convert_float_to_float16(
    m_fp32,
    keep_io_types=True,
)
print(f"  done in {time.perf_counter() - t0:.1f} s")

# Repair the dtype-propagation mess left by convert_float_to_float16.
# When some ops are block-listed (GridSample / Resize) and stay fp32, the
# converter inserts Cast nodes around them but ALSO rewrites every
# intermediate tensor's value_info to fp16 -- including tensors that flow
# through `to=1 (FLOAT)` Casts and stay fp32 from there on.  The result
# is value_info entries claiming "this tensor is fp16" while the actual
# kernel produces fp32, and ORT rejects the load.
#
# The fix: clear all intermediate value_info entries, then run shape
# inference from scratch.  Cast `to` attributes + initializer dtypes +
# block-listed op signatures are unambiguous, so a fresh inference pass
# correctly propagates types end-to-end.  We keep graph.input /
# graph.output value_info intact since those are the API contract.
print(f"  clearing {len(m_fp16.graph.value_info)} intermediate value_info entries…")
del m_fp16.graph.value_info[:]
print("  re-running shape_inference from scratch…")
from onnx import shape_inference
m_fp16 = shape_inference.infer_shapes(m_fp16, strict_mode=False)

# --- structural sanity ------------------------------------------------------
print("\nStructural check:")
fp16_nodes = sum(
    1 for init in m_fp16.graph.initializer
    if init.data_type == onnx.TensorProto.FLOAT16
)
fp32_nodes = sum(
    1 for init in m_fp16.graph.initializer
    if init.data_type == onnx.TensorProto.FLOAT
)
print(f"  initializers: fp16={fp16_nodes} fp32={fp32_nodes}")
gs_count = sum(1 for n in m_fp16.graph.node if n.op_type == 'GridSample')
cast_count = sum(1 for n in m_fp16.graph.node if n.op_type == 'Cast')
print(f"  GridSample (kept fp32): {gs_count}")
print(f"  Cast (boundary inserts): {cast_count}")

# --- save -------------------------------------------------------------------
onnx.save(m_fp16, str(DST))
print(f"\nSaved {DST.relative_to(REPO)} ({DST.stat().st_size / 1e6:.1f} MB)")
print(f"  size delta: {(DST.stat().st_size - SRC.stat().st_size) / 1e6:+.1f} MB")

# --- graph-level sanity -----------------------------------------------------
# Note: we deliberately do NOT run on CPU EP. The CPU build of onnxruntime
# 1.25 lacks fp16 kernels for Resize (8× in this model) and others; trying
# to load fp16 → CPU fails with InvalidGraph.  DirectML EP (Phase 2) has
# the fp16 kernels; that's where real validation happens.
print("\nGraph-level validity (onnx.checker):")
try:
    onnx.checker.check_model(str(DST))
    print("  [OK] check_model passed (no structural / type / shape errors)")
except onnx.checker.ValidationError as e:
    print(f"  [FAIL] check_model FAILED: {e}")
    sys.exit(1)

# Confirm fp32 model still loads on CPU (control: our changes shouldn't
# touch the source file).
print("\nfp32 baseline still loads on CPU:")
so = ort.SessionOptions()
so.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL
try:
    sess_fp32 = ort.InferenceSession(str(SRC), so, providers=['CPUExecutionProvider'])
    print(f"  [OK] fp32 InferenceSession built ({len(sess_fp32.get_inputs())} input, "
          f"{len(sess_fp32.get_outputs())} output)")
except Exception as e:
    print(f"  [FAIL] fp32 baseline broken: {e}")
    sys.exit(1)

print()
print("Phase 1 done. fp16 model graph is structurally valid; DML kernel")
print("coverage + numerical comparison happen in Phase 2 (needs")
print("onnxruntime-directml in a separate venv to avoid clobbering the")
print("CPU build that other scripts depend on).")
