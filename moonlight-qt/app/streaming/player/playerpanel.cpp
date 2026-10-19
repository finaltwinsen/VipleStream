// VipleStream 2.0 §VR-PLAYER — 見 playerpanel.h。

#include "playerpanel.h"

#include <QFont>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace player {

namespace {

constexpr int kW = 1024;
constexpr int kH = 192;
constexpr int kMargin = 24;
// 時間軸：條本身在 y 58～68，可點的範圍放寬到 40～88
constexpr int kBarY = 58;
constexpr int kBarH = 10;
constexpr int kBarHitTop = 40;
constexpr int kBarHitBottom = 88;
// 按鈕列
constexpr int kBtnTop = 100;
constexpr int kBtnBottom = 180;
constexpr int kBtnGap = 6;

QRect buttonRect(int i)
{
    const int n = PlayerPanel::ButtonCount;
    const int w = (kW - 2 * kMargin - (n - 1) * kBtnGap) / n;
    return QRect(kMargin + i * (w + kBtnGap), kBtnTop, w, kBtnBottom - kBtnTop);
}

// 預乘 alpha 的顏色（Format_ARGB32_Premultiplied 上用 QPainter 畫一般顏色即可，這裡只是方便寫半透明）
QColor rgba(int r, int g, int b, int a)
{
    return QColor(r, g, b, a);
}

void drawIcon(QPainter& p, const QRect& r, int action, bool paused)
{
    // 播放／暫停用圖形畫（字型不一定有 ▶ ❚❚ 這些字元）
    const QPointF c = r.center();
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(240, 240, 240));
    if (action == PlayerPanel::PlayPause) {
        if (paused) {
            QPainterPath tri;
            tri.moveTo(c.x() - 12, c.y() - 16);
            tri.lineTo(c.x() + 16, c.y());
            tri.lineTo(c.x() - 12, c.y() + 16);
            tri.closeSubpath();
            p.drawPath(tri);
        }
        else {
            p.drawRect(QRectF(c.x() - 13, c.y() - 15, 9, 30));
            p.drawRect(QRectF(c.x() + 4, c.y() - 15, 9, 30));
        }
    }
}

}  // namespace

QString PlayerPanel::formatTime(double sec)
{
    if (!(sec >= 0.0)) {
        sec = 0.0;
    }
    const int t = static_cast<int>(sec);
    const int h = t / 3600, m = (t / 60) % 60, s = t % 60;
    return h > 0 ? QString::asprintf("%d:%02d:%02d", h, m, s) : QString::asprintf("%d:%02d", m, s);
}

