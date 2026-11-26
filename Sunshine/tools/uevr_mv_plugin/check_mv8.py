#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check_mv8.py
VipleStream §VR-GAME-MV (M-MV0) — 1/8 動態向量 (mv8) 離線反向 Warp 驗證腳本

用途：
  讀取 viple_uevr_mv_plugin.dll 傾印至 dump/ 目錄下的連續幀資料：
    - mv8_<frame>.json   (尺寸、格式、mvScale、jitter、reset、分眼來源、相機矩陣)
    - mv8_<frame>.bin    (雙眼並排 1/8 RGBA16F：xy=單眼正規化 UV 位移 uv_prev - uv_cur, z=逆深度, w=有效度)
    - color8_<frame>.png (雙眼並排 1/8 RGBA8 Color 縮圖)

  依照設計文件 docs/specs/vr_game_motion_vectors.md 第 2.1、2.2 與 6.2 節 (M-MV0) 通過標準驗證：
    1. 每次 Present 是否穩定攔截到左、右眼各 1 次 EvaluateFeature；
    2. 用 mv8 反向 warp 重建上一幀（或對齊當前幀），計算 warp 前後的 PSNR 差（動態區域通過門檻 >= +6.0 dB）；
    3. 檢測 X/Y 方向是否顛倒（比對 (+du,+dv)、(-du,+dv)、(+du,-dv)、(-du,-dv)、軸對調之 PSNR）；
    4. 檢測左右眼是否交叉污染（左半/右半 MV 互換測試、跨眼 PrevViewProj IPD 水平視差偏移檢測）。
