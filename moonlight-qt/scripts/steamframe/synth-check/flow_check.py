# -*- coding: utf-8 -*-
# 2026-10-10：補幀把「在動的東西」往哪個方向推、推了多少，對不對？
# 影像組只看得出合成的那一張和真的差在哪；這支用 --xr-dump-frame 多存的三個檔（<base>-prev8.png、-cur8.png、-flow.bin）核對：
#   A. shader 算出來的位移場，和這裡用 numpy 另外算的參考位移（上一張→這一張的 1/8 縮圖做區塊比對）方向、大小是否一致；
#      參考說沒動的地方 shader 有沒有說在動（配錯）。
#   B. 端到端：合成的那一張相對於真的那一張，在動的東西實際被移了多少（同一張存圖的上半對下半做區塊比對），
#      和「參考位移的一半、往前」比。方向相反或大小離譜＝補幀讓在動的東西每隔一格跳一下。
# 位移的定義跟 shader 一致：mv＝上一張的位置 − 這一張的位置（影像座標，x 往右、y 往下）。物體一格走 v＝−mv；
# 半格之後它在 p − 0.5·mv，所以合成那一張的 q 取自真的那一張的 q ＋ 0.5·mv：量到的 o（synth(q)≈real(q+o)）應該＝＋0.5·mv。
# 用法：python flow_check.py <base 路徑（不含 .png）> [--log <client log>] [--fv a,cxL,cxR,cyL,cyR] [--selftest]
import sys, os, re
import numpy as np
from PIL import Image


def lum(path):
    a = np.asarray(Image.open(path).convert('RGB')).astype(np.float32)
    return a[..., 0] * 0.299 + a[..., 1] * 0.587 + a[..., 2] * 0.114


def box(a, r):
    # (2r+1)x(2r+1) 的區塊總和（邊緣用鏡射補）
    p = np.pad(a, r, mode='edge')
    c = np.cumsum(np.cumsum(p, axis=0), axis=1)
    c = np.pad(c, ((1, 0), (1, 0)))
    k = 2 * r + 1
    return c[k:, k:] - c[:-k, k:] - c[k:, :-k] + c[:-k, :-k]


def shift_eye(img, dx, dy):
    # 回傳 out(p)＝img(p ＋ (dx,dy))；左右眼各自處理、不跨過中線；超出去的地方用邊緣的值
    h, w = img.shape
    hw = w // 2
    out = np.empty_like(img)
    ys = np.clip(np.arange(h) + dy, 0, h - 1)
    for x0 in (0, hw):
        xs = np.clip(np.arange(hw) + dx, 0, hw - 1) + x0
        out[:, x0:x0 + hw] = img[np.ix_(ys, xs)]
    return out


def match(cur, prev, rad, win, bias=0.02):
    # 每個像素找 d，使 cur(p) ≈ prev(p ＋ d)（d＝上一張的位置 − 這一張的位置）；回傳 d（次像素）、最小成本、不動的成本
    h, w = cur.shape
    best = None
    bd = np.zeros((h, w, 2), np.float32)
    costs = {}
    n = float((2 * win + 1) ** 2)
    for dy in range(-rad, rad + 1):
        for dx in range(-rad, rad + 1):
            c = box(np.abs(cur - shift_eye(prev, dx, dy)), win) / n + bias * np.hypot(dx, dy)
            costs[(dx, dy)] = c
            if best is None:
                best = c.copy()
                bd[..., 0] = dx
                bd[..., 1] = dy
            else:
                m = c < best
                best[m] = c[m]
                bd[m, 0] = dx
                bd[m, 1] = dy
    zero = costs[(0, 0)]
    # 次像素：最小值左右、上下各取一點做拋物線
    sub = np.zeros_like(bd)
    ix = bd[..., 0].astype(int)
    iy = bd[..., 1].astype(int)
    for dy in range(-rad + 1, rad):
        for dx in range(-rad + 1, rad):
            m = (ix == dx) & (iy == dy)
            if not m.any():
                continue
            c0 = costs[(dx, dy)][m]
            for ax, (a, b) in enumerate((((dx - 1, dy), (dx + 1, dy)), ((dx, dy - 1), (dx, dy + 1)))):
                cm = costs[a][m]
                cp = costs[b][m]
                den = cm - 2 * c0 + cp
                s = np.where(den > 1e-6, 0.5 * (cm - cp) / np.maximum(den, 1e-6), 0.0)
                sub[..., ax][m] = np.clip(s, -0.5, 0.5)
    return bd + sub, best, zero


