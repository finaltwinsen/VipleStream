#include "nvapp.h"

#define SER_APPNAME "name"
#define SER_APPID "id"
#define SER_APPHDR "hdr"
#define SER_APPCOLLECTOR "appcollector"
#define SER_HIDDEN "hidden"
#define SER_DIRECTLAUNCH "directlaunch"
// VipleStream H Phase 2: provenance tags persist across app restarts
// so the first fetch against a manual-only host on startup doesn't
// spuriously mark every app as changed (which would trigger an
// avoidable re-sort + signal storm). Missing on old saved state →
// defaults to empty strings, which is correct ("manual" provenance).
#define SER_APPSOURCE "source"
#define SER_APPSTEAMID "steam_app_id"
#define SER_APPSTEAMOWNERS "steam_owners"
#define SER_APPLASTPLAYED "last_played"
#define SER_APPPLAYTIME "playtime_minutes"

NvApp::NvApp(QSettings& settings)
{
    name = settings.value(SER_APPNAME).toString();
    id = settings.value(SER_APPID).toInt();
    hdrSupported = settings.value(SER_APPHDR).toBool();
    isAppCollectorGame = settings.value(SER_APPCOLLECTOR).toBool();
    hidden = settings.value(SER_HIDDEN).toBool();
    directLaunch = settings.value(SER_DIRECTLAUNCH).toBool();
    source = settings.value(SER_APPSOURCE).toString();
    steamAppId = settings.value(SER_APPSTEAMID).toString();
    steamOwners = settings.value(SER_APPSTEAMOWNERS).toString();
    lastPlayed = settings.value(SER_APPLASTPLAYED).toLongLong();
    playtimeMinutes = settings.value(SER_APPPLAYTIME).toLongLong();
}

void NvApp::serialize(QSettings& settings) const
{
    settings.setValue(SER_APPNAME, name);
    settings.setValue(SER_APPID, id);
    settings.setValue(SER_APPHDR, hdrSupported);
    settings.setValue(SER_APPCOLLECTOR, isAppCollectorGame);
    settings.setValue(SER_HIDDEN, hidden);
    settings.setValue(SER_DIRECTLAUNCH, directLaunch);
    settings.setValue(SER_APPSOURCE, source);
    settings.setValue(SER_APPSTEAMID, steamAppId);
    settings.setValue(SER_APPSTEAMOWNERS, steamOwners);
    settings.setValue(SER_APPLASTPLAYED, lastPlayed);
    settings.setValue(SER_APPPLAYTIME, playtimeMinutes);
}
