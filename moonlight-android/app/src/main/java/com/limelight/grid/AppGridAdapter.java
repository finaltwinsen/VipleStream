package com.limelight.grid;

import android.content.Context;
import android.graphics.BitmapFactory;
import android.view.View;
import android.widget.ImageView;
import android.widget.ProgressBar;
import android.widget.TextView;

import com.limelight.AppView;
import com.limelight.LimeLog;
import com.limelight.R;
import com.limelight.grid.assets.CachedAppAssetLoader;
import com.limelight.grid.assets.DiskAssetLoader;
import com.limelight.grid.assets.MemoryAssetLoader;
import com.limelight.grid.assets.NetworkAssetLoader;
import com.limelight.nvstream.http.ComputerDetails;
import com.limelight.preferences.PreferenceConfiguration;

import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.HashSet;
import java.util.List;
import java.util.Set;

@SuppressWarnings("unchecked")
public class AppGridAdapter extends GenericGridAdapter<AppView.AppObject> {
    private static final int ART_WIDTH_PX = 300;
    private static final int SMALL_WIDTH_DP = 100;
    private static final int LARGE_WIDTH_DP = 150;

    private final ComputerDetails computer;
    private final String uniqueId;
    private final boolean showHiddenApps;

    private CachedAppAssetLoader loader;
    private Set<Integer> hiddenAppIds = new HashSet<>();
    private ArrayList<AppView.AppObject> allApps = new ArrayList<>();

    // VipleStream H Phase 2: live sort mode. Captured at adapter
    // construction and refreshed by AppView whenever user pops the
    // settings sheet. Volatile because addApp() / removeApp() can be
    // called from a poller thread while the UI thread sets a new mode.
    private volatile PreferenceConfiguration.AppSortMode sortMode =
            PreferenceConfiguration.AppSortMode.RECENT;

    public AppGridAdapter(Context context, PreferenceConfiguration prefs, ComputerDetails computer, String uniqueId, boolean showHiddenApps) {
        super(context, getLayoutIdForPreferences(prefs));

        this.computer = computer;
        this.uniqueId = uniqueId;
        this.showHiddenApps = showHiddenApps;
        this.sortMode = prefs.appSortMode;

        updateLayoutWithPreferences(context, prefs);
    }

    /**
     * VipleStream H Phase 2: re-sort current and all-apps lists with a
     * new mode. AppView calls this after the settings activity returns
     * and its prefs have been re-read. Notifies the adapter so the
     * grid redraws in new order without a full refetch.
     */
    public void setSortMode(PreferenceConfiguration.AppSortMode mode) {
        if (mode == null || mode == this.sortMode) {
            return;
        }
        this.sortMode = mode;
        sortList(allApps);
        sortList(itemList);
        notifyDataSetChanged();
    }

    /**
     * VipleStream H Phase 2: re-sort with the current mode.
     * AppView's app-list poller calls this when an existing app's
     * sort-relevant metadata (lastPlayed / playtimeMinutes / source)
     * changes between polls — without re-sorting, the host's fresh
     * playtime updates wouldn't push a just-played game to the top.
     * Caller is responsible for invoking notifyDataSetChanged().
     */
    public void resort() {
        sortList(allApps);
        sortList(itemList);
    }

    public void updateHiddenApps(Set<Integer> newHiddenAppIds, boolean hideImmediately) {
        this.hiddenAppIds.clear();
        this.hiddenAppIds.addAll(newHiddenAppIds);

        if (hideImmediately) {
            // Reconstruct the itemList with the new hidden app set
            itemList.clear();
            for (AppView.AppObject app : allApps) {
                app.isHidden = hiddenAppIds.contains(app.app.getAppId());

                if (!app.isHidden || showHiddenApps) {
                    itemList.add(app);
                }
            }
        }
        else {
            // Just update the isHidden state to show the correct UI indication
            for (AppView.AppObject app : allApps) {
                app.isHidden = hiddenAppIds.contains(app.app.getAppId());
            }
        }

        notifyDataSetChanged();
    }

    private static int getLayoutIdForPreferences(PreferenceConfiguration prefs) {
        if (prefs.smallIconMode) {
            return R.layout.app_grid_item_small;
        }
        else {
            return R.layout.app_grid_item;
        }
    }

