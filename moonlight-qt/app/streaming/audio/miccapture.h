// §VR-MIC（2026-11-05）：頭盔麥克風 → host。
//
// 開系統預設的錄音裝置（SDL2 API；Steam Frame 上是 PipeWire 的 PulseAudio 介面，Flatpak 已有 --socket=pulseaudio），
// 48 kHz 單聲道、每 20 ms 用 Opus（VOIP 模式、約 24 kbps、in-band FEC、預期掉包 10%）編一個封包，交給 send
// 送出（Session 包成 VIPLE_VR_C2S_MIC 走 VR 控制通道 unsequenced）。payload 格式：{u16 seq（LE）, Opus 封包}，
// 整則不超過 VIPLE_VR_MAX_CTRL_PAYLOAD（Opus 封包上限設 240 bytes）。
//
// 執行緒：SDL 的錄音 callback 只把樣本放進環形緩衝；另一條編碼執行緒每湊滿 960 個樣本就編碼並呼叫 send（send 不可阻塞太久）。
// 靜音（setMuted）時照樣送封包，但內容是靜音（Opus 會壓到很小），host 那邊的時序不會亂。
#pragma once

#include <cstdint>
#include <functional>
#include <memory>

class MicCapture
{
public:
    using SendFn = std::function<void(const uint8_t* payload, int len)>;

    /// 開錄音裝置並開始送；失敗（沒有錄音裝置、權限、Opus 建不起來）回 nullptr，並寫 [VIPLE-VR-MIC] log
    static std::unique_ptr<MicCapture> start(SendFn send);

    virtual ~MicCapture() = default;  // 解構＝停止錄音與編碼執行緒

    virtual void setMuted(bool muted) = 0;

    /// 統計（log 用）：已送出的封包數、錄音緩衝溢出（丟掉的樣本）次數
    virtual uint32_t packetsSent() const = 0;
    virtual uint32_t overruns() const = 0;
};
