package com.limelight.utils;

import android.annotation.TargetApi;
import android.app.Activity;
import android.content.pm.ShortcutInfo;
import android.content.pm.ShortcutManager;
import android.graphics.Bitmap;
import android.graphics.drawable.Icon;
import android.os.Build;

import com.limelight.LimeLog;
import com.limelight.R;
import com.limelight.nvstream.http.ComputerDetails;
import com.limelight.nvstream.http.NvApp;

import java.util.ArrayList;
import java.util.Collections;
import java.util.HashSet;
import java.util.LinkedList;
import java.util.List;
import java.util.concurrent.ConcurrentHashMap;

public class ShortcutHelper {

    private final ShortcutManager sm;
    private final Activity context;
    private final TvChannelHelper tvChannelHelper;

    // 這個行程裡已經確認「動態捷徑在、名稱是這個」的主機（uuid → 當時的名稱）。
    // 主機清單每 1.5 秒輪詢一次，沒有這張表的話每台主機每輪都要打 5～8 次
    // ShortcutManager（內容沒變也會讓系統重寫捷徑檔、叫醒啟動器）。
    // 只有輪詢那條路（createAppViewShortcutForOnlineHost）會查；使用者操作的路徑照舊。
    private static final ConcurrentHashMap<String, String> ensuredHosts = new ConcurrentHashMap<>();
    // 捷徑被停用或被擠掉時遞增（受 ensuredHosts 的 monitor 保護）。輪詢那邊的確認如果
    // 跨過這個時間點就不記，否則會把「停用之前確認過」記回去，之後這台主機重新加入時
    // 捷徑就一直停在停用狀態。
    private static int ensuredGeneration;

    private static void forgetEnsuredHost(String id) {
        synchronized (ensuredHosts) {
            ensuredGeneration++;
            if (id != null) {
                ensuredHosts.remove(id);
            }
        }
    }