    public void updateLayoutWithPreferences(Context context, PreferenceConfiguration prefs) {
        int dpi = context.getResources().getDisplayMetrics().densityDpi;
        int dp;

        if (prefs.smallIconMode) {
            dp = SMALL_WIDTH_DP;
        }
        else {
            dp = LARGE_WIDTH_DP;
        }

        double scalingDivisor = ART_WIDTH_PX / (dp * (dpi / 160.0));
        if (scalingDivisor < 1.0) {
            // We don't want to make them bigger before draw-time
            scalingDivisor = 1.0;
        }
        LimeLog.info("Art scaling divisor: " + scalingDivisor);

        if (loader != null) {
            // Cancel operations on the old loader
            cancelQueuedOperations();
        }

        this.loader = new CachedAppAssetLoader(computer, scalingDivisor,
                new NetworkAssetLoader(context, uniqueId),
                new MemoryAssetLoader(),
                new DiskAssetLoader(context),
                BitmapFactory.decodeResource(context.getResources(), R.drawable.no_app_image));

        // This will trigger the view to reload with the new layout
        setLayoutId(getLayoutIdForPreferences(prefs));
    }

    public void cancelQueuedOperations() {
        loader.cancelForegroundLoads();
        loader.cancelBackgroundLoads();
        loader.freeCacheMemory();
    }

    /**
     * Case-insensitive name compare — used both as a tie-breaker for
     * the data-driven modes and as the whole sort key for
     * AppSortMode.NAME / AppSortMode.DEFAULT.
     */
    private static int compareName(AppView.AppObject lhs, AppView.AppObject rhs) {
        return lhs.app.getAppName().toLowerCase().compareTo(rhs.app.getAppName().toLowerCase());
    }

    /**
     * Comparator: long descending, with 0/missing values sinking to
     * the bottom (handled by callers via the alphabetic tie-break).
     * Used for both lastPlayed and playtimeMinutes.
     */
    private static int compareLongDesc(long lhs, long rhs) {
        return Long.compare(rhs, lhs);
    }

    private void sortList(List<AppView.AppObject> list) {
        // Snapshot the volatile field once per sort so different
        // entries can't disagree mid-sort (Comparator contract).
        final PreferenceConfiguration.AppSortMode mode = this.sortMode;

        Collections.sort(list, new Comparator<AppView.AppObject>() {
            @Override
            public int compare(AppView.AppObject lhs, AppView.AppObject rhs) {
                // Manual entries always pin to the top regardless of
                // mode. Source=="" (or null) means "declared in
                // apps.json by hand" — typically Desktop / Steam Big
                // Picture. Auto-imported Steam apps come after.
                boolean lManual = lhs.app.isManualEntry();
                boolean rManual = rhs.app.isManualEntry();
                if (lManual != rManual) {
                    return lManual ? -1 : 1;
                }

                switch (mode) {
                    case RECENT: {
                        long lp = lhs.app.getLastPlayed();
                        long rp = rhs.app.getLastPlayed();
                        if (lp != rp) {
                            // Both 0 falls through to name; otherwise
                            // descending date with 0s pushed last.
                            if (lp == 0) return 1;
                            if (rp == 0) return -1;
                            return compareLongDesc(lp, rp);
                        }
                        return compareName(lhs, rhs);
                    }
                    case PLAYTIME: {
                        long lp = lhs.app.getPlaytimeMinutes();
                        long rp = rhs.app.getPlaytimeMinutes();
                        if (lp != rp) {
                            if (lp == 0) return 1;
                            if (rp == 0) return -1;
                            return compareLongDesc(lp, rp);
                        }
                        return compareName(lhs, rhs);
                    }
                    case NAME:
                    case DEFAULT:
                    default:
                        return compareName(lhs, rhs);
                }
            }
        });
    }

    public void addApp(AppView.AppObject app) {
        // Update hidden state
        app.isHidden = hiddenAppIds.contains(app.app.getAppId());

        // Always add the app to the all apps list
        allApps.add(app);
        sortList(allApps);

        // Add the app to the adapter data if it's not hidden
        if (showHiddenApps || !app.isHidden) {
            // Queue a request to fetch this bitmap into cache
            loader.queueCacheLoad(app.app);

            // Add the app to our sorted list
            itemList.add(app);
            sortList(itemList);
        }
    }

    public void removeApp(AppView.AppObject app) {
        itemList.remove(app);
        allApps.remove(app);
    }

    @Override
    public void clear() {
        super.clear();
        allApps.clear();
    }

    @Override
    public void populateView(View parentView, ImageView imgView, ProgressBar prgView, TextView txtView, ImageView overlayView, AppView.AppObject obj) {
        // Let the cached asset loader handle it
        loader.populateImageView(obj.app, imgView, txtView);

        if (obj.isRunning) {
            // Show the play button overlay
            overlayView.setImageResource(R.drawable.ic_play);
            overlayView.setVisibility(View.VISIBLE);
        }
        else {
            overlayView.setVisibility(View.GONE);
        }

        if (obj.isHidden) {
            parentView.setAlpha(0.40f);
        }
        else {
            parentView.setAlpha(1.0f);
        }
    }
}
