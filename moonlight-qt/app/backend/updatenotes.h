#pragma once

// §UPDATE-NOTES（2026-11-01）：更新對話框的「更新摘要」。
//
// 來源是 GitHub release 的 body（Markdown，格式見 CLAUDE.md「Release notes 規範」）。
// 2.0.123 起 release notes 是雙語（排法同 README.md）：英文在前，「# 繁體中文」這一行之後是中文，
// 兩段各自是下面這個結構：
//   ## vX.Y.Z — 一句話主軸
//   開頭說明
//   ### Assets        ← 檔案表，使用者不需要
//   ### What's new    ← 要的
//   ### Known limitations／### Upgrade ← 略過（安裝步驟由更新器自己做）
// 摘要＝標題（改成粗體）＋開頭說明＋What's new 段。找不到 What's new 段就整段。
// 選哪一段看介面語系：zh 開頭（zh_TW、zh_CN、zh_HK…）用中文段，其餘用英文段；
// 沒有「# 繁體中文」分隔行的舊 release（只有中文）就整篇當一段，任何語系都顯示它。
// 只用 Qt Core，方便單獨編譯測試。

#include <QRegularExpression>
#include <QString>
#include <QStringList>

namespace UpdateNotes {

constexpr int kMaxChars = 8000;  // 對話框裡能捲動，但不要整本塞進來

// 從雙語 body 取出這個語系的那一段。localeName 是 QLocale 的名字（"zh_TW"、"en_US"、"de"…）。
inline QString pickLanguage(const QString& body, const QString& localeName)
{
    // 分隔行：單獨一行的一級標題「# 繁體中文」（README.md 也是這樣分）
    static const QRegularExpression zhHeading(QStringLiteral("^#\\s*繁體中文\\s*$"),
                                              QRegularExpression::MultilineOption);
    const QString text = QString(body).replace(QLatin1String("\r\n"), QLatin1String("\n"));
    const QRegularExpressionMatch m = zhHeading.match(text);
    if (!m.hasMatch()) {
        return text.trimmed();
    }
    const QString en = text.left(m.capturedStart()).trimmed();
    const QString zh = text.mid(m.capturedEnd()).trimmed();
    const bool wantZh = localeName.startsWith(QLatin1String("zh"), Qt::CaseInsensitive);
    // 那一段是空的（例如只寫了一種語言）就退回另一段，不要給空白
    if (wantZh) {
        return zh.isEmpty() ? en : zh;
    }
    return en.isEmpty() ? zh : en;
}

inline QString summarize(const QString& body, const QString& localeName = QString())
{
    static const QRegularExpression whatsNew(QStringLiteral("^#{2,4}\\s*(what'?s new|新功能|更新內容)"),
                                             QRegularExpression::CaseInsensitiveOption);
    // 語言切換列（「**English** · [繁體中文](#…)」）與分隔線不是內容
    static const QRegularExpression chrome(QStringLiteral("^(-{3,}|.*\\]\\(#[^)]*\\).*)$"));
    const QString text = pickLanguage(body, localeName);
    QStringList lines;
    const QStringList rawLines = text.split(QLatin1Char('\n'));
    for (const QString& l : rawLines) {
        if (!chrome.match(l.trimmed()).hasMatch()) {
            lines << l;
        }
    }

    QStringList intro, news;
    enum { Intro, News, Skip } part = Intro;
    for (const QString& raw : lines) {
        const QString line = raw.trimmed();
        if (line.startsWith(QLatin1String("## "))) {
            // release 標題（版號＋一句話主軸）改成粗體，不要大標題
            if (part == Intro && intro.join(QString()).trimmed().isEmpty()) {
                intro.clear();
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
        ? lines.join(QLatin1Char('\n'))
        : intro.join(QLatin1Char('\n')).trimmed() + QStringLiteral("\n\n") + news.join(QLatin1Char('\n')).trimmed();
    out = out.trimmed();
    if (out.size() > kMaxChars) {
        out = out.left(kMaxChars).trimmed() + QStringLiteral("\n\n…");
    }
    return out;
}

}  // namespace UpdateNotes