def unwarp_axis(t_out, c, a):
    # 輸出（均勻）座標 → 解碼（壓縮過）座標，同 shader 的 vs_inv1；回傳 (解碼座標, 斜率 de/dt 的倒數＝a+3(1−a)e²)
    ext = np.where(t_out < c, c, 1.0 - c)
    t = (t_out - c) / ext
    if a >= 0.999:
        return t_out, np.ones_like(t_out)
    b = 1.0 - a
    e = t.copy()
    for _ in range(30):
        e = e - (b * e ** 3 + a * e - t) / (3 * b * e ** 2 + a)
    e = np.clip(e, -1.0, 1.0)
    return c + e * ext, a + 3 * b * e ** 2


def selftest():
    rng = np.random.RandomState(1)
    base = rng.rand(64, 128).astype(np.float32) * 255
    base = box(base, 1) / 9.0
    prev = base
    # 物體一格往右 3、往下 2：cur(p)＝prev(p − (3,2))，所以 mv＝上一張的位置 − 這一張的位置＝(−3, −2)
    cur = shift_eye(prev, -3, -2)
    d, best, zero = match(cur, prev, 6, 2)
    md = np.median(d[10:-10, 10:54].reshape(-1, 2), axis=0)
    assert abs(md[0] + 3) < 0.2 and abs(md[1] + 2) < 0.2, md
    # 合成：再往前半格＝往右 1.5、往下 1 → synth(q)＝cur(q − (1.5,1))≈cur(q ＋ 0.5·mv)。用整數測：往右 2、往下 1
    synth = shift_eye(cur, -2, -1)
    o, _, _ = match(synth, cur, 6, 2)
    mo = np.median(o[10:-10, 10:54].reshape(-1, 2), axis=0)
    assert abs(mo[0] + 2) < 0.2 and abs(mo[1] + 1) < 0.2, mo
    print('selftest ok: mv median', md, '(expect -3,-2); synth offset median', mo, '(expect -2,-1 = same sign as mv)')


