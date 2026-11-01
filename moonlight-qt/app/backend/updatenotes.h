#pragma once

// §UPDATE-NOTES（2026-11-01）：更新對話框的「更新摘要」。
//
// 來源是 GitHub release 的 body（Markdown，格式見 CLAUDE.md「Release notes 規範」）：
//   ## vX.Y.Z — 一句話主軸
//   開頭說明
//   ### Asset        ← 檔案表，使用者不需要
//   ### What's new   ← 要的
//   ### 已知限制／### Upgrade ← 略過（安裝步驟由更新器自己做）
// 摘要＝標題（改成粗體）＋開頭說明＋What's new 段。找不到 What's new 段就整篇。
// 只用 Qt Core，方便單獨編譯測試。

#include <QRegularExpression>
#include <QString>
#include <QStringList>

namespace UpdateNotes {

constexpr int kMaxChars = 8000;  // 對話框裡能捲動，但不要整本塞進來

inline QString summarize(const QString& body)
{
    static const QRegularExpression whatsNew(QStringLiteral("^#{2,4}\\s*(what'?s new|新功能|更新內容)"),
                                             QRegularExpression::CaseInsensitiveOption);
    const QString text = QString(body).replace(QLatin1String("\r\n"), QLatin1String("\n")).trimmed();
    const QStringList lines = text.split(QLatin1Char('\n'));

    QStringList intro, news;
    enum { Intro, News, Skip } part = Intro;
    for (const QString& raw : lines) {
        const QString line = raw.trimmed();
        if (line.startsWith(QLatin1String("## "))) {
            // release 標題（版號＋一句話主軸）改成粗體，不要大標題
            if (part == Intro && intro.isEmpty()) {
                intro << QStringLiteral("**%1**").arg(line.mid(3).trimmed()) << QString();
                continue;
            }
            part = Skip;
            continue;
        }
        if (line.startsWith(QLatin1Char('#'))) {
            part = whatsNew.match(line).hasMatch() ? News : Skip;
            continue;
        }
        if (part == Intro) {
            intro << raw;
        }
        else if (part == News) {
            news << raw;
        }
    }

    QString out = news.isEmpty()
        ? text
        : intro.join(QLatin1Char('\n')).trimmed() + QStringLiteral("\n\n") + news.join(QLatin1Char('\n')).trimmed();
    out = out.trimmed();
    if (out.size() > kMaxChars) {
        out = out.left(kMaxChars).trimmed() + QStringLiteral("\n\n…");
    }
    return out;
}

}  // namespace UpdateNotes
