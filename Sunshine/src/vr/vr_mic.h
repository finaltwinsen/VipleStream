/**
 * @file src/vr/vr_mic.h
 * @brief §VR-MIC（2026-11-05）：頭盔麥克風 → host 的虛擬麥克風。
 *
 * 頭盔每 20 ms 送一則 C2S MIC（VIPLE_VR_C2S_MIC）：{u16 seq（LE）, Opus 封包（48 kHz 單聲道）}，走 VR 控制通道的
 * unsequenced（掉了不重送）。host 解碼後寫進 Steam 裝的虛擬裝置「Steam Streaming Microphone」的播放端；那個驅動程式
 * 會把播放端的聲音轉到同名的錄音端，遊戲／Discord 選「Microphone (Steam Streaming Microphone)」就收得到
 * （做法同 Apollo PR #1428）。Steam 本來就是 VR 的前提，不必自己寫或簽驅動程式。
 *
 * 只有 Windows 有實作（vr_mic_win.cpp）；其他平台 available() 恆為 false、create() 回 nullptr（vr_mic_stub.cpp）。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace vr::mic {

  /// host 上找得到可寫入的虛擬麥克風（Steam Streaming Microphone 的播放端）
  bool available();

  /**
   * @brief 每個 PCVR session 一份。建立時開好裝置與背景播放執行緒，解構時停止並釋放。
   */
  class sink_t {
  public:
    /// 找不到裝置或開不起來回 nullptr（呼叫端改送 VIPLE_VR_NOTICE_MIC_UNAVAILABLE）
    static std::unique_ptr<sink_t> create();
    virtual ~sink_t() = default;

    /**
     * @brief 收到一則 C2S MIC（控制執行緒呼叫，不可阻塞）。
     * seq 跳號時：剛好少一則就用下一則的 Opus in-band FEC 補，少更多就用 PLC；亂序、重複或太舊的直接丟。
     * 解碼後放進 60～80 ms 的抖動緩衝，由播放執行緒依裝置的節奏取用；緩衝空了補靜音、塞爆了丟最舊的。
     */
    virtual void push(uint16_t seq, const uint8_t *opus, size_t len) = 0;
  };

}  // namespace vr::mic
