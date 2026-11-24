package com.limelight.ui;

import android.hardware.display.DisplayManager;
import android.view.Display;

import com.limelight.LimeLog;

/**
 * §EXT-DISPLAY：挑選可拿來放串流畫面的外接顯示器。
 * 模擬次要顯示器（overlay_display_devices）依 AOSP OverlayDisplayAdapter 是
 * TYPE_OVERLAY、FLAG_PRESENTATION、STATE_ON，而且不是 FLAG_PRIVATE。
 * USB-C 眼鏡多半是 TYPE_EXTERNAL。這裡不用 type 過濾，避免兩種都被排掉。
 * Activity 自己所在的顯示器也要排除：DeX 與 Chromebook 會把 Activity 放在外接螢幕上，
 * 那台不是 {@code DEFAULT_DISPLAY}，拿來當 Presentation 會蓋住自己。
 */
public final class ExternalDisplayHelper {
    private ExternalDisplayHelper() {
    }

    public static final class Candidate {
        public final Display display;
        public final Display.Mode[] modes;
        public final Display.Mode chosen;

        Candidate(Display display, Display.Mode[] modes, Display.Mode chosen) {
            this.display = display;
            this.modes = modes;
            this.chosen = chosen;
        }
    }

    public static Candidate find(DisplayManager displayManager, int streamWidth, int streamHeight, int streamFps,
                                  int excludedDisplayId, int alsoExcludedDisplayId) {
        if (displayManager == null) {
            LimeLog.info("[VIPLE-EXTDISP] DisplayManager is null");
            return null;
        }
        Display[] displays = displayManager.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION);
        Display best = null;
        long bestPixels = -1;
        if (displays != null) {
            for (Display display : displays) {
                int id = display.getDisplayId();
                if (id != Display.DEFAULT_DISPLAY
                        && (id == excludedDisplayId || id == alsoExcludedDisplayId)) {
                    // 實機上這行出現，代表 Activity 正在這台外接螢幕上，沒有拿它當 Presentation。
                    LimeLog.info("[VIPLE-EXTDISP] skip displayId=" + id
                            + " name=" + display.getName()
                            + "; the activity is already on this display");
                }
                if (!isUsable(display, excludedDisplayId, alsoExcludedDisplayId)) {
                    continue;
                }
                Display.Mode current = display.getMode();
                long pixels = (long) current.getPhysicalWidth() * (long) current.getPhysicalHeight();
                if (pixels > bestPixels) {
                    best = display;
                    bestPixels = pixels;
                }
            }
        }
        if (best == null) {
            LimeLog.info("[VIPLE-EXTDISP] no usable presentation display");
            return null;
        }
        Display.Mode[] modes = best.getSupportedModes();
        if (modes == null) {
            modes = new Display.Mode[0];
        }
        Display.Mode chosen = pickMode(modes, streamWidth, streamHeight, streamFps);
        logSelection(best, modes, chosen);
        return new Candidate(best, modes, chosen);
    }

    public static boolean isPresentationCandidate(DisplayManager displayManager, int displayId,
                                                  int excludedDisplayId, int alsoExcludedDisplayId) {
        if (displayManager == null) {
            return false;
        }
        Display[] displays = displayManager.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION);
        if (displays == null) {
            return false;
        }
        for (Display display : displays) {
            if (display.getDisplayId() == displayId
                    && isUsable(display, excludedDisplayId, alsoExcludedDisplayId)) {
                return true;
            }
        }
        return false;
    }

    /**
     * 先鎖定解析度，再在那個解析度裡挑更新率。
     * 沒有任何模式的面積大於等於串流時，退回面積最大的；否則沒有模式可設。
     */
    public static Display.Mode pickMode(Display.Mode[] modes, int streamWidth, int streamHeight, int streamFps) {
        if (modes == null || modes.length == 0) {
            return null;
        }
        int targetWidth = streamWidth;
        int targetHeight = streamHeight;
        boolean exact = false;
        for (Display.Mode mode : modes) {
            if (mode.getPhysicalWidth() == streamWidth && mode.getPhysicalHeight() == streamHeight) {
                exact = true;
                break;
            }
        }
        if (!exact) {
            long streamArea = (long) streamWidth * (long) streamHeight;
            long bestDelta = Long.MAX_VALUE;
            long largestArea = -1;
            int largestWidth = modes[0].getPhysicalWidth();
            int largestHeight = modes[0].getPhysicalHeight();
            boolean foundNotSmaller = false;
            int nearWidth = largestWidth;
            int nearHeight = largestHeight;
            for (Display.Mode mode : modes) {
                long area = (long) mode.getPhysicalWidth() * (long) mode.getPhysicalHeight();
                if (area > largestArea) {
                    largestArea = area;
                    largestWidth = mode.getPhysicalWidth();
                    largestHeight = mode.getPhysicalHeight();
                }
                if (area < streamArea) {
                    continue;
                }
                long delta = area - streamArea;
                if (!foundNotSmaller || delta < bestDelta) {
                    foundNotSmaller = true;
                    bestDelta = delta;
                    nearWidth = mode.getPhysicalWidth();
                    nearHeight = mode.getPhysicalHeight();
                }
            }
            if (foundNotSmaller) {
                targetWidth = nearWidth;
                targetHeight = nearHeight;
            }
            else {
                targetWidth = largestWidth;
                targetHeight = largestHeight;
            }
        }

        Display.Mode highest = null;
        Display.Mode slowestEnough = null;
        for (Display.Mode mode : modes) {
            if (mode.getPhysicalWidth() != targetWidth || mode.getPhysicalHeight() != targetHeight) {
                continue;
            }
            if (highest == null || mode.getRefreshRate() > highest.getRefreshRate()) {
                highest = mode;
            }
            // 等於串流 fps 的最小更新率。沒有 >= 的就用這個解析度裡最高的。
            if (mode.getRefreshRate() >= streamFps
                    && (slowestEnough == null || mode.getRefreshRate() < slowestEnough.getRefreshRate())) {
                slowestEnough = mode;
            }
        }
        if (slowestEnough != null) {
            return slowestEnough;
        }
        return highest;
    }

    public static void logSelection(Display display, Display.Mode[] modes, Display.Mode chosen) {
        StringBuilder all = new StringBuilder();
        if (modes != null) {
            for (int i = 0; i < modes.length; i++) {
                if (i > 0) {
                    all.append(',');
                }
                all.append(formatMode(modes[i]));
            }
        }
        LimeLog.info("[VIPLE-EXTDISP] display name=" + display.getName()
                + " id=" + display.getDisplayId()
                + " flags=0x" + Integer.toHexString(display.getFlags())
                + " state=" + display.getState()
                + " current=" + formatMode(display.getMode())
                + " modes=[" + all + "]"
                + " chosen=" + formatMode(chosen));
    }

    private static boolean isUsable(Display display, int excludedDisplayId, int alsoExcludedDisplayId) {
        int id = display.getDisplayId();
        // getDisplay() 與 getDefaultDisplay() 在 DeX 上可能各回一個 id，兩個都不是這台 Activity 能再拿來蓋 Presentation 的。
        if (id == Display.DEFAULT_DISPLAY || id == excludedDisplayId || id == alsoExcludedDisplayId) {
            return false;
        }
        // FLAG_PRIVATE 是虛擬螢幕或系統私有層，不能拿來當使用者看的外接畫面。
        if ((display.getFlags() & Display.FLAG_PRIVATE) != 0) {
            return false;
        }
        return display.getState() == Display.STATE_ON;
    }

    public static String formatMode(Display.Mode mode) {
        if (mode == null) {
            return "none";
        }
        return mode.getPhysicalWidth() + "x" + mode.getPhysicalHeight()
                + "@" + mode.getRefreshRate()
                + "(id=" + mode.getModeId() + ")";
    }
}
