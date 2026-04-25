#pragma once

#include <QSettings>

class NvApp
{
public:
    NvApp() {}
    explicit NvApp(QSettings& settings);

    bool operator==(const NvApp& other) const
    {
        return id == other.id &&
                name == other.name &&
                hdrSupported == other.hdrSupported &&
                isAppCollectorGame == other.isAppCollectorGame &&
                hidden == other.hidden &&
                directLaunch == other.directLaunch &&
                // VipleStream: include source in equality so that when
                // a server upgrades to emit `<Source>` for the first
                // time, NvComputer::updateAppList detects a diff and
                // actually re-sorts appList. Without this, the list
                // is silently kept in whatever order it was first
                // loaded (typically alphabetical-only, which buries
                // the manual "Desktop" / "Steam Big Picture" pins
                // under a-starting Steam games).
                source == other.source &&
                steamAppId == other.steamAppId &&
                steamOwners == other.steamOwners &&
                lastPlayed == other.lastPlayed &&
                playtimeMinutes == other.playtimeMinutes;
    }

    bool operator!=(const NvApp& other) const
    {
        return !operator==(other);
    }

    bool isInitialized()
    {
        return id != 0 && !name.isEmpty();
    }

    void
    serialize(QSettings& settings) const;

    int id = 0;
    QString name;
    bool hdrSupported = false;
    bool isAppCollectorGame = false;
    bool hidden = false;
    bool directLaunch = false;

    // VipleStream Phase 2 of H (Steam auto-import): server tags apps
    // with their provenance. Empty = manual entry from apps.json (pinned
    // to the top of the app list); "steam" = auto-imported by Sunshine's
    // Steam scanner. Future: "epic" / "xbox" / etc.
    //
    // steamAppId + steamOwners are populated only when source == "steam"
    // — used by the profile-filter dropdown (Phase 3) to hide apps not
    // owned by the currently-selected Steam user.
    QString source;
    QString steamAppId;
    QString steamOwners;  ///< comma-separated SteamID3s
    // VipleStream H Phase 2.2: aggregated playtime stats (max
    // LastPlayed / sum Playtime across all owners on the host).
    // Zero means nobody has launched this app yet. Client uses these
    // for alternative sort modes (Recently-Played / Most-Played).
    qint64  lastPlayed = 0;       ///< unix timestamp
    qint64  playtimeMinutes = 0;  ///< all-time total
};

Q_DECLARE_METATYPE(NvApp)
