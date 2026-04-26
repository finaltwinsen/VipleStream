#include "richpresencemanager.h"

#include <QDebug>

RichPresenceManager::RichPresenceManager(StreamingPreferences& prefs, QString gameName)
    : m_DiscordActive(false)
{
#ifdef HAVE_DISCORD
    if (prefs.richPresence) {
        DiscordEventHandlers handlers = {};
        handlers.ready = discordReady;
        handlers.disconnected = discordDisconnected;
        handlers.errored = discordErrored;
        // VipleStream Discord application ID (registered v1.2.123).
        // The Rich Presence asset named "icon" referenced below as
        // largeImageKey must be uploaded to this App's Rich Presence
        // → Art Assets section in the Discord developer portal; if
        // the asset is missing, Discord still shows the status text
        // but with no icon.  Upstream Moonlight's ID was
        // 594668102021677159 — kept here for archaeology only.
        Discord_Initialize("1497869749244268545", &handlers, 0, nullptr);
        m_DiscordActive = true;
    }

    if (m_DiscordActive) {
        // v1.2.123 layout: game name promoted to `details` (the more
        // prominent top line in Discord's status card) so viewers see
        // the actual game name first; brand goes on the secondary
        // `state` line.  largeImageText surfaces "VipleStream" as the
        // hover tooltip on the icon, matching what App Name says.
        QByteArray detailsStr = (QString("Playing ") + gameName).toUtf8();

        DiscordRichPresence discordPresence = {};
        discordPresence.details = detailsStr.data();
        discordPresence.state = "via VipleStream";
        discordPresence.startTimestamp = time(nullptr);
        discordPresence.largeImageKey = "icon";
        discordPresence.largeImageText = "VipleStream";
        Discord_UpdatePresence(&discordPresence);
    }
#else
    Q_UNUSED(prefs)
    Q_UNUSED(gameName)
#endif
}

RichPresenceManager::~RichPresenceManager()
{
#ifdef HAVE_DISCORD
    if (m_DiscordActive) {
        Discord_ClearPresence();
        Discord_Shutdown();
    }
#endif
}

void RichPresenceManager::runCallbacks()
{
#ifdef HAVE_DISCORD
    if (m_DiscordActive) {
        Discord_RunCallbacks();
    }
#endif
}

#ifdef HAVE_DISCORD
void RichPresenceManager::discordReady(const DiscordUser* request)
{
    qInfo() << "Discord integration ready for user:" << request->username;
}

void RichPresenceManager::discordDisconnected(int errorCode, const char *message)
{
    qInfo() << "Discord integration disconnected:" << errorCode << message;
}

void RichPresenceManager::discordErrored(int errorCode, const char *message)
{
    qWarning() << "Discord integration error:" << errorCode << message;
}
#endif
