/*
 * VipleStream — Safe / Bold editorial design-variant runtime tuner.
 *
 * The two main activities (PcView, AppView) and the grid-item adapters
 * all inflate a single layout that defaults to the "Bold" editorial
 * typography (oversized display titles, full §NN section meta above
 * each tile). When the user toggles the "Bold editorial design"
 * preference OFF (Safe), we shrink the relevant text sizes, trim the
 * masthead height, and hide some decorative §NN labels at runtime.
 *
 * Activities call applyToActivity(...) in onCreate, and adapters call
 * applyToTile(...) in bindView/getView. No layout duplication needed.
 */
package com.limelight.ui;

import android.view.View;
import android.view.ViewGroup;
import android.widget.TextView;

import com.limelight.preferences.PreferenceConfiguration;

public final class VsDesignVariant {
    private VsDesignVariant() {}

    /**
     * Retune a View subtree to the current Safe/Bold variant.
     * Walks the tree looking for TextViews tagged via
     * {@code android:tag} with magic markers ("vs_display" /
     * "vs_meta") and adjusts typography accordingly. Untagged Views
     * are left alone. Returns silently on null inputs.
     */
    public static void apply(View root, boolean bold) {
        if (root == null) return;
        if (root instanceof ViewGroup) {
            ViewGroup vg = (ViewGroup) root;
            for (int i = 0; i < vg.getChildCount(); ++i) {
                apply(vg.getChildAt(i), bold);
            }
        }
        if (!(root instanceof TextView)) return;
        TextView tv = (TextView) root;
        Object tag = tv.getTag();
        if (!(tag instanceof CharSequence)) return;
        String t = tag.toString();
        if ("vs_display".equals(t)) {
            // Bold mastheads (VipleStream wordmark, PC name) use the
            // oversized type; Safe quiets them by ~6sp.
            tv.setTextSize(bold ? 24f : 17f);
        } else if ("vs_display_large".equals(t)) {
            // Extra-large cover-story title (reserved for Bold hero).
            tv.setTextSize(bold ? 28f : 18f);
        } else if ("vs_meta".equals(t)) {
            // "§ NN · HEADER" mono lime meta above the display title.
            // Hidden outright on Safe — the magazine numbering is the
            // main Bold affordance; Safe tiles just show their name.
            tv.setVisibility(bold ? View.VISIBLE : View.GONE);
        } else if ("vs_meta_small".equals(t)) {
            // Smaller in-tile §NN meta that we want visible in both
            // variants, just sized differently.
            tv.setTextSize(bold ? 10f : 8f);
        }
    }

    /** Convenience: read the pref + apply in one shot. */
    public static void apply(View root, android.content.Context ctx) {
        apply(root, PreferenceConfiguration.readPreferences(ctx).designBold);
    }
}
