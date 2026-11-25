package com.limelight.ui;

import android.app.Presentation;
import android.content.Context;
import android.graphics.Color;
import android.os.Bundle;
import android.view.Display;
import android.view.Gravity;
import android.view.SurfaceHolder;
import android.view.ViewGroup;
import android.util.TypedValue;
import android.view.Window;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.TextView;

/**
 * §EXT-DISPLAY：串流畫面畫在外接顯示器上。手機 Activity 另外當觸控板。
 */
public class StreamPresentation extends Presentation {
    private final Display.Mode preferredMode;
    private Runnable onDisplayLost;
    private StreamView streamView;
    private TextView topHint;

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
        topHint = new TextView(getContext());
        topHint.setTextColor(Color.WHITE);
        topHint.setTextSize(TypedValue.COMPLEX_UNIT_SP, 18);
        topHint.setGravity(Gravity.CENTER);
        topHint.setBackgroundColor(0xCC000000);
        int pad = (int) (12 * getContext().getResources().getDisplayMetrics().density + 0.5f);
        topHint.setPadding(pad, pad, pad, pad);
        // §VITURE：這行蓋在畫面上緣。設成可點的話，眼鏡上的觸控會被提示吃掉。
        topHint.setClickable(false);
        topHint.setFocusable(false);
        topHint.setVisibility(ViewGroup.GONE);
        FrameLayout.LayoutParams hintParams = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.TOP | Gravity.CENTER_HORIZONTAL);
        hintParams.topMargin = pad;
        hintParams.leftMargin = pad * 2;
        hintParams.rightMargin = pad * 2;
        root.addView(topHint, hintParams);
        setContentView(root);
    }

    /** 空字串會藏起來。呼叫端在切回 2D 時清掉。 */
    public void setTopHint(CharSequence text) {
        if (topHint == null) {
            return;
        }
        if (text == null || text.length() == 0) {
            topHint.setVisibility(ViewGroup.GONE);
            topHint.setText("");
            return;
        }
        topHint.setText(text);
        topHint.setVisibility(ViewGroup.VISIBLE);
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
