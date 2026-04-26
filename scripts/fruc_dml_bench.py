#!/usr/bin/env python3
"""
VipleStream G.2 Phase 2: DirectML latency benchmark for fruc.onnx (fp32)
vs fruc_fp16.onnx, across stream resolutions.

Goal: validate whether fp16 fits the 30->60 doubling budget
(~16.7 ms per inference, ~14 ms threshold with 0.85 safety margin)
on the user's actual GPU before integrating into the C++ client.

Run from repo root with the DML venv:
    .venv-dml/Scripts/python.exe scripts/fruc_dml_bench.py

Outputs a per-(model, resolution) table with mean / p50 / p95 / p99 ms
plus pass/fail vs the 14 ms threshold.
"""

import sys
import time
from pathlib import Path

import numpy as np
import onnxruntime as ort

REPO  = Path(__file__).resolve().parents[1]
FP32  = REPO / "tools" / "fruc.onnx"
FP16  = REPO / "tools" / "fruc_fp16.onnx"
IFR_S = REPO / "tools" / "fruc_ifrnet_s.onnx"

# Budget: 30->60 doubling.  The interpolated frame must be ready before
# the half-rate display slot, so the inference budget is HALF the native
# frame interval -- 16.67 ms at 30 fps native.  0.85 safety margin gives
# the 14.2 ms threshold the C++ client uses (d3d11va.cpp 'halfRateMs *
# kBudgetMargin').  Note: Python ORT-DML inference is ~5-10x slower than
# the C++ DML pipeline (which has D3D11<->DML interop, pre-allocated
# tensors, multi-ring concat allocator).  So the absolute ms here are
# pessimistic; the *ratio* fp32/fp16 is what predicts production gain.
HALFRATE_MS  = (1000.0 / 30.0) / 2.0
THRESHOLD_MS = HALFRATE_MS * 0.85

# Stream resolutions to measure.  RIFE 4.25 requires H/W divisible by 128
# (block[0] uses scale=32 + two stride-2 convs).  Our newly-exported fp16
# model lacks the implicit Pad-to-128 wrapper of the older fruc.onnx, so
# we feed already-padded sizes.  In production the C++ side pads externally.
#   1080p -> 1152 (pad +72 vertical)
#    720p ->  768 (pad +48)
#    540p ->  640 (pad +100)
RESOLUTIONS = [
    # Production shapes (the wrapper pads/crops to 128-multiple internally
    # since v1.2.127, so feeding 1080 / 720 / 540 directly works -- the
    # earlier "_padded" variants were workarounds for the missing wrapper).
    ("1080p", 1920, 1080),
    ("720p",  1280,  720),
    ("540p",   960,  540),
]

# Each measurement: WARMUP runs to settle DML graph compile + tensor
# allocation, then RUNS timed runs.  RIFE has cold-start cost on first
# inference (DML kernel JIT); WARMUP eats that.
WARMUP = 3
RUNS   = 30


