// VipleStream 2.0 §VR M3a — XrKeyboard：β 的自繪虛擬鍵盤（版面、命中測試、貼圖繪製）。
// 設計：vr_architecture.md β 輸入列（「藍牙鍵盤或自繪鍵盤」）。
//
// 分工：
//   - XrKeyboard（本檔，純邏輯，不碰 OpenXR／Vulkan）：US QWERTY 五列版面（16 單位寬）、UV → 按鍵、
//     字元 → 按鍵（dev 自測用）、以 QImage＋QPainter 畫整塊貼圖（黏滯修飾鍵啟用時換色）。
//     版面有兩頁：第 0 頁＝QWERTY，第 1 頁＝數字鍵盤（「123」／「ABC」鍵切換，兩頁上這顆鍵在同一個位置）。
//   - XrInput：鍵盤開關、射線命中（鍵盤優先於影像螢幕）、trigger 按下／放開 → 按鍵事件、黏滯修飾鍵。
//   - XrContext：擺放（影像螢幕下方、朝向使用者、後仰 30°）、貼圖上傳到 swapchain、hover／按下高亮 quad。
//
// 按鍵碼一律是 Win32 VK（LiSendKeyboardEvent 的約定，host 以 US 版面解讀）。
// 「中/英」鍵（K::Ime）送 VK_LSHIFT 的按下與放開，兩個事件的 flags 都帶 VIPLE_KBE_FLAG_IME_TOGGLE（0x80）。
// 新的 Windows host 看到這個位元，就讀前景視窗的輸入法狀態來切換中／英（不靠快速鍵）；切成功就把這組按鍵吞掉。
// 舊 host 不認得這個位元，照舊注入左 Shift 單擊，行為與改之前相同（注音預設也是用 Shift 切，但使用者可以改到別的鍵）。
// Win+Space 是切換鍵盤配置／輸入語言，不是這個鍵要做的事。
//
// §KBD-CAPS（2026-11-20）：Caps 是鍵盤自己的鎖定狀態，不送 VK_CAPITAL。理由：(1) client 讀不到 host 的 Caps 狀態，
// 送切換鍵的話鍵面的燈號和 host 可能從一開始就相反，而且串流結束後 host 會留在大寫鎖定；(2) Shift 也是同一種做法
// （黏滯狀態留在本地，按一般鍵時才在它前後送 Shift 的按下／放開）。規則照實體鍵盤：只影響字母、和 Shift 互相抵銷
// （Caps＋Shift＝小寫），符號與數字只看 Shift；有 Ctrl／Alt／Win 時不加 Shift（Ctrl+C 不能變成 Ctrl+Shift+C）。
// 關掉鍵盤時 Caps 與頁面都回到預設。
// §KBD-NUMPAD（2026-11-20）：數字鍵盤頁送 VK_NUMPAD0～9、VK_MULTIPLY／ADD／SUBTRACT／DECIMAL／DIVIDE、VK_NUMLOCK
// （和上排數字的 '0'～'9' 是不同的鍵碼）。數字鍵盤的 Enter 送 VK_RETURN：這個協定只帶 VK，沒有 extended 旗標可以區分。
// host 的 NumLock 關著時這些鍵會變成方向／Home／End（同實體鍵盤），所以頁面上留了 NumLk。
//
// TODO(M3a)：按住連發以外的長按字元。

#pragma once

#include <QImage>
#include <QRectF>
#include <QString>

#include <cstdint>
#include <vector>

class XrKeyboard
{
public:
    enum class Kind { Normal, Shift, Ctrl, Alt, Win, Ime, Close, Caps, Page };

    struct Key {
        QRectF rect;       // 貼圖像素座標
        QString label;     // 一般狀態
        QString shiftLabel;  // Shift 啟用時（空＝與 label 相同／字母轉大寫）
        int vk = 0;        // Win32 VK（Normal 與 Ime 用）
        Kind kind = Kind::Normal;
        int page = 0;      // 0＝QWERTY、1＝數字鍵盤
    };

    // 黏滯修飾鍵位元（與 Limelight MODIFIER_* 相同數值）
    static constexpr uint8_t kModShift = 0x01;
    static constexpr uint8_t kModCtrl = 0x02;
    static constexpr uint8_t kModAlt = 0x04;
    static constexpr uint8_t kModMeta = 0x08;
    // 鎖定狀態位元（只在本地，不送給 host）；和黏滯修飾鍵合成一個位元組＝render() 與貼圖快取的索引
    static constexpr uint8_t kStateCaps = 0x10;
    static constexpr uint8_t kStatePage = 0x20;  // 設了＝顯示數字鍵盤頁
    static constexpr int kStateCount = 0x40;     // 狀態位元組的組合數
    static int pageOf(uint8_t state) { return (state & kStatePage) ? 1 : 0; }

    XrKeyboard();

    int width() const { return m_Width; }
    int height() const { return m_Height; }
    float aspect() const { return static_cast<float>(m_Width) / static_cast<float>(m_Height); }

    int keyCount() const { return static_cast<int>(m_Keys.size()); }
    const Key& key(int idx) const { return m_Keys[static_cast<size_t>(idx)]; }

    // u 往右、v 往下（0..1）；只看 page 那一頁的鍵；沒命中按鍵回 -1
    int keyAt(float u, float v, int page) const;
    // 按鍵中心與大小（UV）
    void keyUv(int idx, float* cu, float* cv, float* du, float* dv) const;
    // 字元 → 按鍵（dev 自測）；只找 page 那一頁；needShift＝要先按 Shift。找不到回 false
    bool keyForChar(QChar c, int* idx, bool* needShift, int page = 0) const;
    int findKind(Kind kind, int page = 0) const;

    // 整塊貼圖（Format_RGBA8888）；state＝黏滯修飾鍵（kMod*）｜鎖定狀態（kState*）：啟用者換色、字母依
    // Shift 與 Caps 轉大寫、kStatePage 決定畫哪一頁
    QImage render(uint8_t state) const;

    static uint8_t modBit(Kind kind);
    static uint8_t lockBit(Kind kind);  // Caps／Page 的鎖定位元（其餘回 0）
    // 按下一般鍵時實際要送的修飾鍵：黏滯修飾鍵，加上 Caps 對字母的作用（規則見檔頭 §KBD-CAPS）
    static uint8_t effectiveMods(const Key& key, uint8_t stickyMods, uint8_t locks);
    static int modVk(Kind kind);  // 修飾鍵送出時的 VK（左側鍵）

private:
    std::vector<Key> m_Keys;
    int m_Width = 0;
    int m_Height = 0;
};
