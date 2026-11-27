#include "logredactor.h"

#include <QRegularExpression>

#include <algorithm>
#include <vector>

namespace {

bool isDigit(QChar c)
{
    const ushort u = c.unicode();
    return u >= '0' && u <= '9';
}

bool isHexLetter(QChar c)
{
    const ushort u = c.unicode();
    return (u >= 'a' && u <= 'f') || (u >= 'A' && u <= 'F');
}

bool isHex(QChar c)
{
    return isDigit(c) || isHexLetter(c);
}

bool isIdent(QChar c)
{
    const ushort u = c.unicode();
    return (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || isDigit(c) || u == '_';
}

bool isScopeChar(QChar c)
{
    const ushort u = c.unicode();
    return isIdent(c) || u == '.' || u == '-' || u == '~';
}

// §LOGDROP：前後還黏著數字或點時，多半是版號，不是一組 IP。
int matchIpv4(const QString& text, int start, bool checkBefore)
{
    const int n = text.size();
    if (start >= n || !isDigit(text.at(start))) {
        return -1;
    }
    if (checkBefore && start > 0) {
        const QChar prev = text.at(start - 1);
        if (isDigit(prev) || prev == QLatin1Char('.')) {
            return -1;
        }
    }
    int i = start;
    for (int group = 0; group < 4; ++group) {
        if (i >= n || !isDigit(text.at(i))) {
            return -1;
        }
        int value = 0;
        int digits = 0;
        while (i < n && isDigit(text.at(i))) {
            if (digits == 3) {
                return -1;
            }
            value = value * 10 + (text.at(i).unicode() - '0');
            ++digits;
            ++i;
        }
        if (digits == 0 || value > 255) {
            return -1;
        }
        if (group < 3) {
            if (i >= n || text.at(i) != QLatin1Char('.')) {
                return -1;
            }
            ++i;
        }
    }
    if (i < n) {
        const QChar next = text.at(i);
        if (isDigit(next) || next == QLatin1Char('.')) {
            return -1;
        }
    }
    return i;
}

QString maskIpv4(const QString& token)
{
    QStringList parts;
    int piece = 0;
    for (int i = 0; i <= token.size(); ++i) {
        if (i == token.size() || token.at(i) == QLatin1Char('.')) {
            parts.append(token.mid(piece, i - piece));
            piece = i + 1;
        }
    }
    if (parts.size() != 4) {
        return QStringLiteral("<ip>");
    }
    int value[4];
    for (int i = 0; i < 4; ++i) {
        bool ok = false;
        value[i] = parts.at(i).toInt(&ok);
        if (!ok) {
            return QStringLiteral("<ip>");
        }
    }
    if (value[0] == 127 || (value[0] == 0 && value[1] == 0 && value[2] == 0 && value[3] == 0)) {
        return token;
    }
    const bool local = value[0] == 10
            || (value[0] == 172 && value[1] >= 16 && value[1] <= 31)
            || (value[0] == 192 && value[1] == 168)
            || (value[0] == 100 && value[1] >= 64 && value[1] <= 127)
            || (value[0] == 169 && value[1] == 254);
    if (local) {
        return parts.at(0) + QLatin1Char('.') + parts.at(1) + QStringLiteral(".x.x");
    }
    return QStringLiteral("<ip>");
}

int macEnd(const QString& text, int start)
{
    const int n = text.size();
    if (start + 17 > n || !isHex(text.at(start))) {
        return -1;
    }
    const QChar sep = text.at(start + 2);
    if (sep != QLatin1Char(':') && sep != QLatin1Char('-')) {
        return -1;
    }
    if (start > 0) {
        const QChar prev = text.at(start - 1);
        if (isHex(prev) || prev == sep) {
            return -1;
        }
    }
    for (int group = 0; group < 6; ++group) {
        const int at = start + group * 3;
        if (!isHex(text.at(at)) || !isHex(text.at(at + 1))) {
            return -1;
        }
        if (group < 5 && text.at(at + 2) != sep) {
            return -1;
        }
    }
    const int end = start + 17;
    if (end < n) {
        const QChar next = text.at(end);
        if (isHex(next) || next == sep) {
            return -1;
        }
    }
    return end;
}

QString maskMac(const QString& token)
{
    const QChar sep = token.at(2);
    return token.left(8) + sep + QStringLiteral("xx") + sep + QStringLiteral("xx") + sep + QStringLiteral("xx");
}

bool ipv6TailBoundary(const QString& text, int end)
{
    if (end >= text.size()) {
        return true;
    }
    const QChar after = text.at(end);
    if (after == QLatin1Char('%')) {
        return true;
    }
    if (after == QLatin1Char('.')) {
        return end + 1 >= text.size() || !isDigit(text.at(end + 1));
    }
    return !isHex(after) && after != QLatin1Char(':');
}

// §LOGDROP：logcat 的 12:34:56 沒有 a–f，也沒有 ::。再擋一次純時間。
bool isClockTime(const QString& token)
{
    const int n = token.size();
    int i = 0;
    int digits = 0;
    while (i < n && digits < 2 && isDigit(token.at(i))) {
        ++digits;
        ++i;
    }
    if (digits < 1 || i >= n || token.at(i) != QLatin1Char(':')) {
        return false;
    }
    ++i;
    digits = 0;
    while (i < n && digits < 2 && isDigit(token.at(i))) {
        ++digits;
        ++i;
    }
    if (digits != 2 || i >= n || token.at(i) != QLatin1Char(':')) {
        return false;
    }
    ++i;
    digits = 0;
    while (i < n && digits < 2 && isDigit(token.at(i))) {
        ++digits;
        ++i;
    }
    return digits == 2 && i == n;
}

int ipv6End(const QString& text, int start)
{
    const int n = text.size();
    if (start >= n) {
        return -1;
    }
    // §LOGDROP：Class::method 這種堆疊也有兩個冒號。緊貼識別字時不是 IPv6。
    if (start > 0) {
        const QChar prev = text.at(start - 1);
        if (isIdent(prev) || prev == QLatin1Char(':')) {
            return -1;
        }
    }
    int i = start;
    int colons = 0;
    int groups = 0;
    bool compressed = false;
    bool hexLetter = false;
    const QChar first = text.at(i);
    if (first == QLatin1Char(':')) {
        if (i + 1 >= n || text.at(i + 1) != QLatin1Char(':')) {
            return -1;
        }
        compressed = true;
        colons = 2;
        i += 2;
        if (i < n && text.at(i) == QLatin1Char(':')) {
            return -1;
        }
    }
    else if (!isHex(first)) {
        return -1;
    }

    if (i < n && text.at(i) != QLatin1Char('%')) {
        while (i < n) {
            if (isDigit(text.at(i)) && colons > 0 && i > start && text.at(i - 1) == QLatin1Char(':')) {
                const int v4 = matchIpv4(text, i, false);
                if (v4 > i && ipv6TailBoundary(text, v4)) {
                    groups += 2;
                    i = v4;
                    break;
                }
            }
            int digits = 0;
            bool letter = false;
            while (i < n && digits < 4 && isHex(text.at(i))) {
                if (isHexLetter(text.at(i))) {
                    letter = true;
                }
                ++digits;
                ++i;
            }
            if (digits == 0) {
                if (compressed && i > start && text.at(i - 1) == QLatin1Char(':')) {
                    break;
                }
                return -1;
            }
            if (i < n && isHex(text.at(i))) {
                return -1;
            }
            if (letter) {
                hexLetter = true;
            }
            ++groups;
            if (i < n && text.at(i) == QLatin1Char(':')) {
                if (i + 1 < n && text.at(i + 1) == QLatin1Char(':')) {
                    if (compressed) {
                        return -1;
                    }
                    compressed = true;
                    colons += 2;
                    i += 2;
                    if (i < n && text.at(i) == QLatin1Char(':')) {
                        return -1;
                    }
                    if (i >= n) {
                        break;
                    }
                    continue;
                }
                if (i + 1 >= n) {
                    return -1;
                }
                ++colons;
                ++i;
                continue;
            }
            break;
        }
    }

    if (colons < 2) {
        return -1;
    }
    // §LOGDROP：不滿八組、又沒有 :: 的十六進位不是 IPv6。八組但全是數字也不是要遮的位址。
    if (!compressed) {
        if (groups != 8 || !hexLetter) {
            return -1;
        }
    }
    else if (groups > 7) {
        return -1;
    }
    if (isClockTime(text.mid(start, i - start))) {
        return -1;
    }
    if (i < n && text.at(i) == QLatin1Char('%')) {
        const int scope = i + 1;
        int j = scope;
        while (j < n && isScopeChar(text.at(j))) {
            ++j;
        }
        if (j == scope) {
            return -1;
        }
        i = j;
    }
    if (i < n) {
        const QChar next = text.at(i);
        if (isHex(next) || next == QLatin1Char(':')) {
            return -1;
        }
        // §LOGDROP：句點可以當結尾。點後面還有數字就是更長的點分字串，不要從中間切。
        if (next == QLatin1Char('.') && i + 1 < n && isDigit(text.at(i + 1))) {
            return -1;
        }
    }
    if (i <= start) {
        return -1;
    }
    return i;
}

QString ipv6Replacement(const QString& token)
{
    const int scope = token.indexOf(QLatin1Char('%'));
    const QString address = scope < 0 ? token : token.left(scope);
    // §LOGDROP：::1 是本機迴圈，不是外洩位址。scope 只標介面，一樣留著。
    if (address == QLatin1String("::1")) {
        return token;
    }
    return QStringLiteral("<ipv6>");
}

QString redactStructured(const QString& text)
{
    QString out;
    out.reserve(text.size());
    int i = 0;
    const int n = text.size();
    while (i < n) {
        const QChar c = text.at(i);
        if (isHex(c)) {
            const int mac = macEnd(text, i);
            if (mac > i) {
                out.append(maskMac(text.mid(i, mac - i)));
                i = mac;
                continue;
            }
            const int v6 = ipv6End(text, i);
            if (v6 > i) {
                out.append(ipv6Replacement(text.mid(i, v6 - i)));
                i = v6;
                continue;
            }
            if (isDigit(c)) {
                const int v4 = matchIpv4(text, i, true);
                if (v4 > i) {
                    out.append(maskIpv4(text.mid(i, v4 - i)));
                    i = v4;
                    continue;
                }
            }
        }
        else if (c == QLatin1Char(':')) {
            const int v6 = ipv6End(text, i);
            if (v6 > i) {
                out.append(ipv6Replacement(text.mid(i, v6 - i)));
                i = v6;
                continue;
            }
        }
        out.append(c);
        ++i;
    }
    return out;
}

QString redactEmails(const QString& text)
{
    const QRegularExpression email(
            QStringLiteral("(?<![A-Z0-9._%+-])[A-Z0-9._%+-]+@[A-Z0-9-]+(?:\\.[A-Z0-9-]+)*\\.[A-Z]{2,}(?![A-Z0-9])"),
            QRegularExpression::CaseInsensitiveOption);
    QString out;
    out.reserve(text.size());
    int cursor = 0;
    QRegularExpressionMatchIterator it = email.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        out.append(text.mid(cursor, match.capturedStart() - cursor));
        out.append(QStringLiteral("<email>"));
        cursor = match.capturedEnd();
    }
    out.append(text.mid(cursor));
    return out;
}

bool regionEqualsIgnoreCase(const QString& text, int at, const QString& needle)
{
    const int n = needle.size();
    for (int k = 0; k < n; ++k) {
        if (text.at(at + k).toLower() != needle.at(k).toLower()) {
            return false;
        }
    }
    return true;
}

int indexOfIgnoreCase(const QString& text, const QString& needle, int from)
{
    const int last = text.size() - needle.size();
    for (int i = from; i <= last; ++i) {
        if (regionEqualsIgnoreCase(text, i, needle)) {
            return i;
        }
    }
    return -1;
}

bool rangeTaken(const std::vector<char>& taken, int start, int end)
{
    for (int i = start; i < end; ++i) {
        if (taken[static_cast<size_t>(i)]) {
            return true;
        }
    }
    return false;
}

QString redactSecrets(const QString& text, const QStringList& extraSecrets)
{
    if (extraSecrets.isEmpty() || text.isEmpty()) {
        return text;
    }
    QStringList secrets;
    secrets.reserve(extraSecrets.size());
    for (const QString& secret : extraSecrets) {
        // §LOGDROP：空字串不遮。§REVIEW-1127 qt_ui #13：原本短於 3 的整個略過，1～2 字元的電腦名稱、使用者名稱
        // 會原樣上傳；現在照收，但只在整詞（前後不是字母或數字）時才遮，不會把內文打散（見下面）
        if (!secret.isEmpty()) {
            secrets.append(secret);
        }
    }
    if (secrets.isEmpty()) {
        return text;
    }
    // §LOGDROP：短名稱先換的話，長名稱會被切碎，電腦名稱就留在 log 裡。
    std::stable_sort(secrets.begin(), secrets.end(), [](const QString& left, const QString& right) {
        return left.size() > right.size();
    });
    const int n = text.size();
    std::vector<char> taken(static_cast<size_t>(n), 0);
    std::vector<int> endAt(static_cast<size_t>(n), -1);
    std::vector<int> starts;
    for (const QString& secret : secrets) {
        int from = 0;
        while (from <= n - secret.size()) {
            const int idx = indexOfIgnoreCase(text, secret, from);
            if (idx < 0) {
                break;
            }
            const int end = idx + secret.size();
            // §REVIEW-1127 qt_ui #13：1～2 字元的只遮整詞（例如名稱 "pc" 不遮 "pcm" 或 "upc" 裡的 "pc"）
            const bool shortSecret = secret.size() < 3;
            const bool wordBounded = (idx == 0 || !text.at(idx - 1).isLetterOrNumber()) &&
                                     (end >= n || !text.at(end).isLetterOrNumber());
            if (shortSecret && !wordBounded) {
                from = idx + 1;
            }
            else if (!rangeTaken(taken, idx, end)) {
                for (int k = idx; k < end; ++k) {
                    taken[static_cast<size_t>(k)] = 1;
                }
                endAt[static_cast<size_t>(idx)] = end;
                starts.push_back(idx);
                from = end;
            }
            else {
                from = idx + 1;
            }
        }
    }
    if (starts.empty()) {
        return text;
    }
    std::sort(starts.begin(), starts.end());
    QString out;
    out.reserve(n);
    int cursor = 0;
    for (int at : starts) {
        if (at < cursor) {
            continue;
        }
        out.append(text.mid(cursor, at - cursor));
        out.append(QStringLiteral("<redacted>"));
        cursor = endAt[static_cast<size_t>(at)];
    }
    out.append(text.mid(cursor));
    return out;
}

// §REVIEW-1127 qt_ui #13：使用者設定檔目錄的名稱（C:/Users/<名稱>、/home/<名稱>，斜線兩種都認）。Windows 的
// USERNAME 跟目錄名稱不一定一樣（改過帳號名稱、Microsoft 帳號自動產生的目錄名），名稱比對遮不到，這裡依路徑位置遮。
// 已經是 <redacted> 的（開頭是 '<'）不會再配到。可能會多遮一點（例如網址裡的 /home/），不影響功能
QString redactHomeDirNames(const QString& text)
{
    static const QRegularExpression homeDir(
            QStringLiteral("([\\\\/](?:Users|home)[\\\\/])([^\\\\/\\s\"'<>:|*?]+)"),
            QRegularExpression::CaseInsensitiveOption);
    QString out;
    out.reserve(text.size());
    int cursor = 0;
    QRegularExpressionMatchIterator it = homeDir.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        out.append(text.mid(cursor, match.capturedStart(2) - cursor));
        out.append(QStringLiteral("<redacted>"));
        cursor = match.capturedEnd(2);
    }
    out.append(text.mid(cursor));
    return out;
}

} // namespace

QString LogRedactor::redact(const QString& text, const QStringList& extraSecrets)
{
    if (text.isNull()) {
        return QString();
    }
    // §LOGDROP：名稱先遮。電腦名稱若剛好是一組公開 IP，要整段換成 <redacted>。
    QString masked = redactSecrets(text, extraSecrets);
    masked = redactHomeDirNames(masked);  // §REVIEW-1127 qt_ui #13
    masked = redactStructured(masked);
    return redactEmails(masked);
}
