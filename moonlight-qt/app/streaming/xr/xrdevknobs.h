/**
 * @file streaming/xr/xrdevknobs.h
 * @brief PCVR 影像圖層的幾個開關（命令列解析那一側寫、XR 那一側讀）。不依賴 OpenXR，沒有 OpenXR 的平台也能 include。
 *
 * §VR-FOVEA-REPROJ（2026-11-15）。注視點分塊（§VR-FOVEA-TILES）交兩種圖層：全視野那塊是 projection、注視點那塊是放在
 * 50 m 外的 quad。頭盔的合成器對兩種圖層走不同的重投影：projection 會照「送出時的眼睛位置 → 顯示時的眼睛位置」的位移
 * 做位置上的補償（把畫面當成在某個固定距離的平面上），quad 則是世界座標裡真的那塊板子（50 m 外，幾公分的頭部位移對它
 * 幾乎沒有影響）。往返 80 ms 的遠端串流下，頭在這段時間平移了幾公分，兩層就往相反的方向偏（配戴回報：注視點裡球桌回正、
 * 注視點外球桌往另一個方向移動）。
 *   layersMode：
 *     0＝原本的做法（projection 用算圖當時的眼睛位置）；
 *     1＝projection 的眼睛位置改用「這一格顯示時的眼睛位置」（方向仍是算圖當時的）：合成器看到的位移是 0，只剩轉動的補償，
 *        和 quad 的行為一致；不花 GPU；
 *     2＝同 1，另外把全視野那塊也用 quad 再疊一次（和注視點那塊走同一條路，兩層一定一致；合成器多畫兩塊大的 quad）。
 *   −1＝用內建的預設值（kLayersDefault＝1）。只在有注視點 quad 的時候才有作用；其餘情況（沒開注視點）不動。
 * poseOffset（dev）：把算圖姿態往頭的右／上／後方向平移這麼多公尺再交給合成器——頭盔不動時用它製造已知的重投影量，
 * 配合合成器的截圖看兩層有沒有錯開。
 * cycleSec（dev）：同一場內每這麼多秒依序換一種做法（A＝0、B＝1、C＝2），換的時候畫面上標 A／B／C 兩秒半——配戴的人直接比
 *   哪一種的兩層是一致的（合成器的畫面抓不出來，只能靠眼睛）。0＝不輪流（預設）；命令列 --vr-fovea-layers cycle＝每 20 秒。
 */
#pragma once

#include <atomic>

namespace xrdev {

    // 2026-11-15 配戴（同一場輪流 A／B／C，各 20 秒）：0 的現象還在；1「非常流暢並解決了問題」；2 也解決了，但 1 更流暢 → 預設 1
    constexpr int kLayersDefault = 1;
    constexpr int kCycleDefaultSec = 0;
    // 沒開注視點（只有一層 projection）時，每這麼多秒在 A＝原本、B＝眼睛位置用顯示時的值 之間切換並標字母（dev 實驗用，0＝關）。
    // 2026-11-15 使用者配戴比過（7 分鐘、切換 22 次，頭的位移 p50 4～9 mm、p95 9～19 mm）：「目視無法區分差異」。
    // 整張畫面一起偏零點幾度沒有參照物、看不出來；看得出來的是兩層偏得不一樣。所以沒開注視點時維持原本的做法，不切換
    constexpr int kNoInsetCycleDefaultSec = 0;

    inline std::atomic<int> layersMode{-1};
    inline std::atomic<int> cycleSec{-1};
    inline std::atomic<float> poseOffsetX{0.0f};
    inline std::atomic<float> poseOffsetY{0.0f};
    inline std::atomic<float> poseOffsetZ{0.0f};

    inline int effectiveLayersMode()
    {
        const int m = layersMode.load(std::memory_order_relaxed);
        return m >= 0 && m <= 2 ? m : kLayersDefault;
    }

    /// 沒開注視點時的切換間隔（秒）；0＝不切換（照原本的做法）
    inline int effectiveNoInsetCycleSec()
    {
        const int m = layersMode.load(std::memory_order_relaxed);
        return m >= 0 && m <= 2 ? 0 : kNoInsetCycleDefaultSec;
    }

    /// 輪流的間隔（秒）；0＝不輪流。明確指定了做法時不輪流
    inline int effectiveCycleSec()
    {
        const int m = layersMode.load(std::memory_order_relaxed);
        if (m >= 0 && m <= 2) {
            return 0;
        }
        const int c = cycleSec.load(std::memory_order_relaxed);
        return c >= 0 ? c : kCycleDefaultSec;
    }

}  // namespace xrdev
