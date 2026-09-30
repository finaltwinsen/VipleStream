// VipleStream 2.0 §VR M3a — XrKeyboard 實作。設計見 xrkeyboard.h。

#include "xrkeyboard.h"

#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>

namespace {

constexpr int kUnitPx = 78;    // 1 單位鍵寬（像素）
constexpr int kMarginPx = 16;  // 外框
constexpr int kGapPx = 6;      // 鍵與鍵的間隙
constexpr int kColumns = 16;   // 每列總寬（單位）
constexpr int kRows = 5;

// Win32 VK
constexpr int VK_BACK_ = 0x08, VK_TAB_ = 0x09, VK_RETURN_ = 0x0D, VK_ESCAPE_ = 0x1B, VK_SPACE_ = 0x20;
constexpr int VK_LEFT_ = 0x25, VK_UP_ = 0x26, VK_RIGHT_ = 0x27, VK_DOWN_ = 0x28, VK_DELETE_ = 0x2E;
constexpr int VK_LWIN_ = 0x5B, VK_LSHIFT_ = 0xA0, VK_LCONTROL_ = 0xA2, VK_LMENU_ = 0xA4;
constexpr int VK_OEM_1_ = 0xBA, VK_OEM_PLUS_ = 0xBB, VK_OEM_COMMA_ = 0xBC, VK_OEM_MINUS_ = 0xBD;
constexpr int VK_OEM_PERIOD_ = 0xBE, VK_OEM_2_ = 0xBF, VK_OEM_3_ = 0xC0, VK_OEM_4_ = 0xDB;
constexpr int VK_OEM_5_ = 0xDC, VK_OEM_6_ = 0xDD, VK_OEM_7_ = 0xDE;

struct Spec {
    float w;  // 單位
    const char* label;
    const char* shiftLabel;
    int vk;
    XrKeyboard::Kind kind;
};

using K = XrKeyboard::Kind;

}  // namespace

