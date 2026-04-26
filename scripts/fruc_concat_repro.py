#!/usr/bin/env python3
"""
Minimal reproducer for the fp16 Concat_12 DML kernel failure.

Build a tiny ONNX with just one 7-input fp16 Concat at the same shape
as the failing Concat_12 in fruc_fp16.onnx (all inputs at H=1152, W=1920,
channels 3/3/4/4/1/1/8 = 24 total channels).

Run on DML EP.  If it errors with the same 0x80070057 -> DML kernel limit
on fp16 7-input Concat, fix is graph surgery (split into binary cat tree).
If it works -> the fruc model has a different problem (interaction with
upstream Slice/GridSample/Tile shape inference).

Run from repo root:
    .venv-dml/Scripts/python.exe scripts/fruc_concat_repro.py
"""
import sys
import numpy as np
import onnx
from onnx import helper, TensorProto
import onnxruntime as ort


# Inputs: shape match fp16 fruc Concat_12 (channels 3/3/4/4/1/1/8 at HxW=1152x1920).
# We tested empirically that 540p_padded (640) ALSO fails, so use 640 to make repro fast.
H, W = 640, 960
INPUTS = [
    ("a", 3),
    ("b", 3),
    ("c", 4),
    ("d", 4),
    ("e", 1),
    ("f", 1),
    ("g", 8),
]


def make_model(num_inputs: int, dtype):
    """Build a tiny ONNX with `num_inputs` inputs concatenated along axis=1."""
    inputs = []
    input_specs = INPUTS[:num_inputs]
    for name, ch in input_specs:
        inputs.append(helper.make_tensor_value_info(name, dtype, [1, ch, H, W]))
    total_ch = sum(ch for _, ch in input_specs)
    output = helper.make_tensor_value_info("out", dtype, [1, total_ch, H, W])
    concat = helper.make_node(
        "Concat",
        inputs=[name for name, _ in input_specs],
        outputs=["out"],
        axis=1,
        name="ConcatRepro",
    )
    graph = helper.make_graph([concat], "concat_repro", inputs, [output])
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 16)])
    onnx.checker.check_model(model)
    return model


def run(model, dtype):
    """Save model + run on DML EP. Returns (ok, message)."""
    path = f"temp/concat_repro_{dtype.__name__}.onnx"
    onnx.save(model, path)
    so = ort.SessionOptions()
    so.log_severity_level = 3  # quiet
    try:
        sess = ort.InferenceSession(path, so,
                                    providers=['DmlExecutionProvider', 'CPUExecutionProvider'])
    except Exception as e:
        return False, f"InferenceSession build failed: {e}"
    inputs = {}
    rng = np.random.default_rng(42)
    for inp in sess.get_inputs():
        shape = [d if isinstance(d, int) else 1 for d in inp.shape]
        inputs[inp.name] = rng.uniform(0, 1, size=shape).astype(dtype)
    try:
        out = sess.run(None, inputs)[0]
        return True, f"OK shape={out.shape} provider={sess.get_providers()[0]}"
    except Exception as e:
        return False, f"Run failed: {type(e).__name__}: {str(e)[:200]}"


def main():
    print(f"ORT: {ort.__version__}")
    print(f"providers: {ort.get_available_providers()}")
    print(f"target shape: H={H} W={W}, total channels = {sum(ch for _,ch in INPUTS)}")
    print()

    # Sweep input counts 2..7 to find at which N DML errors out.
    for n in range(2, 8):
        for dtype in (np.float32, np.float16):
            ok, msg = run(make_model(n, TensorProto.FLOAT16 if dtype == np.float16 else TensorProto.FLOAT), dtype)
            verdict = "PASS" if ok else "FAIL"
            print(f"  n={n:>2}  {dtype.__name__:>8}  {verdict}  {msg}")
        print()


if __name__ == "__main__":
    sys.exit(main() or 0)
