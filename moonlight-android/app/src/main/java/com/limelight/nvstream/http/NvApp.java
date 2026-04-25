package com.limelight.nvstream.http;

import com.limelight.LimeLog;

public class NvApp {
    private String appName = "";
    private int appId;
    private boolean initialized;
    private boolean hdrSupported;

    // VipleStream H Phase 2: provenance + Steam metadata fields. These
    // mirror the moonlight-qt NvApp additions so the Android client can
    // do the same sort modes (recent / playtime) and pin manually-defined
    // apps to the top of the list. All optional — apps coming from the
    // upstream Sunshine still parse fine, the new fields just stay empty
    // / zero in that case.
    //
    // source       — origin tag of this app entry. "" means a manually
    //                declared apps.json entry; "steam" means it was
    //                auto-imported by the host's Steam scanner. Sort
    //                logic uses an "empty source first" rule to pin
    //                manual entries to the top regardless of mode.
    // steamAppId   — Steam AppID as decimal string ("1382330"); used
    //                client-side for ownership filtering & deep-linking.
    // steamOwners  — comma-separated list of SteamID3 strings of the
    //                host users who own this title. Reserved for the
    //                Phase-3 "switch profile" UI; today we just round-trip.
    // lastPlayed   — Unix epoch seconds of the most recent play across
    //                all owners on the host. 0 if never played / unknown.
    // playtimeMins — total minutes played across all owners. 0 if
    //                unknown.
    private String source = "";
    private String steamAppId = "";
    private String steamOwners = "";
    private long lastPlayed = 0L;
    private long playtimeMinutes = 0L;

    public NvApp() {}

    public NvApp(String appName) {
        this.appName = appName;
    }

    public NvApp(String appName, int appId, boolean hdrSupported) {
        this.appName = appName;
        this.appId = appId;
        this.hdrSupported = hdrSupported;
        this.initialized = true;
    }

    public void setAppName(String appName) {
        this.appName = appName;
    }

    public void setAppId(String appId) {
        try {
            this.appId = Integer.parseInt(appId);
            this.initialized = true;
        } catch (NumberFormatException e) {
            LimeLog.warning("Malformed app ID: "+appId);
        }
    }

    public void setAppId(int appId) {
        this.appId = appId;
        this.initialized = true;
    }

    public void setHdrSupported(boolean hdrSupported) {
        this.hdrSupported = hdrSupported;
    }

    // VipleStream: setters parse the raw XML text — null/blank is OK,
    // we just preserve the default "" / 0L. Numeric setters use the
    // String overload so XML pull-parser callers don't need to do the
    // Long.parseLong themselves; malformed values fall back to 0
    // (no exception leaks up to derail the whole applist parse).
    public void setSource(String source) {
        this.source = (source == null) ? "" : source;
    }

    public void setSteamAppId(String steamAppId) {
        this.steamAppId = (steamAppId == null) ? "" : steamAppId;
    }

    public void setSteamOwners(String steamOwners) {
        this.steamOwners = (steamOwners == null) ? "" : steamOwners;
    }

    public void setLastPlayed(String lastPlayed) {
        if (lastPlayed == null || lastPlayed.isEmpty()) {
            this.lastPlayed = 0L;
            return;
        }
        try {
            this.lastPlayed = Long.parseLong(lastPlayed.trim());
        } catch (NumberFormatException e) {
            LimeLog.warning("Malformed LastPlayed: " + lastPlayed);
            this.lastPlayed = 0L;
        }
    }

    public void setPlaytimeMinutes(String playtime) {
        if (playtime == null || playtime.isEmpty()) {
            this.playtimeMinutes = 0L;
            return;
        }
        try {
            this.playtimeMinutes = Long.parseLong(playtime.trim());
        } catch (NumberFormatException e) {
            LimeLog.warning("Malformed Playtime: " + playtime);
            this.playtimeMinutes = 0L;
        }
    }

    public String getAppName() {
        return this.appName;
    }

    public int getAppId() {
        return this.appId;
    }

    public boolean isHdrSupported() {
        return this.hdrSupported;
    }

    public boolean isInitialized() {
        return this.initialized;
    }

    public String getSource() {
        return this.source;
    }

    public String getSteamAppId() {
        return this.steamAppId;
    }

    public String getSteamOwners() {
        return this.steamOwners;
    }

    public long getLastPlayed() {
        return this.lastPlayed;
    }

    public long getPlaytimeMinutes() {
        return this.playtimeMinutes;
    }

    /**
     * VipleStream: convenience flag for the sort comparator. Manual
     * (apps.json-declared) entries have an empty source; auto-imported
     * ones carry a non-empty tag like "steam". We pin the former to
     * the top of the list across all sort modes.
     */
    public boolean isManualEntry() {
        return this.source == null || this.source.isEmpty();
    }

    @Override
    public String toString() {
        StringBuilder str = new StringBuilder();
        str.append("Name: ").append(appName).append("\n");
        str.append("HDR Supported: ").append(hdrSupported ? "Yes" : "Unknown").append("\n");
        str.append("ID: ").append(appId).append("\n");
        if (source != null && !source.isEmpty()) {
            str.append("Source: ").append(source).append("\n");
        }
        if (steamAppId != null && !steamAppId.isEmpty()) {
            str.append("SteamAppId: ").append(steamAppId).append("\n");
        }
        if (lastPlayed > 0) {
            str.append("LastPlayed: ").append(lastPlayed).append("\n");
        }
        if (playtimeMinutes > 0) {
            str.append("Playtime: ").append(playtimeMinutes).append(" min\n");
        }
        return str.toString();
    }
}
