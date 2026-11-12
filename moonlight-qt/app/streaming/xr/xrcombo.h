// VipleStream 2.0 §VR：menu＋trigger 按住一段時間 → SYSTEM 的判斷（純邏輯，沒有 OpenXR／Qt 相依，可單獨測試）。
//
// 規則：
//   - 兩鍵都按著滿 comboNs → 進入組合狀態：遮掉 MENU 與 TRIGGER，送 SYSTEM。
//   - 組合狀態結束（任一鍵放開）之後，另一鍵如果還按著，繼續遮掉直到它也放開。
//     §REVIEW-1111（第二輪）：原本組合一結束就不遮了，還按著沒動的那一鍵在下一格重新出現，被當成一次新的按下
//     （按鍵計數多加 1、host 收到一次多餘的 MENU 或 TRIGGER）。
#pragma once

#include <cstdint>

namespace xrcombo {

struct State {
    uint64_t startNs = 0;     // 兩鍵同時按住的起點（0＝沒有同時按著）
    bool active = false;      // 已經變成 SYSTEM
    uint32_t afterMask = 0;   // 組合結束後還沒放開、要繼續遮掉的鍵
};

// 依這一格的按鍵更新狀態並改寫 buttons／touches／trigger。回傳 true＝這一格剛變成 SYSTEM。
inline bool apply(State& st, uint32_t menuBit, uint32_t triggerBit, uint32_t systemBit, uint64_t comboNs, uint64_t nowNs,
                  uint32_t& buttons, uint32_t& touches, float& trigger)
{
    const bool held = (buttons & menuBit) && (buttons & triggerBit);
    bool fired = false;
    if (!held) {
        if (st.active) {
            st.afterMask = menuBit | triggerBit;
        }
        st.startNs = 0;
        st.active = false;
    }
    else {
        if (st.startNs == 0) {
            st.startNs = nowNs;
        }
        if (!st.active && nowNs - st.startNs >= comboNs) {
            st.active = true;
            fired = true;
        }
    }
    if (st.active) {
        buttons = (buttons & ~(menuBit | triggerBit)) | systemBit;
        touches &= ~triggerBit;
        trigger = 0.0f;
    }
    else if (st.afterMask != 0) {
        st.afterMask &= buttons;  // 已經放開的鍵不再遮
        if (st.afterMask != 0) {
            buttons &= ~st.afterMask;
            if (st.afterMask & triggerBit) {
                touches &= ~triggerBit;
                trigger = 0.0f;
            }
        }
    }
    return fired;
}

}  // namespace xrcombo