QImage PlayerPanel::render(const State& s)
{
    QImage img(kW, kH, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    // 背景：深灰半透明圓角
    p.setPen(Qt::NoPen);
    p.setBrush(rgba(18, 18, 22, 215));
    p.drawRoundedRect(QRectF(0, 0, kW, kH), 22, 22);

    QFont f = p.font();
    f.setPixelSize(24);
    p.setFont(f);
    p.setPen(QColor(235, 235, 235));
    // 片名（左）與時間（右）
    const QString time = s.durationSec > 0.0 ? formatTime(s.positionSec) + " / " + formatTime(s.durationSec)
                                             : formatTime(s.positionSec);
    QString status;
    if (s.ended) {
        status = QStringLiteral("  (ended)");
    }
    else if (s.buffering) {
        status = QStringLiteral("  (buffering…)");
    }
    const QRect timeRect(kW - kMargin - 360, 10, 360, 36);
    p.drawText(timeRect, Qt::AlignRight | Qt::AlignVCenter, time + status);
    const QRect titleRect(kMargin, 10, kW - 2 * kMargin - 380, 36);
    p.drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
               p.fontMetrics().elidedText(s.title, Qt::ElideMiddle, titleRect.width()));

    // 時間軸
    const QRectF bar(kMargin, kBarY, kW - 2 * kMargin, kBarH);
    p.setPen(Qt::NoPen);
    p.setBrush(rgba(90, 90, 100, 230));
    p.drawRoundedRect(bar, kBarH / 2.0, kBarH / 2.0);
    if (s.durationSec > 0.0) {
        const double frac = std::clamp(s.positionSec / s.durationSec, 0.0, 1.0);
        p.setBrush(QColor(60, 150, 255));
        p.drawRoundedRect(QRectF(bar.x(), bar.y(), bar.width() * frac, bar.height()), kBarH / 2.0, kBarH / 2.0);
        p.setBrush(QColor(250, 250, 250));
        p.drawEllipse(QPointF(bar.x() + bar.width() * frac, bar.center().y()), 9, 9);
        if (s.hoverTimeline >= 0.0f && s.seekable) {
            const double hx = bar.x() + bar.width() * s.hoverTimeline;
            p.setBrush(rgba(255, 255, 255, 140));
            p.drawRect(QRectF(hx - 1.5, bar.y() - 8, 3, bar.height() + 16));
            f.setPixelSize(18);
            p.setFont(f);
            p.setPen(QColor(220, 220, 220));
            p.drawText(QRectF(hx - 60, bar.bottom() + 2, 120, 22), Qt::AlignCenter, formatTime(s.hoverTimeline * s.durationSec));
            f.setPixelSize(24);
            p.setFont(f);
        }
    }

    // 按鈕
    const QString labels[ButtonCount] = {
        QStringLiteral("−30s"), QStringLiteral("−10s"), QString(), QStringLiteral("+10s"), QStringLiteral("+30s"),
        QStringLiteral("Vol −"), QStringLiteral("Vol +"), s.formatLabel, s.screenLabel, QStringLiteral("Center"),
        QStringLiteral("Exit")};
    f.setPixelSize(22);
    p.setFont(f);
    for (int i = 0; i < ButtonCount; i++) {
        const QRect r = buttonRect(i);
        const bool hover = s.hover == i;
        const bool disabled = !s.seekable && i <= Fwd30 && i != PlayPause;
        p.setPen(Qt::NoPen);
        p.setBrush(hover ? QColor(70, 110, 190) : (i == Exit ? rgba(120, 40, 40, 230) : rgba(48, 48, 56, 230)));
        p.drawRoundedRect(QRectF(r), 12, 12);
        if (i == PlayPause) {
            drawIcon(p, r, i, s.paused);
            continue;
        }
        p.setPen(disabled ? QColor(120, 120, 120) : QColor(240, 240, 240));
        if (i == VolDown || i == VolUp) {
            // 音量數字放在兩顆音量鍵下緣
            p.drawText(r.adjusted(0, -14, 0, -14), Qt::AlignCenter, labels[i]);
            f.setPixelSize(16);
            p.setFont(f);
            p.setPen(QColor(180, 180, 180));
            p.drawText(r.adjusted(0, 30, 0, 0), Qt::AlignHCenter | Qt::AlignVCenter,
                       QString::number(static_cast<int>(std::lround(s.volume * 100.0f))) + QStringLiteral("%"));
            f.setPixelSize(22);
            p.setFont(f);
            continue;
        }
        p.drawText(r, Qt::AlignCenter | Qt::TextWordWrap, labels[i]);
    }
    p.end();
    return img;
}

int PlayerPanel::hitTest(float u, float v, float* seekFrac)
{
    const float x = u * kW, y = v * kH;
    if (y >= kBarHitTop && y <= kBarHitBottom && x >= kMargin - 10 && x <= kW - kMargin + 10) {
        *seekFrac = std::clamp((x - kMargin) / static_cast<float>(kW - 2 * kMargin), 0.0f, 1.0f);
        return Timeline;
    }
    for (int i = 0; i < ButtonCount; i++) {
        const QRect r = buttonRect(i);
        if (x >= r.left() && x <= r.right() && y >= r.top() && y <= r.bottom()) {
            return i;
        }
    }
    return None;
}

}  // namespace player
