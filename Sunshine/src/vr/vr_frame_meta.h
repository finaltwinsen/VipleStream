/**
 * @file src/vr/vr_frame_meta.h
 * @brief VipleStream 2.0 §VR：一幀的 VR metadata（平台中立的 POD）。
 *
 * M1b S1-03：原本定義在 video.h。platform/common.h 的 img_t 要掛
 * std::optional<video::vr_frame_meta_t>（需要完整型別），而 video.h 本身 include
 * platform/common.h，留在 video.h 會形成 include 循環，所以搬到這個獨立的小 header。
 *
 * 規則：
 *   - 只依賴標準函式庫；**不可** include video.h、platform/common.h 或任何平台 header
 *     （Windows／Linux／macOS 共用，Linux 建置也會編到）。
 *   - 保留 namespace video，既有使用者（video.cpp、stream.cpp）不必改名。
 *   - 新欄位只加在尾端、一律用 default member initializer（不變式 4），
 *     既有的 `vr_frame_meta_t vr_meta {};` 等初始化點不必改。
 */
#pragma once

// standard includes
#include <cstdint>

namespace video {

  /**
   * @brief VipleStream 2.0 §VR：一幀的 VR metadata（encode 執行緒填、videoBroadcast 執行緒
   *        組 24 B 的 0x81 header）。POD，一般 session 永遠是全 0。
   *
   * M1b 起 pcvr 的擷取端（display_vr_t）也會把 driver frame descriptor 的內容填進
   * `platf::img_t::vr`；一般 session 的 `img_t::vr` 永遠是 nullopt。
   */
  struct vr_frame_meta_t {
    bool valid = false;  // 帶了收到的 tracking 樣本（POSE_VALID|ECHO_MATCHED）
    uint32_t echoSampleId = 0;  // 0 = 沒有樣本
    float pos[3] = {};  // HMD 位置（公尺）
    float rot[4] = {0.0f, 0.0f, 0.0f, 1.0f};  // HMD 四元數 x, y, z, w
    uint8_t flags = 0;  // VIPLE_VR_FF_*（REFRESH_DONE 以外；那個由 wave_done 決定）
    bool in_wave = false;  // 屬於一波 intra refresh（frameType = 4）
    bool wave_start = false;  // wave 的第一幀：videoBroadcast 看到就送 REFRESH_START
    bool wave_done = false;  // wave 的最後一幀：vrFlags 加 VIPLE_VR_FF_REFRESH_DONE
    uint8_t wave_len = 0;  // wave 長度（REFRESH_START.frameCnt）
    uint8_t wave_reason = 0;  // VIPLE_VR_REFRESH_*

    // ── M1b S1-03 新增（尾端、default member initializer）────────────────
    // 只有 pcvr（display_vr_t）會從 driver 的 frame descriptor（vr_ipc_abi.h 的
    // vripc_frame_desc_t）填入；M1a stub 與一般 session 一律維持 0。
    // 這些欄位不改變 0x81 header 的線上格式（stream.cpp 只挑需要的欄位組 24 B）。
    uint8_t layout_epoch = 0;  // 眼睛配置 epoch（= 本 generation config 的值；0x81 的 layoutEpoch 由此取）
    uint16_t space_epoch = 0;  // 空間 epoch（space-delta／recenter 換代時遞增）
    uint64_t frame_id = 0;  // driver descriptor 的 frame_id（單調遞增，用來算 skipped／連續性）
    int64_t present_qpc = 0;  // driver Present() 進入時的時鐘 tick（platf::vr_clock_ticks() 時基；Windows = raw QPC）
  };

}  // namespace video
