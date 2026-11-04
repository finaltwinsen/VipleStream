// VipleStream 2.0 §VR-PLAYER — VR 裡的播放控制列：畫成一張 QImage（XrContext::kPanelW×kPanelH），以及「射線指在哪裡＝哪個按鈕」。
//
// 版面（1024×192）：上半是片名、時間與時間軸（點時間軸＝跳到那裡）；下半一排按鈕：
//   −30s、−10s、播放／暫停、+10s、+30s、音量−、音量+、畫面格式、螢幕大小、畫質、字幕、重新置中、離開。
// 畫質與字幕只有網站影片（YouTube）才能用，其他來源顯示成灰色。
// 背景半透明（預乘 alpha），指到的按鈕變亮。QPainter 畫 QImage 可以在任何執行緒（Qt 文件允許），呼叫端在播放器的執行緒畫。

#pragma once

#include <QImage>
#include <QString>

namespace player {

class PlayerPanel
{
public:
    enum Action {
        None = -1,
        Back30 = 0,
        Back10,
        PlayPause,
        Fwd10,
        Fwd30,
        VolDown,
        VolUp,
        Format,
        Screen,
        Quality,
        Subtitles,
        Recenter,
        Exit,
        ButtonCount,
        Timeline = 100,  // 時間軸（seekFrac＝0..1）
    };

    struct State {
        QString title;
        double positionSec = 0.0;
        double durationSec = 0.0;  // 0＝未知（直播）
        bool seekable = true;
        bool paused = false;
        bool buffering = false;
        bool ended = false;
        float volume = 1.0f;
        QString formatLabel;   // 例如 "2D"、"3D SBS"、"360°"
        QString screenLabel;   // 例如 "Screen M"
        QString qualityLabel;  // 例如 "1080p60"；空＝不能換（灰色）
        QString subtitleLabel; // 例如 "Off"、"zh-Hant"；空＝沒有字幕（灰色）
        int hover = None;      // 指到的按鈕（Action）
        float hoverTimeline = -1.0f;  // 指在時間軸上的位置（0..1），沒有＝-1
    };

    static QImage render(const State& s);
    // 字幕：透明底、文字置中靠下、後面一塊半透明黑底（最多 3 行，太長的換行、再多的截掉）。w×h＝XrContext::kSubW×kSubH
    static QImage renderSubtitle(const QString& text, int w, int h);
    // u、v＝控制列上的位置（0..1，左上原點）。回傳按鈕；時間軸回 Timeline 並給 *seekFrac
    static int hitTest(float u, float v, float* seekFrac);
    static QString formatTime(double sec);
};

}  // namespace player
