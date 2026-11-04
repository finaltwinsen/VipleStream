/**
 * @file src/platform/windows/steamvr_driver/hand_skeleton.h
 * @brief §VR-SKELETON（2026-11-04）：由手指彎曲程度算 SteamVR 手部骨架（31 根骨頭）。
 *
 * Frame 的 input profile 有 /input/skeleton/left|right，driver 原本沒建：用骨架判斷握持、顯示手的遊戲
 * （Half-Life: Alyx、ACC 等的 action manifest 都有 skeleton 動作）會記 "invalid device path for skeleton output"，
 * 手的模型不會動。Frame 沒有手指追蹤，彎曲程度從扳機、握把、拇指的觸碰推出來（VRSkeletalTracking_Partial）。
 *
 * 骨架的算法改寫自 Valve OpenVR SDK 的範例 driver samples/drivers/drivers/handskeletonsimulation/src/hand_simulation.cpp
 * （Copyright (c) Valve Corporation，BSD 3-Clause，授權全文見 SteamVR driver 套件裡的 LICENSE-OpenVR.txt），
 * 範例本身參考 Monado 的 u_hand_simulation.c。改成全 float、不依賴範例的 vrmath.h，並補上範例沒填的 5 根 aux 骨頭。
 */
#pragma once

#include <openvr_driver.h>

namespace vrdrv::hand {

  constexpr uint32_t k_bone_count = 31;

  /// 0＝伸直、1＝完全彎曲
  struct curls_t {
    float thumb = 0.0f;
    float index = 0.0f;
    float middle = 0.0f;
    float ring = 0.0f;
    float pinky = 0.0f;
  };

  /// 填滿 out[k_bone_count]（父骨頭座標系，SteamVR 骨架輸入的格式）
  void compute(bool right, const curls_t &curls, vr::VRBoneTransform_t *out);

}  // namespace vrdrv::hand