XrKeyboard::XrKeyboard()
{
    m_Width = kColumns * kUnitPx + 2 * kMarginPx;
    m_Height = kRows * kUnitPx + 2 * kMarginPx;

    // 每列總寬 16 單位
    const std::vector<std::vector<Spec>> rows = {
        {{1, "Esc", "", VK_ESCAPE_, K::Normal}, {1, "`", "~", VK_OEM_3_, K::Normal},
         {1, "1", "!", '1', K::Normal}, {1, "2", "@", '2', K::Normal}, {1, "3", "#", '3', K::Normal},
         {1, "4", "$", '4', K::Normal}, {1, "5", "%", '5', K::Normal}, {1, "6", "^", '6', K::Normal},
         {1, "7", "&", '7', K::Normal}, {1, "8", "*", '8', K::Normal}, {1, "9", "(", '9', K::Normal},
         {1, "0", ")", '0', K::Normal}, {1, "-", "_", VK_OEM_MINUS_, K::Normal},
         {1, "=", "+", VK_OEM_PLUS_, K::Normal}, {2, "Bksp", "", VK_BACK_, K::Normal}},
        {{1.5f, "Tab", "", VK_TAB_, K::Normal}, {1, "q", "", 'Q', K::Normal}, {1, "w", "", 'W', K::Normal},
         {1, "e", "", 'E', K::Normal}, {1, "r", "", 'R', K::Normal}, {1, "t", "", 'T', K::Normal},
         {1, "y", "", 'Y', K::Normal}, {1, "u", "", 'U', K::Normal}, {1, "i", "", 'I', K::Normal},
         {1, "o", "", 'O', K::Normal}, {1, "p", "", 'P', K::Normal}, {1, "[", "{", VK_OEM_4_, K::Normal},
         {1, "]", "}", VK_OEM_6_, K::Normal}, {1.5f, "\\", "|", VK_OEM_5_, K::Normal},
         {1, "Del", "", VK_DELETE_, K::Normal}},
        {{1.75f, "中/英", "", VK_LSHIFT_, K::Ime}, {1, "a", "", 'A', K::Normal},
         {1, "s", "", 'S', K::Normal}, {1, "d", "", 'D', K::Normal}, {1, "f", "", 'F', K::Normal},
         {1, "g", "", 'G', K::Normal}, {1, "h", "", 'H', K::Normal}, {1, "j", "", 'J', K::Normal},
         {1, "k", "", 'K', K::Normal}, {1, "l", "", 'L', K::Normal}, {1, ";", ":", VK_OEM_1_, K::Normal},
         {1, "'", "\"", VK_OEM_7_, K::Normal}, {3.25f, "Enter", "", VK_RETURN_, K::Normal}},
        {{2.25f, "Shift", "", 0, K::Shift}, {1, "z", "", 'Z', K::Normal}, {1, "x", "", 'X', K::Normal},
         {1, "c", "", 'C', K::Normal}, {1, "v", "", 'V', K::Normal}, {1, "b", "", 'B', K::Normal},
         {1, "n", "", 'N', K::Normal}, {1, "m", "", 'M', K::Normal}, {1, ",", "<", VK_OEM_COMMA_, K::Normal},
         {1, ".", ">", VK_OEM_PERIOD_, K::Normal}, {1, "/", "?", VK_OEM_2_, K::Normal},
         {3.75f, "Shift", "", 0, K::Shift}},
        {{1.5f, "Ctrl", "", 0, K::Ctrl}, {1.25f, "Win", "", 0, K::Win}, {1.25f, "Alt", "", 0, K::Alt},
         {5.5f, "Space", "", VK_SPACE_, K::Normal}, {1.25f, "Alt", "", 0, K::Alt},
         {1, "←", "", VK_LEFT_, K::Normal}, {1, "↑", "", VK_UP_, K::Normal},
         {1, "↓", "", VK_DOWN_, K::Normal}, {1, "→", "", VK_RIGHT_, K::Normal},
         {1.25f, "✕", "", 0, K::Close}},
    };

    for (int r = 0; r < kRows; r++) {
        float x = 0.0f;
        for (const Spec& s : rows[static_cast<size_t>(r)]) {
            Key k;
            k.rect = QRectF(kMarginPx + x * kUnitPx + kGapPx * 0.5, kMarginPx + r * kUnitPx + kGapPx * 0.5,
                            s.w * kUnitPx - kGapPx, kUnitPx - kGapPx);
            k.label = QString::fromUtf8(s.label);
            k.shiftLabel = QString::fromUtf8(s.shiftLabel);
            k.vk = s.vk;
            k.kind = s.kind;
            m_Keys.push_back(k);
            x += s.w;
        }
    }
}

int XrKeyboard::keyAt(float u, float v) const
{
    const QPointF p(u * m_Width, v * m_Height);
    for (int i = 0; i < keyCount(); i++) {
        // 含半個間隙：鍵與鍵之間不留死區
        if (m_Keys[static_cast<size_t>(i)].rect.adjusted(-kGapPx * 0.5, -kGapPx * 0.5, kGapPx * 0.5, kGapPx * 0.5).contains(p)) {
            return i;
        }
    }
    return -1;
}

void XrKeyboard::keyUv(int idx, float* cu, float* cv, float* du, float* dv) const
{
    const QRectF& r = m_Keys[static_cast<size_t>(idx)].rect;
    *cu = static_cast<float>(r.center().x() / m_Width);
    *cv = static_cast<float>(r.center().y() / m_Height);
    *du = static_cast<float>(r.width() / m_Width);
    *dv = static_cast<float>(r.height() / m_Height);
}