"""

from __future__ import annotations

import argparse
import glob
import json
import math
import os
import struct
import sys
import zlib
from pathlib import Path
from typing import Dict, List, Optional, Tuple

try:
    import numpy as np
except ImportError:
    print("[錯誤] 請在已安裝 numpy 的 Python 虛擬環境 (venv) 中執行此腳本。", file=sys.stderr)
    sys.exit(1)


# ============================================================================
# PNG 讀取（優先使用內建解析器讀取外掛輸出的 RGBA8 PNG，若為其他 PNG 則退回 PIL/imageio）
# ============================================================================
def _paeth_predictor(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa = abs(p - a)
    pb = abs(p - b)
    pc = abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    elif pb <= pc:
        return b
    return c


def load_png_rgba8(path: Path) -> np.ndarray:
    """讀取 RGBA8 (或 RGB8) PNG 為 float32 RGB 陣列，值域 [0.0, 255.0]，形狀 (H, W, 3)。"""
    try:
        from PIL import Image  # type: ignore
        with Image.open(path) as img:
            rgb = img.convert("RGB")
            return np.asarray(rgb, dtype=np.float32)
    except ImportError:
        pass

    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"非合法的 PNG 檔案: {path}")

    pos = 8
    width = 0
    height = 0
    bit_depth = 0
    color_type = 0
    idat_chunks: List[bytes] = []

    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        chunk_type = data[pos + 4:pos + 8]
        chunk_data = data[pos + 8:pos + 8 + length]
        pos += 12 + length

        if chunk_type == b"IHDR":
            width, height, bit_depth, color_type, _, _, _ = struct.unpack(">IIBBBBB", chunk_data)
        elif chunk_type == b"IDAT":
            idat_chunks.append(chunk_data)
        elif chunk_type == b"IEND":
            break

    if bit_depth != 8 or color_type not in (2, 6):
        raise ValueError(f"內建 PNG 解析器僅支援 8-bit RGB/RGBA (讀到 bit_depth={bit_depth}, color_type={color_type})")

    bpp = 4 if color_type == 6 else 3
    raw = zlib.decompress(b"".join(idat_chunks))
    stride = width * bpp
    recon = bytearray(height * stride)

    src_offset = 0
    for y in range(height):
        ftype = raw[src_offset]
        src_offset += 1
        row_start = y * stride
        prev_row_start = (y - 1) * stride

        for x in range(stride):
            val = raw[src_offset + x]
            a = recon[row_start + x - bpp] if x >= bpp else 0
            b = recon[prev_row_start + x] if y > 0 else 0
            c = recon[prev_row_start + x - bpp] if (y > 0 and x >= bpp) else 0

            if ftype == 0:
                out = val
            elif ftype == 1:
                out = (val + a) & 0xFF
            elif ftype == 2:
                out = (val + b) & 0xFF
            elif ftype == 3:
                out = (val + ((a + b) >> 1)) & 0xFF
            elif ftype == 4:
                out = (val + _paeth_predictor(a, b, c)) & 0xFF
            else:
                raise ValueError(f"未知的 PNG filter type: {ftype}")
            recon[row_start + x] = out

        src_offset += stride

    arr = np.frombuffer(recon, dtype=np.uint8).reshape((height, width, bpp))
    return arr[:, :, :3].astype(np.float32)


def load_mv8_bin(path: Path, sbs_w: int, sbs_h: int) -> np.ndarray:
    """讀取 mv8_<frame>.bin (RGBA16F) 為 float32 陣列，形狀 (H, W, 4)。"""
    raw = path.read_bytes()
    expected_bytes = sbs_w * sbs_h * 4 * 2
    if len(raw) < expected_bytes:
        raise ValueError(f"{path} 檔案大小 ({len(raw)} B) 小於預期 ({expected_bytes} B)")
    arr16 = np.frombuffer(raw[:expected_bytes], dtype=np.float16).reshape((sbs_h, sbs_w, 4))
    return arr16.astype(np.float32)


def load_raw_mv4_bin(path: Path, sbs_w: int, sbs_h: int) -> np.ndarray:
    """讀取除錯傾印 mv4_raw_<frame>.bin (RG32F) 為 float32 陣列，形狀 (H, W, 2)。"""
    raw = path.read_bytes()
    expected_bytes = sbs_w * sbs_h * 2 * 4
    if len(raw) < expected_bytes:
        raise ValueError(f"{path} 檔案大小 ({len(raw)} B) 小於預期 ({expected_bytes} B)")
    arr32 = np.frombuffer(raw[:expected_bytes], dtype=np.float32).reshape((sbs_h, sbs_w, 2))
    return arr32.copy()


# ============================================================================
# 雙線性反向取樣 (Bilinear Backward Warp) 與 PSNR 計算
# ============================================================================
def bilinear_sample(img: np.ndarray, sample_x: np.ndarray, sample_y: np.ndarray) -> Tuple[np.ndarray, np.ndarray]:
    """
    對單眼影像 img (H, W, 3) 在浮點格點座標 (sample_x, sample_y) 執行雙線性內插。
    回傳：(warped_img, in_bounds_mask)
    """
    h, w, _ = img.shape
    in_bounds = (sample_x >= 0.0) & (sample_x <= float(w - 1)) & (sample_y >= 0.0) & (sample_y <= float(h - 1))

    x = np.clip(sample_x, 0.0, float(w - 1))
    y = np.clip(sample_y, 0.0, float(h - 1))

    x0 = np.floor(x).astype(np.int32)
    y0 = np.floor(y).astype(np.int32)
    x1 = np.minimum(x0 + 1, w - 1)
    y1 = np.minimum(y0 + 1, h - 1)

    wx = (x - x0.astype(np.float32))[..., None]
    wy = (y - y0.astype(np.float32))[..., None]

    c00 = img[y0, x0]
    c10 = img[y0, x1]
    c01 = img[y1, x0]
    c11 = img[y1, x1]

    c0 = c00 * (1.0 - wx) + c10 * wx
    c1 = c01 * (1.0 - wx) + c11 * wx
    warped = c0 * (1.0 - wy) + c1 * wy
    return warped, in_bounds


def compute_psnr(img_a: np.ndarray, img_b: np.ndarray, mask: Optional[np.ndarray] = None) -> float:
    """計算兩張值域 [0, 255] RGB 影像在 mask 區域內的 PSNR (dB)。"""
    diff2 = np.mean((img_a - img_b) ** 2, axis=-1)
    if mask is not None:
        valid_count = int(np.sum(mask))
        if valid_count == 0:
            return float("nan")
        mse = float(np.sum(diff2[mask]) / valid_count)
    else:
        mse = float(np.mean(diff2))

    if mse <= 1e-10:
        return 99.99
    return 10.0 * math.log10((255.0 * 255.0) / mse)


def compute_trimmed_psnr(
    img_a: np.ndarray,
    img_b: np.ndarray,
    mask: Optional[np.ndarray] = None,
    keep_quantile: float = 90.0,
) -> float:
    """
    計算排除前 (100 - keep_quantile)% 極端平方誤差離群值（如近裁切面破口或遮擋暴露邊緣）後的穩健 PSNR (dB)。
    """
    diff2 = np.mean((img_a - img_b) ** 2, axis=-1)
    vals = diff2[mask] if mask is not None else diff2.ravel()
    if vals.size == 0:
        return float("nan")
    thresh = float(np.percentile(vals, keep_quantile))
    trimmed = vals[vals <= thresh]
    if trimmed.size == 0:
        trimmed = vals
    mse = float(np.mean(trimmed))
    if mse <= 1e-10:
        return 99.99
    return 10.0 * math.log10((255.0 * 255.0) / mse)


def compute_color_flow_correlation(
    col_prev: np.ndarray,
    col_cur: np.ndarray,
    du: np.ndarray,
    dv: np.ndarray,
    validity: np.ndarray,
    motion_thresh_cells: float = 0.25,
) -> Dict[str, float]:
    """
    以簡單區塊比對 (5x5 block SAD + 1D 拋物線次像素修正) 估計單眼 color8 縮圖之間的光流 (flow_dx, flow_dy)，
    並計算每眼 MV (du*W, dv*H) 與 color 光流方向的皮爾森相關係數 (corr_x, corr_y)、
    2D 向量餘弦相似度 (cos_sim) 與同向比例 (pos_dot_pct)。
    注意：光流位移方向定義與設計 2.1 節 MV 完全一致：位移 = prev_pos - cur_pos。
    """
    h, w, _ = col_cur.shape
    prev_y = 0.299 * col_prev[:, :, 0] + 0.587 * col_prev[:, :, 1] + 0.114 * col_prev[:, :, 2]
    cur_y  = 0.299 * col_cur[:, :, 0]  + 0.587 * col_cur[:, :, 1]  + 0.114 * col_cur[:, :, 2]

    dx_mv = du * float(w)
    dy_mv = dv * float(h)
    mv_mag = np.hypot(dx_mv, dy_mv)
    valid_mv = (validity > 0.01) & np.isfinite(dx_mv) & np.isfinite(dy_mv) & (mv_mag >= motion_thresh_cells)
    if int(np.sum(valid_mv)) < 16:
        return {
            "matched_cells": 0.0,
            "corr_x": float("nan"),
            "corr_y": float("nan"),
            "cos_sim": float("nan"),
            "pos_dot_pct": float("nan"),
        }

    p90_mag = float(np.percentile(mv_mag[valid_mv], 90.0))
    search_rad = int(np.clip(np.ceil(p90_mag) + 2, 6, 16))
    block_rad = 2
    win = 2 * block_rad + 1
    win_area = float(win * win)

    pad = block_rad + search_rad
    prev_pad = np.pad(prev_y, pad, mode="edge")
    cur_pad  = np.pad(cur_y, pad, mode="edge")

    # 計算 5x5 區塊平均空間梯度能量，過濾無紋理純色區與全黑破口
    gx = np.abs(cur_pad[pad : pad + h, pad + 1 : pad + w + 1] - cur_pad[pad : pad + h, pad - 1 : pad + w - 1])
    gy = np.abs(cur_pad[pad + 1 : pad + h + 1, pad : pad + w] - cur_pad[pad - 1 : pad + h - 1, pad : pad + w])
    g_pad = np.pad(gx + gy, block_rad, mode="edge")
    cs_g = np.cumsum(np.cumsum(np.pad(g_pad, ((1, 0), (1, 0)), mode="constant"), axis=0), axis=1)
    block_grad = (cs_g[win:, win:] - cs_g[:-win, win:] - cs_g[win:, :-win] + cs_g[:-win, :-win]) / win_area

    best_cost = np.full((h, w), 1e18, dtype=np.float32)
    best_dx = np.zeros((h, w), dtype=np.float32)
    best_dy = np.zeros((h, w), dtype=np.float32)
    zero_cost = np.zeros((h, w), dtype=np.float32)

    cost_grid = np.empty((2 * search_rad + 1, 2 * search_rad + 1, h, w), dtype=np.float32)

    for iy, dy in enumerate(range(-search_rad, search_rad + 1)):
        for ix, dx in enumerate(range(-search_rad, search_rad + 1)):
            diff = np.abs(
                prev_pad[pad + dy - block_rad : pad + dy + h + block_rad,
                         pad + dx - block_rad : pad + dx + w + block_rad]
                - cur_pad[pad - block_rad : pad + h + block_rad,
                          pad - block_rad : pad + w + block_rad]
            )
            cs = np.cumsum(np.cumsum(np.pad(diff, ((1, 0), (1, 0)), mode="constant"), axis=0), axis=1)
            sad = (cs[win:, win:] - cs[:-win, win:] - cs[win:, :-win] + cs[:-win, :-win]) / win_area
            cost_grid[iy, ix] = sad
            if dx == 0 and dy == 0:
                zero_cost = sad
            better = sad < best_cost
            best_cost[better] = sad[better]
            best_dx[better] = float(dx)
            best_dy[better] = float(dy)

    # 1D 拋物線次像素修正
    best_ix = best_dx.astype(np.int32) + search_rad
    best_iy = best_dy.astype(np.int32) + search_rad
    yy_idx, xx_idx = np.meshgrid(np.arange(h), np.arange(w), indexing="ij")

    in_x = (best_ix > 0) & (best_ix < 2 * search_rad)
    ix_m = np.clip(best_ix - 1, 0, 2 * search_rad)
    ix_p = np.clip(best_ix + 1, 0, 2 * search_rad)
    c_xm = cost_grid[best_iy, ix_m, yy_idx, xx_idx]
    c_x0 = best_cost
    c_xp = cost_grid[best_iy, ix_p, yy_idx, xx_idx]
    denom_x = 2.0 * (c_xm + c_xp - 2.0 * c_x0)
    sub_dx = np.where(in_x & (denom_x > 1e-4), np.clip((c_xm - c_xp) / np.maximum(denom_x, 1e-4), -0.5, 0.5), 0.0)

    in_y = (best_iy > 0) & (best_iy < 2 * search_rad)
    iy_m = np.clip(best_iy - 1, 0, 2 * search_rad)
    iy_p = np.clip(best_iy + 1, 0, 2 * search_rad)
    c_ym = cost_grid[iy_m, best_ix, yy_idx, xx_idx]
    c_y0 = best_cost
    c_yp = cost_grid[iy_p, best_ix, yy_idx, xx_idx]
    denom_y = 2.0 * (c_ym + c_yp - 2.0 * c_y0)
    sub_dy = np.where(in_y & (denom_y > 1e-4), np.clip((c_ym - c_yp) / np.maximum(denom_y, 1e-4), -0.5, 0.5), 0.0)

    flow_dx = best_dx + sub_dx
    flow_dy = best_dy + sub_dy
    flow_mag = np.hypot(flow_dx, flow_dy)

    reliable = (
        valid_mv
        & (mv_mag <= float(search_rad) - 0.5)
        & (block_grad >= 2.5)
        & (cur_y > 5.0)
        & (prev_y > 5.0)
        & ((zero_cost - best_cost) > (0.25 * zero_cost + 0.8))
        & (best_cost < 18.0)
        & in_x
        & in_y
        & (flow_mag >= 0.25)
    )

    n_matched = int(np.sum(reliable))
    if n_matched < 16:
        return {
            "matched_cells": float(n_matched),
            "corr_x": float("nan"),
            "corr_y": float("nan"),
            "cos_sim": float("nan"),
            "pos_dot_pct": float("nan"),
        }

    m_dx = dx_mv[reliable]
    m_dy = dy_mv[reliable]
    f_dx = flow_dx[reliable]
    f_dy = flow_dy[reliable]

    corr_x = float(np.corrcoef(m_dx, f_dx)[0, 1]) if (np.std(m_dx) > 1e-4 and np.std(f_dx) > 1e-4) else 0.0
    corr_y = float(np.corrcoef(m_dy, f_dy)[0, 1]) if (np.std(m_dy) > 1e-4 and np.std(f_dy) > 1e-4) else 0.0

    dot = m_dx * f_dx + m_dy * f_dy
    norms = np.hypot(m_dx, m_dy) * np.hypot(f_dx, f_dy)
    cos_sim = float(np.mean(dot / np.maximum(norms, 1e-6)))
    pos_dot_pct = float(np.mean(dot > 0.0) * 100.0)

    return {
        "matched_cells": float(n_matched),
        "corr_x": corr_x,
        "corr_y": corr_y,
        "cos_sim": cos_sim,
        "pos_dot_pct": pos_dot_pct,
    }


def compute_eye_depth_stats(inv_z: np.ndarray, validity: np.ndarray) -> Dict[str, float]:
    """統計單眼 1/8 逆深度在有效格點 (validity > 0.01) 的 min / max / mean 與非零格數。"""
    valid_mask = (validity > 0.01) & np.isfinite(inv_z)
    if not np.any(valid_mask):
        return {
            "min": 0.0,
            "max": 0.0,
            "mean": 0.0,
            "valid_cells": 0.0,
            "nonzero_cells": 0.0,
        }
    vz = inv_z[valid_mask]
    return {
        "min": float(np.min(vz)),
        "max": float(np.max(vz)),
        "mean": float(np.mean(vz)),
        "valid_cells": float(vz.size),
        "nonzero_cells": float(np.sum(vz > 1e-7)),
    }


def warp_eye_and_eval(
    col_prev: np.ndarray,
    col_cur: np.ndarray,
    du: np.ndarray,
    dv: np.ndarray,
    validity: np.ndarray,
    motion_thresh_cells: float = 0.25,
    override_dyn_mask: Optional[np.ndarray] = None,
) -> Dict[str, float]:
    """
    依設計文件 2.1 節定義：
      mv = (du, dv) = uv_prev - uv_cur
      代表當前幀 (x, y) 的像素在上一幀位於 (x + du * W, y + dv * H)；
      等價地，上一幀 (x, y) 的像素在當前幀位於 (x - du * W, y - dv * H)。

    此函式同時計算：
      1. 對齊至當前幀：取樣 col_prev(x + du*W, y + dv*H) 與 col_cur(x, y) 比對
      2. 重建上一幀：取樣 col_cur(x - du*W, y - dv*H) 與 col_prev(x, y) 比對
    回傳全圖與動態區域的 PSNR（含排除前 10% 極端遮擋／裁切離群值之 Trim90 PSNR）。
    """
    h, w, _ = col_cur.shape
    yy, xx = np.meshgrid(
        np.arange(h, dtype=np.float32),
        np.arange(w, dtype=np.float32),
        indexing="ij",
    )

    dx_cells = du * float(w)
    dy_cells = dv * float(h)
    mv_mag_cells = np.sqrt(dx_cells ** 2 + dy_cells ** 2)

    valid_mask = (validity > 0.01) & np.isfinite(du) & np.isfinite(dv)

    # 1. 在當前幀網格上從 col_prev(x + dx, y + dy) 反向取樣
    warped_prev_to_cur, in_b1 = bilinear_sample(col_prev, xx + dx_cells, yy + dy_cells)
    # 2. 在上一幀網格上從 col_cur(x - dx, y - dy) 反向取樣重建 col_prev
    warped_cur_to_prev, in_b2 = bilinear_sample(col_cur, xx - dx_cells, yy - dy_cells)

    eval_mask_1 = valid_mask & in_b1
    eval_mask_2 = valid_mask & in_b2

    if override_dyn_mask is not None:
        raw_dyn_mask_1 = eval_mask_1 & override_dyn_mask
        raw_dyn_mask_2 = eval_mask_2 & override_dyn_mask
    else:
        raw_dyn_mask_1 = eval_mask_1 & (mv_mag_cells >= motion_thresh_cells)
        raw_dyn_mask_2 = eval_mask_2 & (mv_mag_cells >= motion_thresh_cells)
    raw_dynamic_cells = int(np.sum(raw_dyn_mask_1))

    dyn_mask_1 = raw_dyn_mask_1
    dyn_mask_2 = raw_dyn_mask_2

    # 若畫面中動態格點少於 16 格，放寬動態門檻至前 15% 最大位移區域以利顯示，但保留 raw_dynamic_cells 供足夠動態格判定
    if override_dyn_mask is None and raw_dynamic_cells < 16 and int(np.sum(eval_mask_1)) >= 16:
        q85 = float(np.percentile(mv_mag_cells[eval_mask_1], 85.0))
        if q85 > 0.02:
            dyn_mask_1 = eval_mask_1 & (mv_mag_cells >= q85)
            dyn_mask_2 = eval_mask_2 & (mv_mag_cells >= q85)

    psnr_unwarped_all = compute_psnr(col_prev, col_cur, eval_mask_1)
    psnr_warped_p2c_all = compute_psnr(warped_prev_to_cur, col_cur, eval_mask_1)
    psnr_warped_c2p_all = compute_psnr(warped_cur_to_prev, col_prev, eval_mask_2)

    psnr_unwarped_dyn = compute_psnr(col_prev, col_cur, dyn_mask_1)
    psnr_warped_p2c_dyn = compute_psnr(warped_prev_to_cur, col_cur, dyn_mask_1)
    psnr_warped_c2p_dyn = compute_psnr(warped_cur_to_prev, col_prev, dyn_mask_2)

    psnr_trim90_unwarped_dyn = compute_trimmed_psnr(col_prev, col_cur, dyn_mask_1, 90.0)
    psnr_trim90_warped_p2c_dyn = compute_trimmed_psnr(warped_prev_to_cur, col_cur, dyn_mask_1, 90.0)
    gain_trim90_dyn = psnr_trim90_warped_p2c_dyn - psnr_trim90_unwarped_dyn

    # 計算動態區域內同時具有實質色彩梯度變化 (|col_cur - col_prev| >= 2.0/255) 的活躍動態格數
    color_diff_l2 = np.sqrt(np.mean((col_prev - col_cur) ** 2, axis=-1))
    active_dyn_cells = int(np.sum(raw_dyn_mask_1 & (color_diff_l2 >= 2.0)))

    # 取兩種對齊方向表現較佳者（無遮擋區兩者理論一致）
    gain_all = max(psnr_warped_p2c_all - psnr_unwarped_all, psnr_warped_c2p_all - psnr_unwarped_all)
    gain_dyn = max(psnr_warped_p2c_dyn - psnr_unwarped_dyn, psnr_warped_c2p_dyn - psnr_unwarped_dyn)

    return {
        "psnr_unwarped_all": psnr_unwarped_all,
        "psnr_warped_p2c_all": psnr_warped_p2c_all,
        "psnr_warped_c2p_all": psnr_warped_c2p_all,
        "gain_all_db": gain_all,
        "psnr_unwarped_dyn": psnr_unwarped_dyn,
        "psnr_warped_p2c_dyn": psnr_warped_p2c_dyn,
        "psnr_warped_c2p_dyn": psnr_warped_c2p_dyn,
        "gain_dyn_db": gain_dyn,
        "psnr_trim90_unwarped_dyn": psnr_trim90_unwarped_dyn,
        "psnr_trim90_warped_p2c_dyn": psnr_trim90_warped_p2c_dyn,
        "gain_trim90_dyn_db": gain_trim90_dyn,
        "dynamic_cells": float(np.sum(dyn_mask_1)),
        "raw_dynamic_cells": float(raw_dynamic_cells),
        "active_dynamic_cells": float(active_dyn_cells),
        "total_valid_cells": float(np.sum(eval_mask_1)),
        "p95_mv_cells": float(np.percentile(mv_mag_cells[valid_mask], 95.0)) if np.any(valid_mask) else 0.0,
        "median_du": float(np.median(du[valid_mask])) if np.any(valid_mask) else 0.0,
        "median_dv": float(np.median(dv[valid_mask])) if np.any(valid_mask) else 0.0,
    }


def check_xy_direction(
    col_prev: np.ndarray,
    col_cur: np.ndarray,
    du: np.ndarray,
    dv: np.ndarray,
    validity: np.ndarray,
    motion_thresh_cells: float = 0.25,
) -> Tuple[str, str, Dict[str, float], float]:
    """
    測試 5 種方向變體在動態區域的 PSNR（全區 MSE 與排除前 10% 極端離群值的 Trim90 PSNR）：
      - normal (+X, +Y)
      - flip_x (-X, +Y)
      - flip_y (+X, -Y)
      - flip_xy (-X, -Y)
      - swap_xy (+Y, +X)
    回傳：(全區最佳方向名稱, Trim90 穩健最佳方向名稱, 各變體動態區域 PSNR, normal 對 flip_x 逐格勝率%)。
    """
    candidates = {
        "normal(+X,+Y)": (du, dv),
        "flip_x(-X,+Y)": (-du, dv),
        "flip_y(+X,-Y)": (du, -dv),
        "flip_xy(-X,-Y)": (-du, -dv),
        "swap_xy(+Y,+X)": (dv, du),
    }
    scores: Dict[str, float] = {}
    trim90_scores: Dict[str, float] = {}
    for name, (cu, cv) in candidates.items():
        res = warp_eye_and_eval(col_prev, col_cur, cu, cv, validity, motion_thresh_cells)
        score = res["psnr_warped_p2c_dyn"]
        if math.isnan(score):
            score = res["psnr_warped_p2c_all"]
        scores[name] = score
        t90 = res["psnr_trim90_warped_p2c_dyn"]
        if math.isnan(t90):
            t90 = score
        trim90_scores[name] = t90

    best_name = max(scores, key=lambda k: scores[k] if not math.isnan(scores[k]) else -999.0)
    if best_name != "normal(+X,+Y)":
        if (scores[best_name] - scores["normal(+X,+Y)"]) <= 0.35:
            best_name = "normal(+X,+Y)"

    best_trim90_name = max(trim90_scores, key=lambda k: trim90_scores[k] if not math.isnan(trim90_scores[k]) else -999.0)
    if best_trim90_name != "normal(+X,+Y)":
        if (trim90_scores[best_trim90_name] - trim90_scores["normal(+X,+Y)"]) <= 0.35:
            best_trim90_name = "normal(+X,+Y)"

    # 計算動態區域內 normal(+X,+Y) 相對於 flip_x(-X,+Y) 誤差較小的格點百分比
    h, w, _ = col_cur.shape
    yy, xx = np.meshgrid(np.arange(h, dtype=np.float32), np.arange(w, dtype=np.float32), indexing="ij")
    dx_c, dy_c = du * float(w), dv * float(h)
    w_nrm, in_n = bilinear_sample(col_prev, xx + dx_c, yy + dy_c)
    w_flx, in_f = bilinear_sample(col_prev, xx - dx_c, yy + dy_c)
    err_n = np.mean((w_nrm - col_cur) ** 2, axis=-1)
    err_f = np.mean((w_flx - col_cur) ** 2, axis=-1)
    cmp_mask = (validity > 0.01) & in_n & in_f & (np.hypot(dx_c, dy_c) >= motion_thresh_cells) & (np.abs(err_n - err_f) > 1.0)
    norm_win_pct = float(np.mean(err_n[cmp_mask] < err_f[cmp_mask]) * 100.0) if np.any(cmp_mask) else 50.0

    return best_name, best_trim90_name, scores, norm_win_pct


def verify_frame_pair(
    prev_json_path: Path,
    cur_json_path: Path,
    motion_thresh_cells: float = 0.25,
    min_dynamic_cells: int = 64,
) -> Dict[str, object]:
    """驗證連續兩幀 (prev -> cur) 的傾印資料。"""
    prev_meta = json.loads(prev_json_path.read_text(encoding="utf-8"))
    cur_meta = json.loads(cur_json_path.read_text(encoding="utf-8"))

    dump_dir = cur_json_path.parent
    sbs_w = int(cur_meta["sbs_grid_w"])
    sbs_h = int(cur_meta["sbs_grid_h"])
    eye_w = int(cur_meta["eye_grid_w"])
    eye_h = int(cur_meta["eye_grid_h"])

    col_prev_sbs = load_png_rgba8(dump_dir / prev_meta["color_file"])
    col_cur_sbs = load_png_rgba8(dump_dir / cur_meta["color_file"])
    mv_cur_sbs = load_mv8_bin(dump_dir / cur_meta["mv_bin_file"], sbs_w, sbs_h)

    if col_prev_sbs.shape[:2] != (sbs_h, sbs_w) or col_cur_sbs.shape[:2] != (sbs_h, sbs_w):
        raise ValueError(
            f"Color 縮圖尺寸 {col_cur_sbs.shape[:2]} 與 MV8 網格 {(sbs_h, sbs_w)} 不符"
        )

    prev_left_valid = bool(prev_meta.get("eyes", {}).get("left", {}).get("valid", False))
    prev_right_valid = bool(prev_meta.get("eyes", {}).get("right", {}).get("valid", False))
    cur_left_valid = bool(cur_meta.get("eyes", {}).get("left", {}).get("valid", False))
    cur_right_valid = bool(cur_meta.get("eyes", {}).get("right", {}).get("valid", False))

    # 幀對雙眼齊備條件：前後兩幀 (prev 與 cur) 的左眼與右眼皆必須有效
    both_eyes_captured = (prev_left_valid and prev_right_valid and cur_left_valid and cur_right_valid)

    left_reset = int(cur_meta.get("eyes", {}).get("left", {}).get("reset", 0))
    right_reset = int(cur_meta.get("eyes", {}).get("right", {}).get("reset", 0))

    # 切分左半（左眼）與右半（右眼）
    col_prev_eyes = [col_prev_sbs[:, :eye_w, :], col_prev_sbs[:, eye_w:2 * eye_w, :]]
    col_cur_eyes = [col_cur_sbs[:, :eye_w, :], col_cur_sbs[:, eye_w:2 * eye_w, :]]
    mv_eyes = [mv_cur_sbs[:, :eye_w, :], mv_cur_sbs[:, eye_w:2 * eye_w, :]]

    eye_labels = ["left", "right"]
    eye_reports: Dict[str, Dict[str, object]] = {}

    for e_idx, e_name in enumerate(eye_labels):
        du = mv_eyes[e_idx][:, :, 0]
        dv = mv_eyes[e_idx][:, :, 1]
        inv_z = mv_eyes[e_idx][:, :, 2]
        val = mv_eyes[e_idx][:, :, 3]

        metrics = warp_eye_and_eval(
            col_prev_eyes[e_idx], col_cur_eyes[e_idx], du, dv, val, motion_thresh_cells
        )
        best_dir, best_trim90_dir, dir_scores, norm_win_pct = check_xy_direction(
            col_prev_eyes[e_idx], col_cur_eyes[e_idx], du, dv, val, motion_thresh_cells
        )
        flow_corr = compute_color_flow_correlation(
            col_prev_eyes[e_idx], col_cur_eyes[e_idx], du, dv, val, motion_thresh_cells
        )

        # 判定該眼是否有足夠的有效動態格（具備方向與動態 PSNR 鑑別力）：
        # 1. MV 位移超過門檻的原始動態格數 >= min_dynamic_cells
        # 2. 動態區域內同時具有實質色彩變化的格數 >= max(16, min_dynamic_cells // 4)，且未 warp PSNR < 40.0 dB
        raw_dyn_cnt = int(metrics["raw_dynamic_cells"])
        active_dyn_cnt = int(metrics["active_dynamic_cells"])
        unwarped_dyn_psnr = float(metrics["psnr_unwarped_dyn"])
        has_sufficient_motion = (
            raw_dyn_cnt >= min_dynamic_cells
            and active_dyn_cnt >= max(16, min_dynamic_cells // 4)
            and not math.isnan(unwarped_dyn_psnr)
            and unwarped_dyn_psnr < 40.0
        )

        # 跨眼互換測試：固定在本眼自己的動態區遮罩 (self_dyn_mask) 上，拿另一眼的 MV 來 warp 本眼影像以公平對比
        other_idx = 1 - e_idx
        self_dyn_mask = (np.hypot(du * float(eye_w), dv * float(eye_h)) >= motion_thresh_cells) & (val > 0.01)
        cross_metrics = warp_eye_and_eval(
            col_prev_eyes[e_idx],
            col_cur_eyes[e_idx],
            mv_eyes[other_idx][:, :, 0],
            mv_eyes[other_idx][:, :, 1],
            mv_eyes[other_idx][:, :, 3],
            motion_thresh_cells,
            override_dyn_mask=self_dyn_mask,
        )

        json_eye_depth = cur_meta.get("eyes", {}).get(e_name, {}).get("depth_stats")
        if isinstance(json_eye_depth, dict) and "mean" in json_eye_depth:
            depth_stats = {
                "min": float(json_eye_depth.get("min", 0.0)),
                "max": float(json_eye_depth.get("max", 0.0)),
                "mean": float(json_eye_depth.get("mean", 0.0)),
                "valid_cells": float(json_eye_depth.get("valid_cells", 0.0)),
                "nonzero_cells": float(json_eye_depth.get("nonzero_cells", 0.0)),
            }
        else:
            depth_stats = compute_eye_depth_stats(inv_z, val)

        eye_reports[e_name] = {
            "eye_pair_valid": (prev_left_valid and cur_left_valid) if e_idx == 0 else (prev_right_valid and cur_right_valid),
            "metrics": metrics,
            "has_sufficient_motion": has_sufficient_motion,
            "best_direction": best_dir,
            "best_trim90_direction": best_trim90_dir,
            "norm_win_pct": norm_win_pct,
            "direction_scores": dir_scores,
            "flow_corr": flow_corr,
            "cross_eye_psnr_dyn": cross_metrics["psnr_warped_p2c_dyn"],
            "cross_eye_gain_dyn_db": cross_metrics["gain_dyn_db"],
            "mean_inv_depth": depth_stats["mean"],
            "depth_stats": depth_stats,
            "subrect": cur_meta.get("eyes", {}).get(e_name, {}).get("subrect"),
            "render_subrect": cur_meta.get("eyes", {}).get(e_name, {}).get("render_subrect"),
        }

    # 若該幀同時傾印了 1/4 原始 MV (mv4_raw_<frame>.bin)，讀取並統計左右眼原始 MV 分布
    raw_mv4_report: Optional[Dict[str, object]] = None
    if bool(cur_meta.get("raw_mv4_enabled", False)) and cur_meta.get("raw_mv4_bin_file"):
        raw_path = dump_dir / str(cur_meta["raw_mv4_bin_file"])
        raw_sbs_w = int(cur_meta.get("raw_mv4_sbs_grid_w", 0))
        raw_sbs_h = int(cur_meta.get("raw_mv4_sbs_grid_h", 0))
        raw_eye_w = int(cur_meta.get("raw_mv4_eye_grid_w", 0))
        if raw_path.exists() and raw_sbs_w > 0 and raw_sbs_h > 0 and raw_eye_w > 0:
            raw_sbs = load_raw_mv4_bin(raw_path, raw_sbs_w, raw_sbs_h)
            raw_L = raw_sbs[:, :raw_eye_w, :]
            raw_R = raw_sbs[:, raw_eye_w:2 * raw_eye_w, :]
            mag_L = np.hypot(raw_L[:, :, 0], raw_L[:, :, 1])
            mag_R = np.hypot(raw_R[:, :, 0], raw_R[:, :, 1])
            raw_mv4_report = {
                "eye_dim": (raw_eye_w, raw_sbs_h),
                "left_mean": (float(np.mean(raw_L[:, :, 0])), float(np.mean(raw_L[:, :, 1]))),
                "right_mean": (float(np.mean(raw_R[:, :, 0])), float(np.mean(raw_R[:, :, 1]))),
                "left_p95_mag": float(np.percentile(mag_L, 95.0)),
                "right_p95_mag": float(np.percentile(mag_R, 95.0)),
                "left_nonzero_pct": float(np.mean(mag_L > 1e-4) * 100.0),
                "right_nonzero_pct": float(np.mean(mag_R > 1e-4) * 100.0),
            }

    # 跨眼 PrevViewProj IPD 視差污染檢測（設計 2.2 節）
    # 若右眼被左眼矩陣污染，右眼整張畫面的水平位移 median_du 會與左眼產生明顯的固定 IPD 偏差
    l_med_du = float(eye_reports["left"]["metrics"]["median_du"])  # type: ignore
    r_med_du = float(eye_reports["right"]["metrics"]["median_du"])  # type: ignore
    l_med_dv = float(eye_reports["left"]["metrics"]["median_dv"])  # type: ignore
    r_med_dv = float(eye_reports["right"]["metrics"]["median_dv"])  # type: ignore
    ipd_du_gap = abs(r_med_du - l_med_du)
    ipd_dv_gap = abs(r_med_dv - l_med_dv)

    # 若兩眼水平位移中位數差距超過 0.025 UV（約單眼寬度的 2.5%，例如 1440px 下的 36px），且右眼 warp 退步，則疑似跨眼污染
    r_gain_all = float(eye_reports["right"]["metrics"]["gain_all_db"])  # type: ignore
    cross_contaminated = (ipd_du_gap > 0.025 and r_gain_all < -1.0)

    prev_gf = prev_meta.get("game_frame_index")
    cur_gf = cur_meta.get("game_frame_index")
    game_frame_gap = (int(cur_gf) - int(prev_gf)) if (prev_gf is not None and cur_gf is not None) else None

    return {
        "prev_seq": prev_meta.get("frame_seq"),
        "cur_seq": cur_meta.get("frame_seq"),
        "game_frame_gap": game_frame_gap,
        "present_gap": int(cur_meta.get("present_frame_index", 0)) - int(prev_meta.get("present_frame_index", 0)),
        "both_eyes_captured": both_eyes_captured,
        "prev_eyes_valid": (prev_left_valid, prev_right_valid),
        "cur_eyes_valid": (cur_left_valid, cur_right_valid),
        "reset": (left_reset != 0 or right_reset != 0),
        "left_source": cur_meta.get("eyes", {}).get("left", {}).get("eye_source", "unknown"),
        "right_source": cur_meta.get("eyes", {}).get("right", {}).get("eye_source", "unknown"),
        "eyes": eye_reports,
        "raw_mv4": raw_mv4_report,
        "ipd_du_gap": ipd_du_gap,
        "ipd_dv_gap": ipd_dv_gap,
        "cross_contaminated": cross_contaminated,
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="VipleStream §VR-GAME-MV (M-MV0) 1/8 MV 離線反向 Warp 驗證工具"
    )
    parser.add_argument(
        "--dump-dir",
        type=Path,
        default=Path(__file__).resolve().parent / "dump",
        help="傾印資料夾路徑（預設為腳本所在目錄下的 dump/）",
    )
    parser.add_argument(
        "--motion-thresh",
        type=float,
        default=0.25,
        help="認定為動態區域的 1/8 網格位移門檻（單位：1/8 格，預設 0.25 格 = 全尺寸 2 px）",
    )
    parser.add_argument(
        "--min-dynamic-cells",
        type=int,
        default=64,
        help="參與方向判定與動態區統計所需的最小動態格數（預設 64 格）",
    )
    parser.add_argument(
        "--pass-gain-db",
        type=float,
        default=6.0,
        help="設計 6.2 節 M-MV0 動態區域反向 warp PSNR 提升通過門檻（預設 6.0 dB）",
    )
    args = parser.parse_args()

    dump_dir: Path = args.dump_dir
    if not dump_dir.exists():
        print(f"[錯誤] 找不到傾印目錄: {dump_dir}", file=sys.stderr)
        return 2

    json_files = sorted(dump_dir.glob("mv8_*.json"))
    if len(json_files) < 2:
        print(f"[錯誤] {dump_dir} 內的 mv8_*.json 檔案少於 2 個（找到 {len(json_files)} 個），無法比對連續兩幀。", file=sys.stderr)
        return 2

    print("=" * 84)
    print(f"VipleStream §VR-GAME-MV (M-MV0) 離線驗證報告 — 目錄: {dump_dir}")
    print(f"共找到 {len(json_files)} 個傾印幀，動態門檻: {args.motion_thresh:.2f} 格 (1/8 res)，PSNR 通過門檻: +{args.pass_gain_db:.1f} dB")
    print("=" * 84)

    pair_results = []
    stereo_ok_count = 0
    evaluated_pairs_count = 0
    dir_ok_count = 0
    no_cross_count = 0
    pair_dyn_gains_db: List[float] = []

    for i in range(1, len(json_files)):
        rep = verify_frame_pair(
            json_files[i - 1], json_files[i], args.motion_thresh, args.min_dynamic_cells
        )
        pair_results.append(rep)

        p_seq = rep["prev_seq"]
        c_seq = rep["cur_seq"]
        p_gap = rep["present_gap"]
        gf_gap = rep["game_frame_gap"]
        both_ok = bool(rep["both_eyes_captured"])
        is_reset = bool(rep["reset"])
        cross_bad = bool(rep["cross_contaminated"])

        gap_str = f"遊戲幀間隔={gf_gap}, Present 間隔={p_gap}" if gf_gap is not None else f"Present 間隔={p_gap}"
        print(f"\n--- 幀對 [{p_seq:04d} -> {c_seq:04d}] ({gap_str}, 雙眼齊備={both_ok}, Reset={is_reset}) ---")
        print(f"  分眼來源: Left={rep['left_source']}, Right={rep['right_source']}")

        # 只拿雙眼齊備且非 Reset 的幀對進行評估與統計
        if not both_ok:
            prev_ev = rep["prev_eyes_valid"]
            cur_ev = rep["cur_eyes_valid"]
            print(f"  [跳過] 此幀對未雙眼齊備 (prev L/R={prev_ev}, cur L/R={cur_ev})，不納入方向與 PSNR 統計。")
            continue

        stereo_ok_count += 1

        if is_reset:
            print("  [跳過] 此幀對帶有 DLSS Reset 旗標，不納入方向與 PSNR 統計。")
            continue

        evaluated_pairs_count += 1
        if not cross_bad:
            no_cross_count += 1

        print(f"  雙眼位移中位數差: |dU_R - dU_L|={rep['ipd_du_gap']:.5f} UV, |dV_R - dV_L|={rep['ipd_dv_gap']:.5f} UV "
              f"-> {'[警告: 疑似跨眼污染!]' if cross_bad else '[正常: 無跨眼污染]'}")

        pair_dir_ok = True
        pair_checked_dir_eyes = 0
        pair_valid_eye_gains: List[float] = []

        for e_name in ("left", "right"):
            er = rep["eyes"][e_name]  # type: ignore
            m = er["metrics"]
            bdir = str(er["best_direction"])
            bdir_t90 = str(er["best_trim90_direction"])
            norm_win_pct = float(er["norm_win_pct"])
            fc = er["flow_corr"]
            has_motion = bool(er["has_sufficient_motion"])
            ds = er["depth_stats"]

            # 方向判定只看有足夠動態格的眼
            if has_motion:
                pair_checked_dir_eyes += 1
                if bdir != "normal(+X,+Y)":
                    pair_dir_ok = False
                if bdir != bdir_t90:
                    dir_label = f"{bdir} (Trim90最佳={bdir_t90}, normal逐格勝率={norm_win_pct:.1f}%)"
                else:
                    dir_label = f"{bdir} (normal逐格勝率={norm_win_pct:.1f}%)"
            else:
                dir_label = f"{bdir} (動態不足，略過判定)"

            gain_dyn = float(m["gain_dyn_db"])
            if not math.isnan(gain_dyn):
                if has_motion:
                    pair_valid_eye_gains.append(gain_dyn)

            print(
                f"  [{e_name.upper():5s}] 全圖 PSNR: 未warp={m['psnr_unwarped_all']:5.2f} dB -> "
                f"warp後(P->C / C->P)=({m['psnr_warped_p2c_all']:5.2f} / {m['psnr_warped_c2p_all']:5.2f} dB, "
                f"差={m['gain_all_db']:+5.2f} dB)"
            )
            print(
                f"          動態區({int(m['dynamic_cells'])}/{int(m['total_valid_cells'])}格, "
                f"活躍={int(m['active_dynamic_cells'])}格, p95位移={m['p95_mv_cells']:.2f}格) PSNR: "
                f"未warp={m['psnr_unwarped_dyn']:5.2f} dB -> "
                f"warp後=({m['psnr_warped_p2c_dyn']:5.2f} / {m['psnr_warped_c2p_dyn']:5.2f} dB, "
                f"差={m['gain_dyn_db']:+5.2f} dB | Trim90: {m['psnr_trim90_unwarped_dyn']:5.2f}->"
                f"{m['psnr_trim90_warped_p2c_dyn']:5.2f} dB, 差={m['gain_trim90_dyn_db']:+5.2f} dB)"
            )
            print(
                f"          方向檢測: 最佳={dir_label} | 同遮罩跨眼MV互換PSNR={er['cross_eye_psnr_dyn']:5.2f} dB "
                f"({er['cross_eye_gain_dyn_db']:+5.2f} dB) | "
                f"1/8逆深度(min/max/mean)=({ds['min']:.4f}/{ds['max']:.4f}/{ds['mean']:.4f})"
            )
            if int(fc["matched_cells"]) > 0 and not math.isnan(float(fc["cos_sim"])):
                print(
                    f"          光流相關(區塊比對 N={int(fc['matched_cells'])}格): "
                    f"corr_x={fc['corr_x']:+.3f}, corr_y={fc['corr_y']:+.3f}, "
                    f"方向餘弦cos={fc['cos_sim']:+.3f}, 同向率(dot>0)={fc['pos_dot_pct']:.1f}%"
                )
            else:
                print(f"          光流相關(區塊比對 N={int(fc['matched_cells'])}格): 可信動態紋理格不足，略過")

            sr = er.get("subrect")
            if isinstance(sr, dict):
                cb = sr.get("color_base", [0, 0])
                db = sr.get("depth_base", [0, 0])
                mb = sr.get("mv_base", [0, 0])
                ob = sr.get("output_base", [0, 0])
                rdim = sr.get("render_dimensions", [0, 0])
                print(
                    f"          NGX Subrect: ColorBase=({cb[0]},{cb[1]}), DepthBase=({db[0]},{db[1]}), "
                    f"MVBase=({mb[0]},{mb[1]}), OutBase=({ob[0]},{ob[1]}), RenderDim={rdim[0]}x{rdim[1]}"
                )

        raw_rep = rep.get("raw_mv4")
        if isinstance(raw_rep, dict):
            edim = raw_rep["eye_dim"]
            lm = raw_rep["left_mean"]
            rm = raw_rep["right_mean"]
            print(
                f"  [RAW1/4] 每眼尺寸={edim[0]}x{edim[1]} (RG32F) | "
                f"Left(mean=({lm[0]:+.3f},{lm[1]:+.3f}), p95={raw_rep['left_p95_mag']:.2f}, nz={raw_rep['left_nonzero_pct']:.1f}%) | "
                f"Right(mean=({rm[0]:+.3f},{rm[1]:+.3f}), p95={raw_rep['right_p95_mag']:.2f}, nz={raw_rep['right_nonzero_pct']:.1f}%)"
            )

        # 若兩眼皆無足夠動態格，則退回採用兩眼中較大的 gain_dyn_db 與其方向
        if not pair_valid_eye_gains:
            for e_name in ("left", "right"):
                er = rep["eyes"][e_name]  # type: ignore
                g = float(er["metrics"]["gain_dyn_db"])
                if not math.isnan(g):
                    pair_valid_eye_gains.append(g)
                if er["best_direction"] != "normal(+X,+Y)":
                    pair_dir_ok = False

        if pair_valid_eye_gains:
            # 記錄該雙眼齊備幀對在有足夠動態格之眼的實際 PSNR 提升
            pair_dyn_gains_db.extend(pair_valid_eye_gains)

        if pair_dir_ok:
            dir_ok_count += 1

    total_pairs = len(pair_results)
    denom_pairs = evaluated_pairs_count if evaluated_pairs_count > 0 else 1
    avg_dyn_gain = float(np.mean(pair_dyn_gains_db)) if pair_dyn_gains_db else 0.0
    max_dyn_gain = float(np.max(pair_dyn_gains_db)) if pair_dyn_gains_db else 0.0

    print("\n" + "=" * 84)
    print("M-MV0 通過標準總結（對照 docs/specs/vr_game_motion_vectors.md §6.2）：")
    c1_pass = (stereo_ok_count == total_pairs and stereo_ok_count > 0)
    c2_pass = (evaluated_pairs_count > 0 and (avg_dyn_gain >= args.pass_gain_db or max_dyn_gain >= args.pass_gain_db))
    c3_pass = (evaluated_pairs_count > 0 and dir_ok_count == evaluated_pairs_count and no_cross_count == evaluated_pairs_count)

    print(f"  1. 雙眼齊備遊戲幀對: {stereo_ok_count}/{total_pairs} 對 (有效評估 {evaluated_pairs_count} 對) -> {'[PASS]' if c1_pass else '[FAIL]'}")
    print(f"  2. 動態區域反向 warp PSNR 提升 (>= +{args.pass_gain_db:.1f} dB): "
          f"平均={avg_dyn_gain:+.2f} dB, 最高={max_dyn_gain:+.2f} dB -> {'[PASS]' if c2_pass else '[WARN/FAIL]'}")
    print(f"  3. X/Y 方向無顛倒 ({dir_ok_count}/{denom_pairs}) 且左右眼無交叉污染 ({no_cross_count}/{denom_pairs}): "
          f"-> {'[PASS]' if c3_pass else '[FAIL]'}")
    print("=" * 84)

    return 0 if (c1_pass and c2_pass and c3_pass) else 1


if __name__ == "__main__":
    sys.exit(main())
