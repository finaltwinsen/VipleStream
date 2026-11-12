package com.limelight.utils;

import java.util.ArrayList;
import java.util.List;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * §UPDATE-NOTES：更新對話框的「更新摘要」。規則和電腦版的 moonlight-qt/app/backend/updatenotes.h 相同：
 * release notes 是雙語（英文在前，單獨一行的「# 繁體中文」之後是中文）；摘要＝標題＋開頭說明＋What's new 段。
 * 介面語系 zh 開頭用中文段，其餘用英文段。這裡輸出純文字（AlertDialog 不畫 Markdown），所以拿掉粗體記號。
 * 只用 java.*，可以在開發機用一般的 JVM 測。
 */
public final class UpdateNotes {
    private static final int MAX_CHARS = 4000;
    private static final Pattern ZH_HEADING = Pattern.compile("^#\\s*繁體中文\\s*$", Pattern.MULTILINE);
    private static final Pattern WHATS_NEW = Pattern.compile("^#{2,4}\\s*(what'?s new|新功能|更新內容).*", Pattern.CASE_INSENSITIVE);
    // 語言切換列（「**English** · [繁體中文](#…)」）與分隔線不是內容
    private static final Pattern CHROME = Pattern.compile("^(-{3,}|.*\\]\\(#[^)]*\\).*)$");

    private UpdateNotes() {
    }

    static String pickLanguage(String body, String languageTag) {
        String text = body.replace("\r\n", "\n");
        Matcher m = ZH_HEADING.matcher(text);
        if (!m.find()) {
            return text.trim();
        }
        String en = text.substring(0, m.start()).trim();
        String zh = text.substring(m.end()).trim();
        boolean wantZh = languageTag != null && languageTag.toLowerCase().startsWith("zh");
        if (wantZh) {
            return zh.isEmpty() ? en : zh;
        }
        return en.isEmpty() ? zh : en;
    }

    public static String summarize(String body, String languageTag) {
        if (body == null || body.trim().isEmpty()) {
            return "";
        }
        String[] raw = pickLanguage(body, languageTag).split("\n", -1);
        List<String> lines = new ArrayList<>();
        for (String l : raw) {
            if (!CHROME.matcher(l.trim()).matches()) {
                lines.add(l);
            }
        }
        StringBuilder intro = new StringBuilder();
        StringBuilder news = new StringBuilder();
        int part = 0;  // 0＝開頭、1＝What's new、2＝略過
        boolean haveTitle = false;
        for (String rawLine : lines) {
            String line = rawLine.trim();
            if (line.startsWith("## ")) {
                if (part == 0 && !haveTitle && intro.toString().trim().isEmpty()) {
                    intro.setLength(0);
                    intro.append(line.substring(3).trim()).append("\n\n");
                    haveTitle = true;
                    continue;
                }
                part = 2;
                continue;
            }
            if (line.startsWith("#")) {
                part = WHATS_NEW.matcher(line).matches() ? 1 : 2;
                continue;
            }
            if (part == 0) {
                intro.append(rawLine).append('\n');
            } else if (part == 1) {
                news.append(rawLine).append('\n');
            }
        }
        String out;
        if (news.toString().trim().isEmpty()) {
            StringBuilder all = new StringBuilder();
            for (String l : lines) {
                all.append(l).append('\n');
            }
            out = all.toString();
        } else {
            out = intro.toString().trim() + "\n\n" + news.toString().trim();
        }
        // 純文字：粗體記號、行內程式碼的反引號、連結的網址拿掉
        out = out.replace("**", "").replace("`", "");
        out = out.replaceAll("\\[([^\\]]+)\\]\\([^)]*\\)", "$1");
        out = out.replaceAll("\\n{3,}", "\n\n").trim();
        if (out.length() > MAX_CHARS) {
            int cut = MAX_CHARS;
            if (Character.isHighSurrogate(out.charAt(cut - 1))) {
                cut--;
            }
            out = out.substring(0, cut).trim() + "\n\n…";
        }
        return out;
    }
}
