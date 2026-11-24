package com.limelight.glasses;

import java.util.Locale;

/**
 * §VITURE：軸向字串是純 Java，JVM 測試才不必碰到 SharedPreferences。
 * 三個逗號分隔的 token，每個是正負號加 x／y／z，三軸都要出現一次。
 * head[i] = sign[i] * sensor[perm[i]]。"+x,+y,+z" 是單位矩陣。
 */
public final class VitureAxisMap {
    public static final String PREFS_NAME = "viture_glasses";
    public static final String PREFS_KEY = "viture_axis_map";
    public static final String DEFAULT = "+x,+y,+z";

    private VitureAxisMap() {
    }

    /** @return 正規化後的字串；格式不對就 null，呼叫端不要覆寫已存的值。 */
    public static String canonical(String text) {
        int[] perm = new int[3];
        int[] sign = new int[3];
        if (!parse(text, perm, sign)) {
            return null;
        }
        return format(perm, sign);
    }

    public static boolean parse(String text, int[] perm, int[] sign) {
        if (text == null || perm == null || sign == null || perm.length < 3 || sign.length < 3) {
            return false;
        }
        String[] parts = text.trim().toLowerCase(Locale.US).split(",");
        if (parts.length != 3) {
            return false;
        }
        boolean[] used = new boolean[3];
        int[] parsedPerm = new int[3];
        int[] parsedSign = new int[3];
        for (int i = 0; i < 3; i++) {
            String part = parts[i].trim();
            if (part.length() != 2) {
                return false;
            }
            char signChar = part.charAt(0);
            char axis = part.charAt(1);
            if (signChar != '+' && signChar != '-') {
                return false;
            }
            int index = axis == 'x' ? 0 : axis == 'y' ? 1 : axis == 'z' ? 2 : -1;
            if (index < 0 || used[index]) {
                return false;
            }
            used[index] = true;
            parsedPerm[i] = index;
            parsedSign[i] = signChar == '+' ? 1 : -1;
        }
        System.arraycopy(parsedPerm, 0, perm, 0, 3);
        System.arraycopy(parsedSign, 0, sign, 0, 3);
        return true;
    }

    public static String format(int[] perm, int[] sign) {
        StringBuilder out = new StringBuilder(11);
        for (int i = 0; i < 3; i++) {
            if (i > 0) {
                out.append(',');
            }
            out.append(sign[i] < 0 ? '-' : '+');
            out.append("xyz".charAt(perm[i]));
        }
        return out.toString();
    }
}
