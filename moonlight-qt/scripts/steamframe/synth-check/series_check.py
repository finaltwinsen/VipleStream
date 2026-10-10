# -*- coding: utf-8 -*-
# 2026-10-10：檢查 --xr-dump-frame 在補幀開著時存下的幾組影像（<base>.png、<base>-first.png、<base>-2.png、<base>-2-first.png…）。
# 每一組：第一次發布的那一張上半 vs 第二次發布的那一張上半（應該逐像素相同）；第二張的上半 vs 下半（合成，只該在動的東西附近不同）；
# 第一張的下半是不是直條紋（每一欄同一個顏色）。另外輸出一張縮圖總表（每組一列：第一張、第二張），最長邊不超過 2000。
# 用法：python series_check.py <base 路徑（不含 .png）>
import sys, os
import numpy as np
from PIL import Image

base = sys.argv[1]


def load(p):
    return np.asarray(Image.open(p).convert('RGB')).astype(np.float32)


def psnr(a, b):
    mse = float(((a - b) ** 2).mean())
    return 99.0 if mse == 0 else 10 * np.log10(255 * 255 / mse)


rows = []
k = 1
bad = 0
while True:
    suf = '' if k == 1 else '-%d' % k
    p2 = base + suf + '.png'
    p1 = base + suf + '-first.png'
    if not (os.path.exists(p1) and os.path.exists(p2)):
        break
    a1 = load(p1)
    a2 = load(p2)
    h = a1.shape[0] // 2
    w = a1.shape[1] // 2
    t1, b1 = a1[:h], a1[h:]
    t2, b2 = a2[:h], a2[h:]
    same = psnr(t1, t2)
    synth = psnr(t2, b2)
    frac = float((np.abs(t2 - b2).max(axis=2) > 24).mean()) * 100.0
    streak = float(b1.std(axis=0).mean())
    lr = psnr(t2[:, :w], t2[:, w:])
    ok = same >= 60.0 and synth >= 24.0
    bad += 0 if ok else 1
    print('pair %d: first.top==second.top PSNR %.1f dB | synth vs real PSNR %.1f dB, pixels differing %.2f%% | first.bottom column-std %.2f | L vs R %.1f dB  %s'
          % (k, same, synth, frac, streak, lr, 'OK' if ok else '<< CHECK'))
    rows.append((Image.open(p1).convert('RGB'), Image.open(p2).convert('RGB')))
    k += 1
if not rows:
    raise SystemExit('no pairs found for ' + base)
print('pairs: %d, flagged: %d' % (len(rows), bad))
# 縮圖總表：每組一列，左＝第一張（上半真的、下半不會被顯示），右＝第二張（上半真的、下半合成）
tw = 480
sheet = Image.new('RGB', (tw * 2 + 8, (tw + 8) * len(rows)), (40, 40, 40))
for i, (im1, im2) in enumerate(rows):
    sheet.paste(im1.resize((tw, tw), Image.BILINEAR), (0, i * (tw + 8)))
    sheet.paste(im2.resize((tw, tw), Image.BILINEAR), (tw + 8, i * (tw + 8)))
if max(sheet.size) > 2000:
    s = 2000.0 / max(sheet.size)
    sheet = sheet.resize((int(sheet.width * s), int(sheet.height * s)), Image.BILINEAR)
out = base + '_sheet.png'
sheet.save(out)
print('sheet:', out, sheet.size)
