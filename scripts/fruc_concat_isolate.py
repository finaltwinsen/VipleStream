#!/usr/bin/env python3
"""
Isolate which of the 7 Concat_12 inputs fails on DML EP.

The standalone Concat_12 reproducer passed -- all 7-input fp16 Concats
work on DML.  So the failure is upstream interaction, not Concat itself.

This script extracts a sub-model whose outputs are each of the 7 inputs
to Concat_12.  Running that sub-model tells us:
  - Which inputs make it through DML EP without error
  - Which input is the "first failure" -- that's our culprit

Run from repo root:
    .venv-dml/Scripts/python.exe scripts/fruc_concat_isolate.py
"""
import sys
import numpy as np
import onnx
from onnx import utils as onnx_utils
import onnxruntime as ort

SRC = "tools/fruc_fp16.onnx"
H, W = 640, 960  # 540p_padded -- known to fail in repro

# Concat_12 inputs (from earlier diagnosis)
CONCAT_INPUTS = [
    "/flownet/Slice_10_output_0",       # warped_img0[:, :3]
    "/flownet/Slice_11_output_0",       # warped_img1[:, :3]
    "/flownet/GridSample_2_output_0",   # wf0
    "/flownet/GridSample_3_output_0",   # wf1
    "/flownet/Tile_output_0",           # timestep plane
    "/flownet/block0/Slice_1_output_0", # mask from block0
    "/flownet/block0/Slice_2_output_0", # feat from block0
]


def extract_subgraph(target_output_name: str) -> str:
    """Use onnx.utils.extract_model to create a sub-model with this single output."""
    dst = f"temp/sub_{target_output_name.replace('/', '_').replace(':', '_')}.onnx"
    try:
        onnx_utils.extract_model(SRC, dst, ["input"], [target_output_name])
        return dst
    except Exception as e:
        return f"EXTRACT_FAILED: {e}"


def run_subgraph(path: str, label: str):
    """Run a sub-model on DML, report success / failure / shape."""
    if path.startswith("EXTRACT_FAILED"):
        return f"  {label:<48} {path[:80]}"
    so = ort.SessionOptions()
    so.log_severity_level = 3
    try:
        sess = ort.InferenceSession(path, so,
                                    providers=['DmlExecutionProvider', 'CPUExecutionProvider'])
    except Exception as e:
        return f"  {label:<48} INIT_FAIL  {type(e).__name__}: {str(e)[:120]}"
    inp_meta = sess.get_inputs()[0]
    np_dtype = np.float16 if 'float16' in inp_meta.type else np.float32
    rng = np.random.default_rng(42)
    inp = rng.uniform(0, 1, size=(1, 7, H, W)).astype(np_dtype)
    try:
        out = sess.run(None, {inp_meta.name: inp})[0]
        return f"  {label:<48} PASS  shape={out.shape} dtype={out.dtype}"
    except Exception as e:
        return f"  {label:<48} RUN_FAIL  {type(e).__name__}: {str(e)[:120]}"


def main():
    print(f"ORT: {ort.__version__}")
    print(f"Target shape: H={H} W={W}")
    print()
    print("Per-input sub-graph tests (each row = pipe up to that tensor, run, see if it errors):")
    print()

    for tensor_name in CONCAT_INPUTS:
        path = extract_subgraph(tensor_name)
        print(run_subgraph(path, tensor_name))

    print()
    print("Also test the Concat output itself:")
    path = extract_subgraph("/flownet/Concat_12_output_0")
    print(run_subgraph(path, "/flownet/Concat_12_output_0  (target)"))


if __name__ == "__main__":
    sys.exit(main() or 0)
