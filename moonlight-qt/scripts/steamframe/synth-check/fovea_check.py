# -*- coding: utf-8 -*-
# 2026-10-10：比對「注視點開」與「注視點關」兩場靜止畫面的存圖（補幀開著時的第二張：上半是真的那一張），量每隻眼在不同高度
# 的垂直／水平位移（像素，存圖的尺度；每眼高＝存圖高度的一半）。還原正確時兩眼都應該接近 0。
# 用法：python fovea_check.py <注視點開的 png> <注視點關的 png>
import sys
import numpy as np
from PIL import Image


def load(p):
    return np.asarray(Image.open(p).convert('RGB')).astype(np.float32)


def gray(a):
    return a[..., 0] * 0.299 + a[..., 1] * 0.587 + a[..., 2] * 0.114


def shift_1d(a, b, maxs=6):
    costs = []
    for s in range(-maxs, maxs + 1):
        if s >= 0:
            d = a[s:, :] - b[:a.shape[0] - s, :]
        else:
            d = a[:s, :] - b[-s:, :]
        costs.append((d ** 2).mean())
    costs = np.array(costs)
    i = int(costs.argmin())
    sub = 0.0
    if 0 < i < len(costs) - 1:
        den = costs[i - 1] - 2 * costs[i] + costs[i + 1]
        if den > 1e-9:
            sub = 0.5 * (costs[i - 1] - costs[i + 1]) / den
    return (i - maxs) + sub, costs[i]


f1 = load(sys.argv[1])
f0 = load(sys.argv[2])
H = f1.shape[0] // 2
W = f1.shape[1] // 2
g1 = gray(f1[:H])
g0 = gray(f0[:H])
print('eye height in the dump: %d px; 1 px = %.3f%% of the eye height' % (H, 100.0 / H))
worst = {}
for eye, name in ((0, 'L'), (1, 'R')):
    x0 = eye * W + int(W * 0.20)
    x1 = eye * W + int(W * 0.80)
    out = []
    for yc in (0.10, 0.20, 0.30, 0.40, 0.50, 0.60, 0.70, 0.80, 0.90):
        y0 = max(8, int(H * yc) - 40)
        y1 = min(H - 8, int(H * yc) + 40)
        sy, _ = shift_1d(g1[y0:y1, x0:x1], g0[y0:y1, x0:x1])
        sx, _ = shift_1d(g1[y0:y1, x0:x1].T, g0[y0:y1, x0:x1].T)
        out.append((yc, sy, sx))
    worst[name] = max(abs(o[1]) for o in out)
    print('eye %s: ' % name + '  '.join('y=%.1f dy=%+.2f dx=%+.2f' % o for o in out))
print('max |dy|: L %.2f px, R %.2f px' % (worst['L'], worst['R']))
