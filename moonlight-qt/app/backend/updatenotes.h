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

// §REVIEW-1119：原本用 `.*\]\(#[^)]*\).*` 濾語言列。一行裡堆很多 `](#` 又沒有右括號時，
// 兩個 `.*` 會在介面執行緒上二次方回溯。分隔線與「](#…)」都改成線性掃描，結果與原本相同。
inline bool isReleaseChromeLine(const QString& line)
{
    if (line.size() >= 3) {
        bool dashes = true;
        for (int i = 0; i < line.size(); ++i) {
            if (line.at(i) != QLatin1Char('-')) {
                dashes = false;
                break;
            }
        }
        if (dashes) {
            return true;
        }
    }
    const int mark = line.indexOf(QLatin1String("](#"));
    return mark >= 0 && line.indexOf(QLatin1Char(')'), mark + 3) >= 0;
}

inline QString summarize(const QString& body, const QString& localeName = QString())
{
    static const QRegularExpression whatsNew(QStringLiteral("^#{2,4}\\s*(what'?s new|新功能|更新內容)"),
                                             QRegularExpression::CaseInsensitiveOption);
    // 正規的 release notes 遠小於這個上限；再長就先截斷，避免 split 與逐行掃描吃完整包。
    QString bounded = body;
    if (bounded.size() > 256 * 1024) {
        bounded.truncate(256 * 1024);
    }
    const QString text = pickLanguage(bounded, localeName);
    QStringList lines;
    const QStringList rawLines = text.split(QLatin1Char('\n'));
    for (const QString& l : rawLines) {
        if (!isReleaseChromeLine(l.trimmed())) {
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
        out = out.left(kMaxChars);
        if (!out.isEmpty() && out.at(out.size() - 1).isHighSurrogate()) {
            out.chop(1);  // §REVIEW-1111（第二輪）：不要切在代理對中間
        }
        out = out.trimmed() + QStringLiteral("\n\n…");
    }
    return out;
}

}  // namespace UpdateNotes
