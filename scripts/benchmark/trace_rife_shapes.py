"""Trace RIFE-v4.25-lite shapes through flownet.param at given input dim.

Used to diagnose Add_503 shape mismatch when Path β.4 inferDim > 256×128.

Usage:  python trace_rife_shapes.py [inferW] [inferH]
"""
import sys
from pathlib import Path

PARAM = Path(r'D:\Mission\VipleStream\moonlight-qt\app\rife_models\rife-v4.25-lite\flownet.param')

inferW = int(sys.argv[1]) if len(sys.argv) > 1 else 512
inferH = int(sys.argv[2]) if len(sys.argv) > 2 else 288

shapes = {}  # name → (c, h, w)

def parse_params(toks):
    """Parse k=v / -23309=array params from layer toks."""
    params = {}
    arrays = {}
    for tok in toks:
        if '=' not in tok:
            continue
        k_raw, v = tok.split('=', 1)
        k = int(k_raw)
        if k_raw.startswith('-'):
            # array form: k=-23309, v=N,v0,v1,...
            parts = v.split(',')
            params[k] = [int(x) if '.' not in x else float(x) for x in parts]
        else:
            try: params[k] = int(v)
            except ValueError: params[k] = float(v)
    return params

def kernel_extent(k, d):
    return d * (k - 1) + 1

with PARAM.open() as f:
    lines = f.readlines()

# Skip magic + counts
# line 1: 7767517
# line 2: <layer_count> <blob_count>
# line 3+: layers

failed = False

