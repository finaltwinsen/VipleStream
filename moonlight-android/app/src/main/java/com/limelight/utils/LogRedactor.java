package com.limelight.utils;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collection;
import java.util.Collections;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * §LOGDROP：純 Java，讓 JVM 單元測試不用帶 Android。
 */
public final class LogRedactor {
    private static final Pattern EMAIL = Pattern.compile(
            "(?i)(?<![A-Z0-9._%+-])[A-Z0-9._%+-]+@[A-Z0-9-]+(?:\\.[A-Z0-9-]+)*\\.[A-Z]{2,}(?![A-Z0-9])");

    private LogRedactor() {
    }

    public static String redact(String text, Collection<String> extraSecrets) {
        if (text == null) {
            return null;
        }
        // §LOGDROP：名稱先遮。電腦名稱若剛好是一組公開 IP，要整段換成 <redacted>。
        String masked = redactSecrets(text, extraSecrets);
        masked = redactStructured(masked);
        return redactEmails(masked);
    }

    // §LOGDROP：MAC 也是多組十六進位加冒號。先遮 MAC，否則會整段變成 <ipv6>，前三組就留不住。
    private static String redactStructured(String text) {
        StringBuilder out = new StringBuilder(text.length());
        int i = 0;
        int n = text.length();
        while (i < n) {
            char c = text.charAt(i);
            if (isHex(c)) {
                int mac = macEnd(text, i);
                if (mac > i) {
                    out.append(maskMac(text.substring(i, mac)));
                    i = mac;
                    continue;
                }
                int v6 = ipv6End(text, i);
                if (v6 > i) {
                    out.append(ipv6Replacement(text.substring(i, v6)));
                    i = v6;
                    continue;
                }
                if (isDigit(c)) {
                    int v4 = matchIpv4(text, i, true);
                    if (v4 > i) {
                        out.append(maskIpv4(text.substring(i, v4)));
                        i = v4;
                        continue;
                    }
                }
            } else if (c == ':') {
                int v6 = ipv6End(text, i);
                if (v6 > i) {
                    out.append(ipv6Replacement(text.substring(i, v6)));
                    i = v6;
                    continue;
                }
            }
            out.append(c);
            i++;
        }
        return out.toString();
    }

    private static String ipv6Replacement(String token) {
        int scope = token.indexOf('%');
        String address = scope < 0 ? token : token.substring(0, scope);
        // §LOGDROP：::1 是本機迴圈，不是外洩位址。scope 只標介面，一樣留著。
        if ("::1".equals(address)) {
            return token;
        }
        return "<ipv6>";
    }

    private static int macEnd(String text, int start) {
        int n = text.length();
        if (start + 17 > n || !isHex(text.charAt(start))) {
            return -1;
        }
        char sep = text.charAt(start + 2);
        if (sep != ':' && sep != '-') {
            return -1;
        }
        if (start > 0) {
            char prev = text.charAt(start - 1);
            if (isHex(prev) || prev == sep) {
                return -1;
            }
        }
        for (int group = 0; group < 6; group++) {
            int at = start + group * 3;
            if (!isHex(text.charAt(at)) || !isHex(text.charAt(at + 1))) {
                return -1;
            }
            if (group < 5 && text.charAt(at + 2) != sep) {
                return -1;
            }
        }
        int end = start + 17;
        if (end < n) {
            char next = text.charAt(end);
            if (isHex(next) || next == sep) {
                return -1;
            }
        }
        return end;
    }

    private static String maskMac(String token) {
        char sep = token.charAt(2);
        return token.substring(0, 8) + sep + "xx" + sep + "xx" + sep + "xx";
    }

