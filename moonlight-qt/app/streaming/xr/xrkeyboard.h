// VipleStream 2.0 §VR M3a — XrKeyboard：β 的自繪虛擬鍵盤（版面、命中測試、貼圖繪製）。
// 設計：vr_architecture.md β 輸入列（「藍牙鍵盤或自繪鍵盤」）。
//
// 分工：
//   - XrKeyboard（本檔，純邏輯，不碰 OpenXR／Vulkan）：US QWERTY 五列版面（16 單位寬）、UV → 按鍵、
//     字元 → 按鍵（dev 自測用）、以 QImage＋QPainter 畫整塊貼圖（黏滯修飾鍵啟用時換色）。
//   - XrInput：鍵盤開關、射線命中（鍵盤優先於影像螢幕）、trigger 按下／放開 → 按鍵事件、黏滯修飾鍵。
//   - XrContext：擺放（影像螢幕下方、朝向使用者、後仰 30°）、貼圖上傳到 swapchain、hover／按下高亮 quad。
//
// 按鍵碼一律是 Win32 VK（LiSendKeyboardEvent 的約定，host 以 US 版面解讀）。
// 「中/英」鍵送 Shift 單擊：Windows 內建注音（台灣最常見的輸入法）預設以 Shift 切換中／英文模式，
// 也是中文版 Windows 螢幕小鍵盤「中/英」的語意；Win+Space 是切換「鍵盤配置／輸入語言」，
// 對只裝一個中文輸入法的 host 會在語言間循環，不是使用者按「中/英」時期待的行為。
//
// TODO(M3a)：Shift 雙擊鎖定（Caps）、按住連發以外的長按字元、數字鍵盤。

#pragma once

#include <QImage>
#include <QRectF>
#include <QString>

#include <cstdint>
#include <vector>

class XrKeyboard
{
public:
    enum class Kind { Normal, Shift, Ctrl, Alt, Win, Ime, Close };

    struct Key {
        QRectF rect;       // 貼圖像素座標
        QString label;     // 一般狀態
        QString shiftLabel;  // Shift 啟用時（空＝與 label 相同／字母轉大寫）
        int vk = 0;        // Win32 VK（Normal 與 Ime 用）
        Kind kind = Kind::Normal;
    };

    // 黏滯修飾鍵位元（與 Limelight MODIFIER_* 相同數值）
    static constexpr uint8_t kModShift = 0x01;
    static constexpr uint8_t kModCtrl = 0x02;
    static constexpr uint8_t kModAlt = 0x04;
    static constexpr uint8_t kModMeta = 0x08;

    XrKeyboard();

    int width() const { return m_Width; }
    int height() const { return m_Height; }
    float aspect() const { return static_cast<float>(m_Width) / static_cast<float>(m_Height); }

    int keyCount() const { return static_cast<int>(m_Keys.size()); }
    const Key& key(int idx) const { return m_Keys[static_cast<size_t>(idx)]; }

    // u 往右、v 往下（0..1）；沒命中按鍵回 -1
    int keyAt(float u, float v) const;
    // 按鍵中心與大小（UV）
    void keyUv(int idx, float* cu, float* cv, float* du, float* dv) const;
    // 字元 → 按鍵（dev 自測）；needShift＝要先按 Shift。找不到回 false
    bool keyForChar(QChar c, int* idx, bool* needShift) const;
    int findKind(Kind kind) const;

    // 整塊貼圖（Format_RGBA8888）；stickyMods 為目前黏滯的修飾鍵（啟用者換色、字母轉大寫）
    QImage render(uint8_t stickyMods) const;

    static uint8_t modBit(Kind kind);
    static int modVk(Kind kind);  // 修飾鍵送出時的 VK（左側鍵）

private:
    std::vector<Key> m_Keys;
    int m_Width = 0;
    int m_Height = 0;
};