def bench(model_path: Path, label: str, w: int, h: int) -> dict:
    """Time RUNS inferences on the DML EP at the given resolution."""
    if not model_path.exists():
        return {"error": f"model not found: {model_path}"}

    so = ort.SessionOptions()
    so.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL
    # Suppress the verbose info-level logs from DML EP build.
    so.log_severity_level = 3

    try:
        sess = ort.InferenceSession(
            str(model_path), so,
            providers=['DmlExecutionProvider', 'CPUExecutionProvider'],
        )
    except Exception as e:
        return {"error": f"InferenceSession build failed: {e}"}

    # Note which EP each node landed on (any unmapped → CPU = bad sign).
    provider = sess.get_providers()[0]

    input_meta  = sess.get_inputs()[0]
    input_name  = input_meta.name
    output_name = sess.get_outputs()[0].name

    # Match the model's input dtype.  PyTorch native fp16 export keeps
    # I/O tensors as fp16; the older fp32 model has fp32 I/O.  Either
    # way, feed random uniform[0,1] for [B, 7, H, W].
    rng = np.random.default_rng(42)
    np_dtype = np.float16 if 'float16' in input_meta.type else np.float32
    inp = rng.uniform(0, 1, size=(1, 7, h, w)).astype(np_dtype)

    # Warmup
    for _ in range(WARMUP):
        sess.run([output_name], {input_name: inp})

    # Timed runs
    samples = []
    for _ in range(RUNS):
        t0 = time.perf_counter()
        out = sess.run([output_name], {input_name: inp})[0]
        samples.append((time.perf_counter() - t0) * 1000.0)

    samples_arr = np.array(samples)
    return {
        "label":    label,
        "provider": provider,
        "shape":    out.shape,
        "dtype":    str(out.dtype),
        "mean":     float(samples_arr.mean()),
        "p50":      float(np.percentile(samples_arr, 50)),
        "p95":      float(np.percentile(samples_arr, 95)),
        "p99":      float(np.percentile(samples_arr, 99)),
        "min":      float(samples_arr.min()),
        "max":      float(samples_arr.max()),
        "samples":  samples,
    }


def fmt_ms(x: float) -> str:
    """Format ms with verdict colour: '!' = over threshold, '*' = at risk."""
    if x > THRESHOLD_MS:
        return f"{x:>7.2f} !"
    if x > THRESHOLD_MS * 0.85:
        return f"{x:>7.2f} *"
    return f"{x:>7.2f}  "


def main() -> int:
    print(f"GPU: {ort.get_available_providers()}")
    print(f"ORT: {ort.__version__}")
    print(f"Half-rate slot (30->60):     {HALFRATE_MS:.2f} ms")
    print(f"Threshold (0.85x):            {THRESHOLD_MS:.2f} ms")
    print(f"Warmup={WARMUP}, timed runs={RUNS}")
    print()
    print("NOTE: Python ORT-DML is ~5-10x slower than the C++ DML pipeline.")
    print("Absolute ms here are pessimistic; what matters is the fp32/fp16")
    print("RATIO -- if fp16 is 1.5x-2x faster, that ratio carries to C++.")
    print()

    results = []
    for label, w, h in RESOLUTIONS:
        for model_label, model_path in [("fp32", FP32), ("fp16", FP16), ("ifr_s", IFR_S)]:
            tag = f"{model_label} {label}"
            print(f"  benching {tag} ({w}x{h})…", flush=True, end=" ")
            r = bench(model_path, tag, w, h)
            if "error" in r:
                print(f"FAILED: {r['error']}")
                results.append({"label": tag, "error": r["error"]})
                continue
            print(f"mean {r['mean']:.1f} ms (provider {r['provider']})")
            results.append(r)

    # Summary table
    print()
    print(f"{'config':>14} {'provider':>22} {'mean':>10} {'p50':>10} {'p95':>10} {'p99':>10}    verdict")
    print(f"{'-'*14} {'-'*22} {'-'*10} {'-'*10} {'-'*10} {'-'*10}    {'-'*15}")
    for r in results:
        if "error" in r:
            print(f"{r['label']:>14}  ERROR: {r['error']}")
            continue
        verdict = ""
        if r['p95'] <= THRESHOLD_MS:
            verdict = "PASS"
        elif r['p50'] <= THRESHOLD_MS:
            verdict = "MARGINAL"
        else:
            verdict = "FAIL"
        print(f"{r['label']:>14} {r['provider']:>22}"
              f" {fmt_ms(r['mean'])} {fmt_ms(r['p50'])}"
              f" {fmt_ms(r['p95'])} {fmt_ms(r['p99'])}    {verdict}")

    print()
    print("Verdict legend:")
    print(f"  PASS     — p95 <= {THRESHOLD_MS:.1f} ms threshold (safe to ship)")
    print(f"  MARGINAL — median fits but p95 spikes; risky on busy GPU")
    print(f"  FAIL     — median over budget; will drop frames")
    print()
    print("'!' suffix = over threshold,  '*' = within 15% of threshold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