    private static int ipv6End(String text, int start) {
        int n = text.length();
        if (start >= n) {
            return -1;
        }
        // §LOGDROP：Class::method 這種堆疊也有兩個冒號。緊貼識別字時不是 IPv6。
        if (start > 0) {
            char prev = text.charAt(start - 1);
            if (isIdent(prev) || prev == ':') {
                return -1;
            }
        }
        int i = start;
        int colons = 0;
        int groups = 0;
        boolean compressed = false;
        boolean hexLetter = false;
        char first = text.charAt(i);
        if (first == ':') {
            if (i + 1 >= n || text.charAt(i + 1) != ':') {
                return -1;
            }
            compressed = true;
            colons = 2;
            i += 2;
            if (i < n && text.charAt(i) == ':') {
                return -1;
            }
        } else if (!isHex(first)) {
            return -1;
        }

        if (i < n && text.charAt(i) != '%') {
            while (i < n) {
                if (isDigit(text.charAt(i)) && colons > 0 && i > start && text.charAt(i - 1) == ':') {
                    int v4 = matchIpv4(text, i, false);
                    if (v4 > i && ipv6TailBoundary(text, v4)) {
                        // IPv4 尾是 32 位元，算兩組。
                        groups += 2;
                        i = v4;
                        break;
                    }
                }
                int digits = 0;
                boolean letter = false;
                while (i < n && digits < 4 && isHex(text.charAt(i))) {
                    if (isHexLetter(text.charAt(i))) {
                        letter = true;
                    }
                    digits++;
                    i++;
                }
                if (digits == 0) {
                    if (compressed && i > start && text.charAt(i - 1) == ':') {
                        break;
                    }
                    return -1;
                }
                if (i < n && isHex(text.charAt(i))) {
                    return -1;
                }
                if (letter) {
                    hexLetter = true;
                }
                groups++;
                if (i < n && text.charAt(i) == ':') {
                    if (i + 1 < n && text.charAt(i + 1) == ':') {
                        if (compressed) {
                            return -1;
                        }
                        compressed = true;
                        colons += 2;
                        i += 2;
                        if (i < n && text.charAt(i) == ':') {
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
                    colons++;
                    i++;
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
        } else if (groups > 7) {
            return -1;
        }
        // §LOGDROP：logcat 的 12:34:56 沒有 a–f、也沒有 ::。再擋一次純時間，避免以後放寬時誤傷。
        if (isClockTime(text.substring(start, i))) {
            return -1;
        }
        if (i < n && text.charAt(i) == '%') {
            int scope = i + 1;
            int j = scope;
            while (j < n && isScopeChar(text.charAt(j))) {
                j++;
            }
            if (j == scope) {
                return -1;
            }
            i = j;
        }
        if (i < n) {
            char next = text.charAt(i);
            if (isHex(next) || next == ':') {
                return -1;
            }
            // §LOGDROP：句點可以當結尾。點後面還有數字就是更長的點分字串，不要從中間切。
            if (next == '.' && i + 1 < n && isDigit(text.charAt(i + 1))) {
                return -1;
            }
        }
        if (i <= start) {
            return -1;
        }
        return i;
    }

    private static boolean ipv6TailBoundary(String text, int end) {
        if (end >= text.length()) {
            return true;
        }
        char after = text.charAt(end);
        if (after == '%') {
            return true;
        }
        if (after == '.') {
            return end + 1 >= text.length() || !isDigit(text.charAt(end + 1));
        }
        return !isHex(after) && after != ':';
    }

    private static boolean isClockTime(String token) {
        return token.matches("\\d{1,2}:\\d{2}:\\d{2}");
    }

    // §LOGDROP：前後若還黏著數字或點，多半是版號或更長的數字，不是一組 IP。
    private static int matchIpv4(String text, int start, boolean checkBefore) {
        int n = text.length();
        if (start >= n || !isDigit(text.charAt(start))) {
            return -1;
        }
        if (checkBefore && start > 0) {
            char prev = text.charAt(start - 1);
            if (isDigit(prev) || prev == '.') {
                return -1;
            }
        }
        int i = start;
        for (int group = 0; group < 4; group++) {
            if (i >= n || !isDigit(text.charAt(i))) {
                return -1;
            }
            int value = 0;
            int digits = 0;
            while (i < n && isDigit(text.charAt(i))) {
                if (digits == 3) {
                    return -1;
                }
                value = value * 10 + (text.charAt(i) - '0');
                digits++;
                i++;
            }
            if (digits == 0 || value > 255) {
                return -1;
            }
            if (group < 3) {
                if (i >= n || text.charAt(i) != '.') {
                    return -1;
                }
                i++;
            }
        }
        if (i < n) {
            char next = text.charAt(i);
            if (isDigit(next) || next == '.') {
                return -1;
            }
        }
        return i;
    }

    private static String maskIpv4(String token) {
        String[] parts = token.split("\\.", -1);
        if (parts.length != 4) {
            return "<ip>";
        }
        int[] value = new int[4];
        try {
            for (int i = 0; i < 4; i++) {
                value[i] = Integer.parseInt(parts[i]);
            }
        } catch (NumberFormatException error) {
            return "<ip>";
        }
        if (value[0] == 127 || (value[0] == 0 && value[1] == 0 && value[2] == 0 && value[3] == 0)) {
            return token;
        }
        boolean local = value[0] == 10
                || (value[0] == 172 && value[1] >= 16 && value[1] <= 31)
                || (value[0] == 192 && value[1] == 168)
                || (value[0] == 100 && value[1] >= 64 && value[1] <= 127)
                || (value[0] == 169 && value[1] == 254);
        if (local) {
            return parts[0] + "." + parts[1] + ".x.x";
        }
        return "<ip>";
    }

    // §LOGDROP：minSdk 28 的 Matcher.appendReplacement 只吃 StringBuffer，StringBuilder 要 API 34。
    private static String redactEmails(String text) {
        Matcher matcher = EMAIL.matcher(text);
        StringBuffer buffer = new StringBuffer(text.length());
        while (matcher.find()) {
            matcher.appendReplacement(buffer, Matcher.quoteReplacement("<email>"));
        }
        matcher.appendTail(buffer);
        return buffer.toString();
    }

    private static String redactSecrets(String text, Collection<String> extraSecrets) {
        if (extraSecrets == null || extraSecrets.isEmpty() || text.isEmpty()) {
            return text;
        }
        ArrayList<String> secrets = new ArrayList<String>();
        for (String secret : extraSecrets) {
            // §LOGDROP：短於 3 的字會把內文打散，不遮。
            if (secret != null && secret.length() >= 3) {
                secrets.add(secret);
            }
        }
        if (secrets.isEmpty()) {
            return text;
        }
        // §LOGDROP：短名稱先換的話，長名稱會被切碎，電腦名稱就留在 log 裡。
        secrets.sort((left, right) -> Integer.compare(right.length(), left.length()));
        int n = text.length();
        boolean[] taken = new boolean[n];
        int[] endAt = new int[n];
        Arrays.fill(endAt, -1);
        ArrayList<Integer> starts = new ArrayList<Integer>();
        for (int s = 0; s < secrets.size(); s++) {
            String secret = secrets.get(s);
            int from = 0;
            while (from <= n - secret.length()) {
                int idx = indexOfIgnoreCase(text, secret, from);
                if (idx < 0) {
                    break;
                }
                int end = idx + secret.length();
                if (!rangeTaken(taken, idx, end)) {
                    for (int k = idx; k < end; k++) {
                        taken[k] = true;
                    }
                    endAt[idx] = end;
                    starts.add(idx);
                    from = end;
                } else {
                    from = idx + 1;
                }
            }
        }
        if (starts.isEmpty()) {
            return text;
        }
        Collections.sort(starts);
        StringBuilder out = new StringBuilder(n);
        int cursor = 0;
        for (int s = 0; s < starts.size(); s++) {
            int at = starts.get(s);
            if (at < cursor) {
                continue;
            }
            out.append(text, cursor, at);
            out.append("<redacted>");
            cursor = endAt[at];
        }
        out.append(text, cursor, n);
        return out.toString();
    }

    private static boolean rangeTaken(boolean[] taken, int start, int end) {
        for (int i = start; i < end; i++) {
            if (taken[i]) {
                return true;
            }
        }
        return false;
    }

    private static int indexOfIgnoreCase(String text, String needle, int from) {
        int last = text.length() - needle.length();
        for (int i = from; i <= last; i++) {
            if (text.regionMatches(true, i, needle, 0, needle.length())) {
                return i;
            }
        }
        return -1;
    }

    private static boolean isDigit(char c) {
        return c >= '0' && c <= '9';
    }

    private static boolean isHexLetter(char c) {
        return (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }

    private static boolean isHex(char c) {
        return isDigit(c) || isHexLetter(c);
    }

    private static boolean isIdent(char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || isDigit(c) || c == '_';
    }

    private static boolean isScopeChar(char c) {
        return isIdent(c) || c == '.' || c == '-' || c == '~';
    }
}