bool XrKeyboard::keyForChar(QChar c, int* idx, bool* needShift) const
{
    if (c == QLatin1Char(' ')) {
        for (int i = 0; i < keyCount(); i++) {
            if (m_Keys[static_cast<size_t>(i)].vk == VK_SPACE_ && m_Keys[static_cast<size_t>(i)].kind == K::Normal) {
                *idx = i;
                *needShift = false;
                return true;
            }
        }
    }
    for (int i = 0; i < keyCount(); i++) {
        const Key& k = m_Keys[static_cast<size_t>(i)];
        if (k.kind != K::Normal || k.label.size() != 1) {
            continue;
        }
        if (k.label.at(0) == c) {
            *idx = i;
            *needShift = false;
            return true;
        }
        if (k.label.at(0).isLetter() && k.label.at(0).toUpper() == c) {
            *idx = i;
            *needShift = true;
            return true;
        }
        if (k.shiftLabel.size() == 1 && k.shiftLabel.at(0) == c) {
            *idx = i;
            *needShift = true;
            return true;
        }
    }
    return false;
}

int XrKeyboard::findKind(Kind kind) const
{
    for (int i = 0; i < keyCount(); i++) {
        if (m_Keys[static_cast<size_t>(i)].kind == kind) {
            return i;
        }
    }
    return -1;
}

uint8_t XrKeyboard::modBit(Kind kind)
{
    switch (kind) {
    case K::Shift: return kModShift;
    case K::Ctrl: return kModCtrl;
    case K::Alt: return kModAlt;
    case K::Win: return kModMeta;
    default: return 0;
    }
}

int XrKeyboard::modVk(Kind kind)
{
    switch (kind) {
    case K::Shift: return VK_LSHIFT_;
    case K::Ctrl: return VK_LCONTROL_;
    case K::Alt: return VK_LMENU_;
    case K::Win: return VK_LWIN_;
    default: return 0;
    }
}

QImage XrKeyboard::render(uint8_t stickyMods) const
{
    QImage img(m_Width, m_Height, QImage::Format_RGBA8888);
    img.fill(QColor(24, 26, 30));
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    QFont font;  // 系統預設（Windows：微軟正黑體等；Linux：fontconfig 的 sans）
    font.setPixelSize(28);
    QFont small = font;
    small.setPixelSize(18);
    const bool haveCjk = QFontMetrics(font).inFont(QChar(0x4E2D));
    const bool shift = (stickyMods & kModShift) != 0;

    for (const Key& k : m_Keys) {
        const bool modActive = modBit(k.kind) != 0 && (stickyMods & modBit(k.kind)) != 0;
        QColor fill(58, 61, 68);
        if (k.kind == K::Close) {
            fill = QColor(110, 40, 40);
        }
        else if (k.kind != K::Normal) {
            fill = QColor(46, 49, 56);
        }
        if (modActive) {
            fill = QColor(45, 108, 223);
        }
        QPainterPath path;
        path.addRoundedRect(k.rect, 8, 8);
        p.fillPath(path, fill);

        QString label = k.label;
        if (k.kind == K::Ime && !haveCjk) {
            label = QStringLiteral("IME");
        }
        if (k.kind == K::Normal && k.label.size() == 1 && k.label.at(0).isLetter() && shift) {
            label = k.label.toUpper();
        }
        p.setPen(QColor(235, 237, 240));
        if (k.kind == K::Normal && !k.shiftLabel.isEmpty()) {
            // 符號鍵：Shift 字元小字在上、一般字元在下；Shift 啟用時對調強調
            p.setFont(small);
            p.setPen(shift ? QColor(235, 237, 240) : QColor(150, 155, 165));
            p.drawText(k.rect.adjusted(8, 4, -8, -k.rect.height() / 2), Qt::AlignLeft | Qt::AlignTop, k.shiftLabel);
            p.setFont(font);
            p.setPen(shift ? QColor(150, 155, 165) : QColor(235, 237, 240));
            p.drawText(k.rect.adjusted(8, k.rect.height() / 3, -8, -4), Qt::AlignCenter, label);
        }
        else {
            p.setFont(k.label.size() > 2 ? small : font);
            p.drawText(k.rect, Qt::AlignCenter, label);
        }
    }
    p.end();
    return img;
}
