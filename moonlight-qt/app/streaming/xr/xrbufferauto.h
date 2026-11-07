// VipleStream 2.0 §VR-BUFFER-AUTO（2026-11-04）：頭盔端緩衝深度自動在 1、2 格之間切換。
// header-only、不依賴 OpenXR／Vulkan：XrVideo（frame thread）與離線模擬（scripts/vr/bufauto_sim）共用同一份邏輯。
//
// 「缺貨」＝這一格只能重複上一張；深度 2 時另把「只剩最新一張可顯示」也算進去（少一格緩衝時那一格就會重複）。
//  - 深度 1：5 秒內缺貨累計到 10 格（每秒 2 格）就加到上限。遠端行動網路平常每秒約 7 格。
//    升格不跳不重複：只是下一次缺貨時不再追上最新，自然多留一格。
//  - 深度 2：切換後至少 60 秒，且 30 秒視窗內「少一格就會缺貨」< 15 格（每秒 0.5 格）→ 退回 1（會跳過一張）。
#pragma once

#include <algorithm>
#include <cstdint>

namespace xrbuf {

    struct AutoDepth {
        static constexpr uint64_t kUpWindowUs = 5000000ull;
        static constexpr uint32_t kUpStarve = 10;
        static constexpr uint64_t kDownWindowUs = 30000000ull;
        static constexpr uint32_t kDownStarve = 15;
        static constexpr uint64_t kMinDwellUs = 60000000ull;

        int maxDepth = 2;
        int depth = 1;
        uint64_t winUs = 0;      // 目前判斷視窗的起點（0＝還沒開始）
        uint64_t sinceUs = 0;    // 上次切換的時刻
        uint32_t starve = 0;     // 視窗內的缺貨格數
        uint32_t ups = 0, downs = 0;
        uint32_t sameRun = 0;    // §VR-RATE-AUTO：這一張已經連續重複幾格

        void reset(int maxD)
        {
            maxDepth = std::max(1, maxD);
            depth = 1;
            winUs = sinceUs = 0;
            starve = 0;
        }

        // 每顯示一格呼叫一次（shown＝這一格顯示的序號、lastShown＝上一格、newest＝目前最新）。回傳深度有沒有變。
        // §VR-RATE-AUTO：div＝串流每張佔幾格（半速 2）。半速時每張本來就顯示兩格，重複到第 div 格以上才算缺貨；
        // 「只剩最新一張」那一條只在全速時算（半速時最新那一張還有一格可以顯示）
        bool step(uint64_t shown, uint64_t lastShown, uint64_t newest, uint64_t now, int div = 1)
        {
            if (winUs == 0) {
                winUs = now;
                sinceUs = now;
            }
            sameRun = shown == lastShown ? sameRun + 1 : 0;
            if ((shown == lastShown && sameRun >= static_cast<uint32_t>(div < 1 ? 1 : div)) || (div <= 1 && depth >= 2 && shown == newest)) {
                starve++;
            }
            const int before = depth;
            if (depth <= 1) {
                if (starve >= kUpStarve) {
                    depth = std::min(2, maxDepth);
                }
                else if (now - winUs < kUpWindowUs) {
                    return false;
                }
            }
            else {
                if (now - winUs < kDownWindowUs) {
                    return false;
                }
                if (now - sinceUs >= kMinDwellUs && starve < kDownStarve) {
                    depth = 1;
                }
            }
            winUs = now;
            starve = 0;
            if (depth != before) {
                sinceUs = now;
                (depth > before ? ups : downs)++;
                return true;
            }
            return false;
        }
    };

    // 要顯示的序號＝max(上次顯示的下一張, 最新往前數 depth 張)，不超過最新（XrVideo::current 同一條規則）
    inline uint64_t pick(uint64_t lastShown, uint64_t newest, int depth)
    {
        const uint64_t d = static_cast<uint64_t>(std::max(0, depth));
        uint64_t want = std::max(lastShown + 1, newest > d ? newest - d : 0);
        return std::min(want, newest);
    }

}  // namespace xrbuf
