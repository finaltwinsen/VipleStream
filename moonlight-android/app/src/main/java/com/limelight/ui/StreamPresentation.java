package com.limelight.ui;

import android.app.Presentation;
import android.content.Context;
import android.graphics.Color;
import android.os.Bundle;
import android.view.Display;
import android.view.Gravity;
import android.view.SurfaceHolder;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowManager;
import android.widget.FrameLayout;

/**
 * §EXT-DISPLAY：串流畫面畫在外接顯示器上。手機 Activity 另外當觸控板。
 */
public class StreamPresentation extends Presentation {
    private final Display.Mode preferredMode;
    private Runnable onDisplayLost;
    private StreamView streamView;

    public StreamPresentation(Context outerContext, Display display, Display.Mode preferredMode, Runnable onDisplayLost) {
        super(outerContext, display);
        this.preferredMode = preferredMode;
        this.onDisplayLost = onDisplayLost;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        Window window = getWindow();
        // 系統列會占掉眼鏡上的一條。KEEP_SCREEN_ON 只作用在這個視窗，不會把手機一起鎖亮。
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON | WindowManager.LayoutParams.FLAG_FULLSCREEN);
        if (preferredMode != null) {
            WindowManager.LayoutParams attrs = window.getAttributes();
            attrs.preferredDisplayModeId = preferredMode.getModeId();
            window.setAttributes(attrs);
        }

        FrameLayout root = new FrameLayout(getContext());
        root.setBackgroundColor(Color.BLACK);
        streamView = new StreamView(getContext());
        root.addView(streamView, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT,
                Gravity.CENTER));
        setContentView(root);
    }

    public StreamView getStreamView() {
        return streamView;
    }

    public SurfaceHolder getSurfaceHolder() {
        return streamView.getHolder();
    }

    public void setPreferredMode(Display.Mode mode) {
        Window window = getWindow();
        if (window == null || mode == null) {
            return;
        }
        WindowManager.LayoutParams attrs = window.getAttributes();
        attrs.preferredDisplayModeId = mode.getModeId();
        window.setAttributes(attrs);
    }

    @Override
    public void onDisplayRemoved() {
        // §EXT-DISPLAY：回呼要排在 super 之前。Game 得先把解碼器改接到暫存 Surface，
        // super 才拆這個視窗；順序反了，解碼器會寫進已經銷毀的 Surface。
        Runnable lost = onDisplayLost;
        onDisplayLost = null;
        if (lost != null) {
            lost.run();
        }
        super.onDisplayRemoved();
    }
}
