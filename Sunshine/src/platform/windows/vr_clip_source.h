#pragma once
#include <d3d11.h>
#include <cstdint>
#include <memory>
#include <string>

namespace platf::dxgi {
  /// §VR-NETBENCH：VR 網路測試的影片來源。open() 在指定的顯示卡上建自己的 D3D11 裝置（VIDEO_SUPPORT＋多執行緒保護），
  /// 用 Media Foundation 硬體解碼（IMFSourceReader＋IMFDXGIDeviceManager），背景解碼輪替寫入 ring 張共享貼圖，到結尾自動從頭循環。
  class vr_clip_source_t {
  public:
    /// adapter_luid：要用的顯示卡（和擷取端同一張）。out_w／out_h：輸出貼圖大小（每一階的串流尺寸，例如 2304x1152、4608x2304）。
    /// ring：共享貼圖環狀緩衝區大小（預設 4）。
    /// 失敗回 nullptr（原因寫 log）。
    static std::unique_ptr<vr_clip_source_t> open(
      LUID adapter_luid,
      const std::wstring &path,
      uint32_t out_w,
      uint32_t out_h,
      int ring = 4
    );
    ~vr_clip_source_t();

    int ring_size() const;
    HANDLE shared_handle(int slot) const;      ///< 擷取端 OpenSharedResource1 用
    bool failed() const;                        ///< 背景解碼已經放棄
    uint64_t decoded() const;                   ///< 背景已解好的張數（統計）
    double decode_ms_max_and_reset();           ///< 自上次呼叫以來，單張（讀樣本＋Blt）最長耗時，呼叫後歸零（執行緒安全，用 atomic 或 mutex）

    uint32_t clip_width() const;   ///< 影片原始寬
    uint32_t clip_height() const;  ///< 影片原始高
    double clip_fps() const;       ///< 影片的幀率（讀 MF_MT_FRAME_RATE）
    uint64_t loops() const;        ///< 已經循環幾次
    int64_t position_ms() const;   ///< 最近一張在片段裡的時間（ms；還沒讀過回 -1）

  private:
    vr_clip_source_t() = default;
    struct impl_t;
    std::unique_ptr<impl_t> impl_;
  };
}  // namespace platf::dxgi