    public ShortcutHelper(Activity context) {
        this.context = context;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N_MR1) {
            sm = context.getSystemService(ShortcutManager.class);
        }
        else {
            sm = null;
        }
        this.tvChannelHelper = new TvChannelHelper(context);
    }

    @TargetApi(Build.VERSION_CODES.N_MR1)
    private void reapShortcutsForDynamicAdd() {
        List<ShortcutInfo> dynamicShortcuts = sm.getDynamicShortcuts();
        while (!dynamicShortcuts.isEmpty() && dynamicShortcuts.size() >= sm.getMaxShortcutCountPerActivity()) {
            ShortcutInfo maxRankShortcut = dynamicShortcuts.get(0);
            for (ShortcutInfo scut : dynamicShortcuts) {
                if (maxRankShortcut.getRank() < scut.getRank()) {
                    maxRankShortcut = scut;
                }
            }
            sm.removeDynamicShortcuts(Collections.singletonList(maxRankShortcut.getId()));
            // 被擠掉的可能是輪詢確認過的主機：忘掉它，之後有空位時輪詢才會把它補回來
            forgetEnsuredHost(maxRankShortcut.getId());
        }
    }

    @TargetApi(Build.VERSION_CODES.N_MR1)
    private List<ShortcutInfo> getAllShortcuts() {
        LinkedList<ShortcutInfo> list = new LinkedList<>();
        list.addAll(sm.getDynamicShortcuts());
        list.addAll(sm.getPinnedShortcuts());
        return list;
    }

    @TargetApi(Build.VERSION_CODES.N_MR1)
    private ShortcutInfo getInfoForId(String id) {
        List<ShortcutInfo> shortcuts = getAllShortcuts();

        for (ShortcutInfo info : shortcuts) {
            if (info.getId().equals(id)) {
                return info;
            }
        }

        return null;
    }

    @TargetApi(Build.VERSION_CODES.N_MR1)
    private boolean isExistingDynamicShortcut(String id) {
        for (ShortcutInfo si : sm.getDynamicShortcuts()) {
            if (si.getId().equals(id)) {
                return true;
            }
        }

        return false;
    }

    public void reportComputerShortcutUsed(ComputerDetails computer) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N_MR1) {
            if (getInfoForId(computer.uuid) != null) {
                sm.reportShortcutUsed(computer.uuid);
            }
        }
    }

    public void reportGameLaunched(ComputerDetails computer, NvApp app) {
        tvChannelHelper.createTvChannel(computer);
        tvChannelHelper.addGameToChannel(computer, app);
    }

    public void createAppViewShortcut(ComputerDetails computer, boolean forceAdd, boolean newlyPaired) {
        ensureAppViewShortcut(computer, forceAdd, newlyPaired);
    }

    // 回傳這台主機的動態捷徑是不是確定存在、內容也是最新的。清單已滿沒加進去、
    // 或系統限流沒收下時回 false：那種情況之後還要再試，呼叫端不可以記成「已確認」。
    private boolean ensureAppViewShortcut(ComputerDetails computer, boolean forceAdd, boolean newlyPaired) {
        boolean ensured = true;

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N_MR1) {
            ShortcutInfo sinfo = new ShortcutInfo.Builder(context, computer.uuid)
                    .setIntent(ServerHelper.createPcShortcutIntent(context, computer))
                    .setShortLabel(computer.name)
                    .setLongLabel(computer.name)
                    .setIcon(Icon.createWithResource(context, R.mipmap.ic_pc_scut))
                    .build();

            ShortcutInfo existingSinfo = getInfoForId(computer.uuid);
            if (existingSinfo != null) {
                // Update in place
                if (!sm.updateShortcuts(Collections.singletonList(sinfo))) {
                    ensured = false;
                }
                sm.enableShortcuts(Collections.singletonList(computer.uuid));
            }

            // Reap shortcuts to make space for this if it's new
            // NOTE: This CAN'T be an else on the above if, because it's
            // possible that we have an existing shortcut but it's not a dynamic one.
            if (!isExistingDynamicShortcut(computer.uuid)) {
                // To avoid a random carousel of shortcuts popping in and out based on polling status,
                // we only add shortcuts if it's not at the limit or the user made a conscious action
                // to interact with this PC.

                if (forceAdd) {
                    // This should free an entry for us to add one below
                    reapShortcutsForDynamicAdd();
                }

                // We still need to check the maximum shortcut count even after reaping,
                // because there's a possibility that it could be zero.
                if (sm.getDynamicShortcuts().size() < sm.getMaxShortcutCountPerActivity()) {
                    // Add a shortcut if there is room
                    if (!sm.addDynamicShortcuts(Collections.singletonList(sinfo))) {
                        ensured = false;
                    }
                }
                else {
                    // 清單已滿、這一輪沒加進去
                    ensured = false;
                }
            }
        }

        if (newlyPaired) {
            // Avoid hammering the channel API for each computer poll because it will throttle us
            tvChannelHelper.createTvChannel(computer);
            tvChannelHelper.requestChannelOnHomeScreen(computer);
        }

        return ensured;
    }

    public void createAppViewShortcutForOnlineHost(ComputerDetails details) {
        final String uuid = details.uuid;
        final String name = details.name;

        // 這個行程裡已經確認過、名稱也沒變：這一輪不必再碰 ShortcutManager
        if (uuid != null && name != null && name.equals(ensuredHosts.get(uuid))) {
            return;
        }

        final int generation;
        synchronized (ensuredHosts) {
            generation = ensuredGeneration;
        }

        if (ensureAppViewShortcut(details, false, false) && uuid != null && name != null) {
            boolean recorded = false;
            synchronized (ensuredHosts) {
                // 確認的途中有捷徑被停用或擠掉就不記，下一輪重新確認
                if (generation == ensuredGeneration) {
                    ensuredHosts.put(uuid, name);
                    recorded = true;
                }
            }
            if (recorded) {
                // 每台主機每個行程只該出現一次（改名、刪除後重加、被擠掉後補回時會再一次）
                LimeLog.info("[VIPLE-PCLIST] shortcut confirmed for " + name);
            }
        }
    }

    private String getShortcutIdForGame(ComputerDetails computer, NvApp app) {
        return computer.uuid + app.getAppId();
    }

    @TargetApi(Build.VERSION_CODES.O)
    public boolean createPinnedGameShortcut(ComputerDetails computer, NvApp app, Bitmap iconBits) {
        if (sm.isRequestPinShortcutSupported()) {
            Icon appIcon;

            if (iconBits != null) {
                appIcon = Icon.createWithAdaptiveBitmap(iconBits);
            } else {
                appIcon = Icon.createWithResource(context, R.mipmap.ic_pc_scut);
            }

            ShortcutInfo sInfo = new ShortcutInfo.Builder(context, getShortcutIdForGame(computer, app))
                .setIntent(ServerHelper.createAppShortcutIntent(context, computer, app))
                .setShortLabel(app.getAppName() + " (" + computer.name + ")")
                .setIcon(appIcon)
                .build();

            return sm.requestPinShortcut(sInfo, null);
        } else {
            return false;
        }
    }

    public void disableComputerShortcut(ComputerDetails computer, CharSequence reason) {
        tvChannelHelper.deleteChannel(computer);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N_MR1) {
            // Delete the computer shortcut itself
            if (getInfoForId(computer.uuid) != null) {
                sm.disableShortcuts(Collections.singletonList(computer.uuid), reason);
            }

            // Delete all associated app shortcuts too
            List<ShortcutInfo> shortcuts = getAllShortcuts();
            LinkedList<String> appShortcutIds = new LinkedList<>();
            for (ShortcutInfo info : shortcuts) {
                if (info.getId().startsWith(computer.uuid)) {
                    appShortcutIds.add(info.getId());
                }
            }
            sm.disableShortcuts(appShortcutIds, reason);
        }

        // 要放在停用之後：這台主機之後再出現（重新加入、重新配對）時，輪詢要重新走一次
        // 完整流程把捷徑啟用回來
        forgetEnsuredHost(computer.uuid);
    }

    public void disableAppShortcut(ComputerDetails computer, NvApp app, CharSequence reason) {
        tvChannelHelper.deleteProgram(computer, app);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N_MR1) {
            String id = getShortcutIdForGame(computer, app);
            if (getInfoForId(id) != null) {
                sm.disableShortcuts(Collections.singletonList(id), reason);
            }
        }
    }

    public void enableAppShortcut(ComputerDetails computer, NvApp app) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N_MR1) {
            String id = getShortcutIdForGame(computer, app);
            if (getInfoForId(id) != null) {
                sm.enableShortcuts(Collections.singletonList(id));
            }
        }
    }

    // enableAppShortcut 的批次版：捷徑清單只查一次、啟用只送一次。AppView 每次建立都會把
    // 整份 app 清單當成新 app 跑一遍，逐個呼叫是每個 app 3 次跨行程呼叫（還在 UI 執行緒上）。
    public void enableAppShortcuts(ComputerDetails computer, List<NvApp> apps) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.N_MR1) {
            if (apps.isEmpty()) {
                return;
            }

            HashSet<String> existingIds = new HashSet<>();
            for (ShortcutInfo info : getAllShortcuts()) {
                existingIds.add(info.getId());
            }

            ArrayList<String> idsToEnable = new ArrayList<>();
            for (NvApp app : apps) {
                String id = getShortcutIdForGame(computer, app);
                if (existingIds.contains(id)) {
                    idsToEnable.add(id);
                }
            }

            if (!idsToEnable.isEmpty()) {
                sm.enableShortcuts(idsToEnable);
            }
        }
    }
}
