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


def warp_eye_and_eval(
    col_prev: np.ndarray,
    col_cur: np.ndarray,
    du: np.ndarray,
    dv: np.ndarray,
    validity: np.ndarray,
    motion_thresh_cells: float = 0.25,
) -> Dict[str, float]:
    """
    依設計文件 2.1 節定義：
      mv = (du, dv) = uv_prev - uv_cur
      代表當前幀 (x, y) 的像素在上一幀位於 (x + du * W, y + dv * H)；
      等價地，上一幀 (x, y) 的像素在當前幀位於 (x - du * W, y - dv * H)。

    此函式同時計算：
      1. 對齊至當前幀：取樣 col_prev(x + du*W, y + dv*H) 與 col_cur(x, y) 比對
      2. 重建上一幀：取樣 col_cur(x - du*W, y - dv*H) 與 col_prev(x, y) 比對
    回傳全圖與動態區域的 PSNR。
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

    dyn_mask_1 = eval_mask_1 & (mv_mag_cells >= motion_thresh_cells)
    dyn_mask_2 = eval_mask_2 & (mv_mag_cells >= motion_thresh_cells)

    # 若畫面中動態格點少於 16 格，放寬動態門檻至前 15% 最大位移區域以利統計
    if int(np.sum(dyn_mask_1)) < 16 and int(np.sum(eval_mask_1)) >= 16:
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

    # 取兩種對齊方向表現較佳或平均（無遮擋區兩者理論一致）
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
        "dynamic_cells": float(np.sum(dyn_mask_1)),
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
) -> Tuple[str, Dict[str, float]]:
    """
    測試 5 種方向變體在動態區域的 PSNR：
      - normal (+X, +Y)
      - flip_x (-X, +Y)
      - flip_y (+X, -Y)
      - flip_xy (-X, -Y)
      - swap_xy (+Y, +X)
    回傳最佳方向名稱與各變體的動態區域 PSNR。
    """
    candidates = {
        "normal(+X,+Y)": (du, dv),
        "flip_x(-X,+Y)": (-du, dv),
        "flip_y(+X,-Y)": (du, -dv),
        "flip_xy(-X,-Y)": (-du, -dv),
        "swap_xy(+Y,+X)": (dv, du),
    }
    scores: Dict[str, float] = {}
    for name, (cu, cv) in candidates.items():
        res = warp_eye_and_eval(col_prev, col_cur, cu, cv, validity, motion_thresh_cells)
        score = res["psnr_warped_p2c_dyn"]
        if math.isnan(score):
            score = res["psnr_warped_p2c_all"]
        scores[name] = score

    best_name = max(scores, key=lambda k: scores[k] if not math.isnan(scores[k]) else -999.0)
    # 若正常方向與最高分差距在 0.35 dB 內（例如純水平移動時 Y 軸無位移），視為正常方向
    if best_name != "normal(+X,+Y)":
        if (scores[best_name] - scores["normal(+X,+Y)"]) <= 0.35:
            best_name = "normal(+X,+Y)"
    return best_name, scores


def verify_frame_pair(
    prev_json_path: Path,
    cur_json_path: Path,
    motion_thresh_cells: float = 0.25,
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

    left_valid = bool(cur_meta.get("eyes", {}).get("left", {}).get("valid", False))
    right_valid = bool(cur_meta.get("eyes", {}).get("right", {}).get("valid", False))
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
        best_dir, dir_scores = check_xy_direction(
            col_prev_eyes[e_idx], col_cur_eyes[e_idx], du, dv, val, motion_thresh_cells
        )

        # 跨眼互換測試：故意拿另一眼的 MV 來 warp 本眼影像
        other_idx = 1 - e_idx
        cross_metrics = warp_eye_and_eval(
            col_prev_eyes[e_idx],
            col_cur_eyes[e_idx],
            mv_eyes[other_idx][:, :, 0],
            mv_eyes[other_idx][:, :, 1],
            mv_eyes[other_idx][:, :, 3],
            motion_thresh_cells,
        )

        eye_reports[e_name] = {
            "metrics": metrics,
            "best_direction": best_dir,
            "direction_scores": dir_scores,
            "cross_eye_psnr_dyn": cross_metrics["psnr_warped_p2c_dyn"],
            "mean_inv_depth": float(np.mean(inv_z[val > 0.01])) if np.any(val > 0.01) else 0.0,
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

    return {
        "prev_seq": prev_meta.get("frame_seq"),
        "cur_seq": cur_meta.get("frame_seq"),
        "present_gap": int(cur_meta.get("present_frame_index", 0)) - int(prev_meta.get("present_frame_index", 0)),
        "both_eyes_captured": (left_valid and right_valid),
        "reset": (left_reset != 0 or right_reset != 0),
        "left_source": cur_meta.get("eyes", {}).get("left", {}).get("eye_source", "unknown"),
        "right_source": cur_meta.get("eyes", {}).get("right", {}).get("eye_source", "unknown"),
        "eyes": eye_reports,
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
    dir_ok_count = 0
    no_cross_count = 0
    dyn_gains_db: List[float] = []

    for i in range(1, len(json_files)):
        rep = verify_frame_pair(json_files[i - 1], json_files[i], args.motion_thresh)
        pair_results.append(rep)

        p_seq = rep["prev_seq"]
        c_seq = rep["cur_seq"]
        p_gap = rep["present_gap"]
        both_ok = bool(rep["both_eyes_captured"])
        is_reset = bool(rep["reset"])
        cross_bad = bool(rep["cross_contaminated"])

        if both_ok:
            stereo_ok_count += 1
        if not cross_bad:
            no_cross_count += 1

        print(f"\n--- 幀對 [{p_seq:04d} -> {c_seq:04d}] (Present 間隔={p_gap}, 雙眼齊備={both_ok}, Reset={is_reset}) ---")
        print(f"  分眼來源: Left={rep['left_source']}, Right={rep['right_source']}")
        print(f"  雙眼位移中位數差: |dU_R - dU_L|={rep['ipd_du_gap']:.5f} UV, |dV_R - dV_L|={rep['ipd_dv_gap']:.5f} UV "
              f"-> {'[警告: 疑似跨眼污染!]' if cross_bad else '[正常: 無跨眼污染]'}")

        pair_dir_ok = True
        for e_name in ("left", "right"):
            er = rep["eyes"][e_name]  # type: ignore
            m = er["metrics"]
            bdir = er["best_direction"]
            if bdir != "normal(+X,+Y)":
                pair_dir_ok = False

            gain_dyn = float(m["gain_dyn_db"])
            if not math.isnan(gain_dyn) and not is_reset and p_gap == 1:
                dyn_gains_db.append(gain_dyn)

            print(
                f"  [{e_name.upper():5s}] 全圖 PSNR: 未warp={m['psnr_unwarped_all']:5.2f} dB -> "
                f"warp後(P->C / C->P)=({m['psnr_warped_p2c_all']:5.2f} / {m['psnr_warped_c2p_all']:5.2f} dB, "
                f"差={m['gain_all_db']:+5.2f} dB)"
            )
            print(
                f"          動態區({int(m['dynamic_cells'])}/{int(m['total_valid_cells'])}格, p95位移={m['p95_mv_cells']:.2f}格) PSNR: "
                f"未warp={m['psnr_unwarped_dyn']:5.2f} dB -> "
                f"warp後=({m['psnr_warped_p2c_dyn']:5.2f} / {m['psnr_warped_c2p_dyn']:5.2f} dB, "
                f"差={m['gain_dyn_db']:+5.2f} dB)"
            )
            print(
                f"          方向檢測: 最佳={bdir} | 跨眼MV互換PSNR={er['cross_eye_psnr_dyn']:5.2f} dB | "
                f"平均逆深度={er['mean_inv_depth']:.4f}"
            )

        if pair_dir_ok:
            dir_ok_count += 1

    total_pairs = len(pair_results)
    avg_dyn_gain = float(np.mean(dyn_gains_db)) if dyn_gains_db else 0.0
    max_dyn_gain = float(np.max(dyn_gains_db)) if dyn_gains_db else 0.0

    print("\n" + "=" * 84)
    print("M-MV0 通過標準總結（對照 docs/specs/vr_game_motion_vectors.md §6.2）：")
    c1_pass = (stereo_ok_count == total_pairs)
    c2_pass = (avg_dyn_gain >= args.pass_gain_db or max_dyn_gain >= args.pass_gain_db)
    c3_pass = (dir_ok_count == total_pairs and no_cross_count == total_pairs)

    print(f"  1. 每次 Present 穩定攔截左右眼各 1 次: {stereo_ok_count}/{total_pairs} 對 -> {'[PASS]' if c1_pass else '[FAIL]'}")
    print(f"  2. 動態區域反向 warp PSNR 提升 (>= +{args.pass_gain_db:.1f} dB): "
          f"平均={avg_dyn_gain:+.2f} dB, 最高={max_dyn_gain:+.2f} dB -> {'[PASS]' if c2_pass else '[WARN/FAIL]'}")
    print(f"  3. X/Y 方向無顛倒 ({dir_ok_count}/{total_pairs}) 且左右眼無交叉污染 ({no_cross_count}/{total_pairs}): "
          f"-> {'[PASS]' if c3_pass else '[FAIL]'}")
    print("=" * 84)

    return 0 if (c1_pass and c2_pass and c3_pass) else 1


if __name__ == "__main__":
    sys.exit(main())