for lineno, line in enumerate(lines[2:], start=3):
    toks = line.split()
    if len(toks) < 4:
        continue
    op_type = toks[0]
    name    = toks[1]
    n_in    = int(toks[2])
    n_out   = int(toks[3])
    inputs  = toks[4:4 + n_in]
    outputs = toks[4 + n_in : 4 + n_in + n_out]
    p_toks  = toks[4 + n_in + n_out:]
    p = parse_params(p_toks)

    if op_type == 'Input':
        # in0 / in1 / in2: shape from runtime, here we use known
        if name == 'in0' or name == 'in1':
            shapes[outputs[0]] = (3, inferH, inferW)
        elif name == 'in2':
            shapes[outputs[0]] = (1, 1, 1)
        else:
            shapes[outputs[0]] = (3, inferH, inferW)
        continue

    if op_type == 'MemoryData':
        # 0=w, 1=h, 2=c
        w = p.get(0, 1)
        h = p.get(1, 1)
        c = p.get(2, 1)
        shapes[outputs[0]] = (c, h, w)
        continue

    if op_type == 'Split':
        # Replicate input to all outputs
        if inputs[0] not in shapes:
            print(f'ERR line {lineno} {name}: input {inputs[0]} not in shapes')
            failed = True; break
        s = shapes[inputs[0]]
        for o in outputs:
            shapes[o] = s
        continue

    # Default: get input 0 shape
    if inputs[0] not in shapes:
        print(f'ERR line {lineno} {name}: input {inputs[0]} not in shapes')
        failed = True; break
    inS = shapes[inputs[0]]
    inC, inH, inW = inS

    if op_type == 'Convolution':
        outC = p.get(0, 0)
        kW = p.get(1, 0); kH = p.get(11, kW)
        dW = p.get(2, 1); dH = p.get(12, dW)
        sW = p.get(3, 1); sH = p.get(13, sW)
        padL = p.get(4, 0); padR = p.get(14, padL)
        padT = p.get(15, padL); padB = p.get(16, padT)
        kExtH = kernel_extent(kH, dH); kExtW = kernel_extent(kW, dW)
        outH = (inH + padT + padB - kExtH) // sH + 1
        outW = (inW + padL + padR - kExtW) // sW + 1
        shapes[outputs[0]] = (outC, outH, outW)

    elif op_type == 'Deconvolution':
        outC = p.get(0, 0)
        kW = p.get(1, 0); kH = p.get(11, kW)
        dW = p.get(2, 1); dH = p.get(12, dW)
        sW = p.get(3, 1); sH = p.get(13, sW)
        padL = p.get(4, 0); padR = p.get(14, padL)
        padT = p.get(15, padL); padB = p.get(16, padT)
        outPadR = p.get(18, 0); outPadB = p.get(19, 0)
        absH = p.get(21, 0); absW = p.get(20, 0)
        kExtH = kernel_extent(kH, dH); kExtW = kernel_extent(kW, dW)
        outH = (inH - 1) * sH + kExtH - padT - padB + outPadB
        outW = (inW - 1) * sW + kExtW - padL - padR + outPadR
        if absH > 0: outH = absH
        if absW > 0: outW = absW
        shapes[outputs[0]] = (outC, outH, outW)

    elif op_type == 'PixelShuffle':
        r = p.get(0, 1)
        if inC % (r * r) != 0:
            print(f'ERR line {lineno} {name}: PixelShuffle inC={inC} not div by r*r={r*r}')
            failed = True; break
        shapes[outputs[0]] = (inC // (r * r), inH * r, inW * r)

    elif op_type == 'Interp':
        hScale = p.get(1, 1.0); wScale = p.get(2, 1.0)
        outH = p.get(3, 0); outW = p.get(4, 0)
        if outH == 0: outH = int(inH * hScale + 0.5)
        if outW == 0: outW = int(inW * wScale + 0.5)
        if outH <= 0: outH = inH
        if outW <= 0: outW = inW
        shapes[outputs[0]] = (inC, outH, outW)

    elif op_type == 'Crop':
        # -23311=axes (1, 0=channel default)
        # -23309=starts, -23310=ends
        starts = p.get(-23309, [1, 0])
        ends = p.get(-23310, [1, 0])
        s = starts[1] if len(starts) > 1 else 0
        e = ends[1] if len(ends) > 1 else 0
        if e == 2147483647: e = inC
        outC = e - s
        shapes[outputs[0]] = (outC, inH, inW)

    elif op_type in ('BinaryOp', 'ReLU', 'Sigmoid', 'Eltwise'):
        # Same shape as input 0
        shapes[outputs[0]] = inS

    elif op_type == 'Concat':
        outC = 0
        outH_c, outW_c = 0, 0
        for i, inp in enumerate(inputs):
            iS = shapes.get(inp)
            if iS is None:
                print(f'ERR line {lineno} {name}: Concat input {inp} not in shapes')
                failed = True; break
            outC += iS[0]
            if i == 0: outH_c, outW_c = iS[1], iS[2]
        shapes[outputs[0]] = (outC, outH_c, outW_c)

    elif op_type == 'rife.Warp':
        # Output = same shape as input 0 (image)
        shapes[outputs[0]] = inS

    else:
        print(f'WARN line {lineno} {name}: unknown op {op_type}')
        shapes[outputs[0]] = inS

    # Track every layer's full record for debugging.
    pass

# After full trace, print the chain for both Add_503 inputs.
# Build name → (line, op, inputs, outputs, p, shape).
records = []
for lineno, line in enumerate(lines[2:], start=3):
    toks = line.split()
    if len(toks) < 4: continue
    op_type = toks[0]; nm = toks[1]
    n_in = int(toks[2]); n_out = int(toks[3])
    inps = toks[4:4 + n_in]
    outs = toks[4 + n_in : 4 + n_in + n_out]
    records.append((lineno, op_type, nm, inps, outs))

# For Add_503's two inputs, walk back through the producer chain and
# print each layer with its computed shape.
def find_producer(tensor_name):
    for r in records:
        if tensor_name in r[4]:
            return r
    return None

def trace_back(start_tensor, depth=15):
    print(f'\n--- chain producing {start_tensor} (last {depth} steps) ---')
    chain = []
    cur = start_tensor
    for _ in range(depth):
        prod = find_producer(cur)
        if prod is None:
            chain.append(('?', cur, '(input or unresolved)'))
            break
        chain.append(prod)
        # Walk via input 0 (primary path)
        if len(prod[3]) == 0: break
        cur = prod[3][0]
    chain.reverse()
    for r in chain:
        if isinstance(r, tuple) and len(r) >= 5:
            lineno, op, nm, inps, outs = r
            shp = shapes.get(outs[0]) if outs else None
            print(f'  L{lineno:4d} {op:18s} {nm:24s}  inputs={inps} → outputs[0]={outs[0]} shape={shp}')

# Find Add_503 inputs and trace back.
for r in records:
    if r[2] == 'Add_503':
        a_in, b_in = r[3]
        a_shape = shapes.get(a_in); b_shape = shapes.get(b_in)
        print(f'\n=== Add_503 ===  a={a_in}{a_shape}  b={b_in}{b_shape}')
        trace_back(a_in, depth=20)
        trace_back(b_in, depth=20)
        break

# Print ALL stride-2 convs and their input/output H to understand
# how the encoder downsamples.
print('\n=== Encoder stride-2 layers ===')
for r in records:
    if r[1] == 'Convolution':
        # Find param 3 (sH=1)... this requires re-parsing.  Walk the file.
        pass

# Print first 70 layers compactly to see encoder pattern.
print('\n=== First 70 layers compact ===')
count = 0
for r in records:
    lineno, op, nm, inps, outs = r
    if outs and outs[0] in shapes:
        s = shapes[outs[0]]
        if op in ('Convolution', 'Deconvolution', 'PixelShuffle', 'Interp', 'Crop'):
            print(f'  L{lineno:4d} {op:14s} {nm:24s} → {outs[0]:8s} shape={s}')
            count += 1
            if count >= 50: break

if failed:
    sys.exit(1)

print(f'\nTrace complete at inferDim={inferW}×{inferH}')
print(f'Total tensors with shape: {len(shapes)}')