def main():
    args = sys.argv[1:]
    if '--selftest' in args:
        selftest()
        return
    base = args[0]
    fv = None
    logp = None
    if '--fv' in args:
        fv = [float(x) for x in args[args.index('--fv') + 1].split(',')]
    if '--log' in args:
        logp = args[args.index('--log') + 1]
    fvs = {}
    if logp and os.path.exists(logp):
        for ln in open(logp, encoding='utf-8', errors='replace'):
            m = re.search(r'synth input dump: (\S+) .*fv=\(([-0-9.,]+)\)', ln)
            if m:
                fvs[os.path.basename(m.group(1))] = [float(x) for x in m.group(2).split(',')]
    k = 1
    tot = {'A_n': 0, 'A_cos': [], 'A_ratio': [], 'B_cos': [], 'B_ratio': [], 'false': [], 'missed': []}
    while True:
        suf = '' if k == 1 else '-%d' % k
        p2 = base + suf + '.png'
        pp = base + suf + '-prev8.png'
        pc = base + suf + '-cur8.png'
        pf = base + suf + '-flow.bin'
        if not os.path.exists(p2):
            break
        if not (os.path.exists(pp) and os.path.exists(pc) and os.path.exists(pf)):
            print('pair %d: no synth inputs saved (the frame had no synthesized image?)' % k)
            k += 1
            continue
        prev = lum(pp)
        cur = lum(pc)
        h, w = cur.shape
        raw = np.fromfile(pf, dtype=np.float16).astype(np.float32)
        assert raw.size == h * w * 4, (raw.size, h, w)
        fl = raw.reshape(h, w, 4)
        F = np.stack([fl[..., 0] * w, fl[..., 1] * h], axis=-1)  # shader 的位移，換成 1/8 縮圖的像素
        R, best, zero = match(cur, prev, 15, 2)
        # 參考位移可信：比不動明顯好（門檻跟 shader 同一個形式），而且這一區有紋理
        tex = box(np.abs(cur - box(cur, 2) / 25.0), 2) / 25.0
        sure = (zero - best > 0.4 * zero + 2.0) & (tex > 1.5)
        Rmag = np.hypot(R[..., 0], R[..., 1])
        Fmag = np.hypot(F[..., 0], F[..., 1])
        mov = sure & (Rmag > 1.0)
        still = (zero < 1.5) & (tex > 1.5)
        a_cos = a_ratio = float('nan')
        if mov.sum() >= 12:
            both = mov & (Fmag > 0.25)
            missed = 100.0 * (1.0 - both.sum() / float(mov.sum()))
            if both.sum() >= 6:
                cosv = (R[both] * F[both]).sum(axis=1) / (Rmag[both] * Fmag[both])
                a_cos = float(np.median(cosv))
                a_ratio = float(np.median(Fmag[both] / Rmag[both]))
                tot['A_cos'].append(a_cos)
                tot['A_ratio'].append(a_ratio)
            tot['missed'].append(missed)
        else:
            missed = float('nan')
        false = 100.0 * float((still & (Fmag > 0.5)).sum()) / max(1, int(still.sum()))
        # shader 說在動的像素佔多少（頭在動、景物不動時也應該只有手那一小塊：頭部轉動的位移已經先扣掉）
        shmov = 100.0 * float((Fmag > 0.5).mean())
        tot.setdefault('shmov', []).append(shmov)
        tot['false'].append(false)

        # B：存圖（1280 見方，上半真的、下半合成）縮一半再比
        im = Image.open(p2).convert('RGB')
        W, H = im.size
        im = im.resize((W // 2, H // 2), Image.BILINEAR)
        a = np.asarray(im).astype(np.float32)
        l2 = a[..., 0] * 0.299 + a[..., 1] * 0.587 + a[..., 2] * 0.114
        hh = l2.shape[0] // 2
        real = l2[:hh]
        synth = l2[hh:]
        oh, ow = real.shape
        f = fvs.get(os.path.basename(pf), fv) or [1.0, 0.5, 0.5, 0.5, 0.5]
        aa, cxl, cxr, cyl, cyr = f
        # 每個輸出像素對到解碼影像的哪裡、位移要乘多少（輸出＝解碼×斜率倒數）
        xo = (np.arange(ow) + 0.5) / ow
        yo = (np.arange(oh) + 0.5) / oh
        right = xo >= 0.5
        ex = np.where(right, xo - 0.5, xo) * 2.0
        dxL, sxL = unwarp_axis(ex, cxl, aa)
        dxR, sxR = unwarp_axis(ex, cxr, aa)
        dx = np.where(right, 0.5 + 0.5 * dxR, 0.5 * dxL)
        sx = np.where(right, sxR, sxL)
        dyL, syL = unwarp_axis(yo, cyl, aa)
        dyR, syR = unwarp_axis(yo, cyr, aa)
        gx = np.clip((dx * w).astype(int), 0, w - 1)
        exp = np.zeros((oh, ow, 2), np.float32)
        expSure = np.zeros((oh, ow), bool)
        for col in range(ow):
            dy_ = dyR if right[col] else dyL
            sy_ = syR if right[col] else syL
            gy = np.clip((dy_ * h).astype(int), 0, h - 1)
            # 解碼座標的位移（1/8 像素）→ UV → 輸出座標的像素；x 方向一隻眼佔一半寬，斜率是對單眼座標算的，比例一樣
            exp[:, col, 0] = 0.5 * R[gy, gx[col], 0] / w * sx[col] * ow
            exp[:, col, 1] = 0.5 * R[gy, gx[col], 1] / h * sy_ * oh
            expSure[:, col] = mov[gy, gx[col]]
        emag = np.hypot(exp[..., 0], exp[..., 1])
        rad = int(min(20, max(4, np.ceil(emag[expSure].max() + 3)))) if expSure.any() else 4
        O, obest, ozero = match(synth, real, rad, 3)
        omag = np.hypot(O[..., 0], O[..., 1])
        sel = expSure & (emag > 1.0) & (ozero - obest > 0.3 * ozero + 1.0)
        b_cos = b_ratio = float('nan')
        if sel.sum() >= 20:
            cosv = (O[sel] * exp[sel]).sum(axis=1) / np.maximum(omag[sel] * emag[sel], 1e-6)
            b_cos = float(np.median(cosv))
            b_ratio = float(np.median(omag[sel] / emag[sel]))
            tot['B_cos'].append(b_cos)
            tot['B_ratio'].append(b_ratio)
        print('pair %d: small %dx%d | shader moving %.2f%% of pixels | ref moving px %d (max %.1f px/frame at 1/8) | A shader-vs-ref cos %.2f ratio %.2f missed %.0f%% | false motion on still %.2f%% | '
              'B synth-vs-expected px %d cos %.2f ratio %.2f (expected max %.1f px at half dump scale)'
              % (k, w, h, shmov, int(mov.sum()), float(Rmag[mov].max()) if mov.any() else 0.0, a_cos, a_ratio, missed, false,
                 int(sel.sum()), b_cos, b_ratio, float(emag[expSure].max()) if expSure.any() else 0.0))
        k += 1

    def med(x):
        return float(np.median(x)) if x else float('nan')
    print('shader moving %% of pixels: median %.2f max %.2f' % (med(tot.get('shmov', [])), max(tot.get('shmov', [0]))))
    print('SUMMARY pairs=%d | A cos median %.2f ratio median %.2f missed median %.0f%% | false motion median %.2f%% max %.2f%% | B cos median %.2f min %.2f ratio median %.2f'
          % (k - 1, med(tot['A_cos']), med(tot['A_ratio']), med(tot['missed']), med(tot['false']), max(tot['false']) if tot['false'] else float('nan'),
             med(tot['B_cos']), min(tot['B_cos']) if tot['B_cos'] else float('nan'), med(tot['B_ratio'])))


main()
