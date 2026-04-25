#include "appmodel.h"

// VipleStream H.4 v1.2.119: poll-loop in requestSteamSwitch needs these.
#include <QCoreApplication>
#include <QDateTime>
#include <QEventLoop>
#include <QThread>

AppModel::AppModel(QObject *parent)
    : QAbstractListModel(parent)
{
    connect(&m_BoxArtManager, &BoxArtManager::boxArtLoadComplete,
            this, &AppModel::handleBoxArtLoaded);
}

void AppModel::initialize(ComputerManager* computerManager, int computerIndex, bool showHiddenGames)
{
    m_ComputerManager = computerManager;
    connect(m_ComputerManager, &ComputerManager::computerStateChanged,
            this, &AppModel::handleComputerStateChanged);

    Q_ASSERT(computerIndex < m_ComputerManager->getComputers().count());
    m_Computer = m_ComputerManager->getComputers().at(computerIndex);
    m_CurrentGameId = m_Computer->currentGameId;
    m_ShowHiddenGames = showHiddenGames;
    {
        QReadLocker lock(&m_Computer->lock);
        m_LastKnownPeerIsViple = m_Computer->isVipleStreamPeer;
    }

    updateAppList(m_Computer->appList);
}

int AppModel::getRunningAppId()
{
    return m_CurrentGameId;
}

QString AppModel::getRunningAppName()
{
    if (m_CurrentGameId != 0) {
        for (int i = 0; i < m_AllApps.count(); i++) {
            if (m_AllApps[i].id == m_CurrentGameId) {
                return m_AllApps[i].name;
            }
        }
    }

    return nullptr;
}

Session* AppModel::createSessionForApp(int appIndex)
{
    Q_ASSERT(appIndex < m_VisibleApps.count());
    NvApp app = m_VisibleApps.at(appIndex);

    return new Session(m_Computer, app);
}

int AppModel::getDirectLaunchAppIndex()
{
    for (int i = 0; i < m_VisibleApps.count(); i++) {
        if (m_VisibleApps[i].directLaunch) {
            return i;
        }
    }

    return -1;
}

int AppModel::featuredAppIndex() const
{
    // Prefer a currently-running game — that's the one the user is
    // resuming / actively streaming.
    if (m_CurrentGameId != 0) {
        for (int i = 0; i < m_VisibleApps.count(); ++i) {
            if (m_VisibleApps[i].id == m_CurrentGameId) {
                return i;
            }
        }
    }
    // Otherwise the direct-launch app if the user pinned one.
    for (int i = 0; i < m_VisibleApps.count(); ++i) {
        if (m_VisibleApps[i].directLaunch) {
            return i;
        }
    }
    // Otherwise whatever's first on the list.
    return m_VisibleApps.isEmpty() ? -1 : 0;
}

QString AppModel::nameAt(int index) const
{
    if (index < 0 || index >= m_VisibleApps.count()) {
        return QString();
    }
    return m_VisibleApps[index].name;
}

int AppModel::rowCount(const QModelIndex &parent) const
{
    // For list models only the root node (an invalid parent) should return the list's size. For all
    // other (valid) parents, rowCount() should return 0 so that it does not become a tree model.
    if (parent.isValid())
        return 0;

    return m_VisibleApps.count();
}

QVariant AppModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return QVariant();

    Q_ASSERT(index.row() < m_VisibleApps.count());
    NvApp app = m_VisibleApps.at(index.row());

    switch (role)
    {
    case NameRole:
        return app.name;
    case RunningRole:
        return m_Computer->currentGameId == app.id;
    case BoxArtRole:
        // FIXME: const-correctness
        return const_cast<BoxArtManager&>(m_BoxArtManager).loadBoxArt(m_Computer, app);
    case HiddenRole:
        return app.hidden;
    case AppIdRole:
        return app.id;
    case DirectLaunchRole:
        return app.directLaunch;
    case AppCollectorGameRole:
        return app.isAppCollectorGame;
    default:
        return QVariant();
    }
}

QHash<int, QByteArray> AppModel::roleNames() const
{
    QHash<int, QByteArray> names;

    names[NameRole] = "name";
    names[RunningRole] = "running";
    names[BoxArtRole] = "boxart";
    names[HiddenRole] = "hidden";
    names[AppIdRole] = "appid";
    names[DirectLaunchRole] = "directLaunch";
    names[AppCollectorGameRole] = "appCollectorGame";

    return names;
}

void AppModel::quitRunningApp()
{
    m_ComputerManager->quitRunningApp(m_Computer);
}

bool AppModel::isAppCurrentlyVisible(const NvApp& app)
{
    for (const NvApp& visibleApp : std::as_const(m_VisibleApps)) {
        if (app.id == visibleApp.id) {
            return true;
        }
    }

    return false;
}

QVector<NvApp> AppModel::getVisibleApps(const QVector<NvApp>& appList)
{
    QVector<NvApp> visibleApps;

    for (const NvApp& app : appList) {
        // Don't immediately hide games that were previously visible. This
        // allows users to easily uncheck the "Hide App" checkbox if they
        // check it by mistake.
        if (m_ShowHiddenGames || !app.hidden || isAppCurrentlyVisible(app)) {
            visibleApps.append(app);
        }
    }

    return visibleApps;
}

void AppModel::updateAppList(QVector<NvApp> newList)
{
    m_AllApps = newList;

    QVector<NvApp> newVisibleList = getVisibleApps(newList);

    // Process removals and updates first
    for (int i = 0; i < m_VisibleApps.count(); i++) {
        const NvApp& existingApp = m_VisibleApps.at(i);

        bool found = false;
        for (const NvApp& newApp : std::as_const(newVisibleList)) {
            if (existingApp.id == newApp.id) {
                // If the data changed, update it in our list
                if (existingApp != newApp) {
                    m_VisibleApps.replace(i, newApp);
                    emit dataChanged(createIndex(i, 0), createIndex(i, 0));
                }

                found = true;
                break;
            }
        }

        if (!found) {
            beginRemoveRows(QModelIndex(), i, i);
            m_VisibleApps.removeAt(i);
            endRemoveRows();
            i--;
        }
    }

    // Process additions now.
    //
    // VipleStream H Phase 2: AppModel runs its own insertion-sort here
    // to build m_VisibleApps one app at a time. The comparator MUST
    // match NvComputer::sortAppList()'s — otherwise the "pre-sorted"
    // appList handed to us gets re-sorted back to the upstream
    // alphabetical-only order, defeating the source/playtime-based
    // ordering. Phase 2.2 centralises this as NvComputer::appLessThan
    // so both call sites use identical rules (including the current
    // AppSortMode preference at call time).
    for (const NvApp& newApp : std::as_const(newVisibleList)) {
        int insertionIndex = m_VisibleApps.size();
        bool found = false;

        for (int i = 0; i < m_VisibleApps.count(); i++) {
            const NvApp& existingApp = m_VisibleApps.at(i);

            if (existingApp.id == newApp.id) {
                found = true;
                break;
            }
            else if (NvComputer::appLessThan(newApp, existingApp)) {
                insertionIndex = i;
                break;
            }
        }

        if (!found) {
            beginInsertRows(QModelIndex(), insertionIndex, insertionIndex);
            m_VisibleApps.insert(insertionIndex, newApp);
            endInsertRows();
        }
    }

    Q_ASSERT(newVisibleList == m_VisibleApps);
}

void AppModel::setAppHidden(int appIndex, bool hidden)
{
    Q_ASSERT(appIndex < m_VisibleApps.count());
    int appId = m_VisibleApps.at(appIndex).id;

    {
        QWriteLocker lock(&m_Computer->lock);

        for (NvApp& app : m_Computer->appList) {
            if (app.id == appId) {
                app.hidden = hidden;
                break;
            }
        }
    }

    m_ComputerManager->clientSideAttributeUpdated(m_Computer);
}

void AppModel::setAppDirectLaunch(int appIndex, bool directLaunch)
{
    Q_ASSERT(appIndex < m_VisibleApps.count());
    int appId = m_VisibleApps.at(appIndex).id;

    {
        QWriteLocker lock(&m_Computer->lock);

        for (NvApp& app : m_Computer->appList) {
            if (directLaunch) {
                // We must clear direct launch from all other apps
                // to set it on the new app.
                app.directLaunch = app.id == appId;
            }
            else if (app.id == appId) {
                // If we're clearing direct launch, we're done once we
                // find our matching app ID.
                app.directLaunch = false;
                break;
            }
        }
    }

    m_ComputerManager->clientSideAttributeUpdated(m_Computer);
}

void AppModel::handleComputerStateChanged(NvComputer* computer)
{
    // Ignore updates for computers that aren't ours
    if (computer != m_Computer) {
        return;
    }

    // VipleStream H.4: detect isVipleStreamPeer flips so QML can re-run
    // its Steam-profile-dropdown populate when the first /serverinfo
    // poll arrives after AppView's initial Component.onCompleted has
    // already given up.  Read under the same lock the rest of the
    // ASSIGN_IF_CHANGED path uses inside NvComputer::update.
    {
        QReadLocker lock(&m_Computer->lock);
        bool nowIsViple = m_Computer->isVipleStreamPeer;
        if (nowIsViple != m_LastKnownPeerIsViple) {
            m_LastKnownPeerIsViple = nowIsViple;
            qInfo() << "[H.4] isVipleStreamPeer changed for"
                    << m_Computer->name << "→" << nowIsViple;
            emit peerIsVipleChanged();
        }
    }

    // If the computer has gone offline or we've been unpaired,
    // signal the UI so we can go back to the PC view.
    if (m_Computer->state == NvComputer::CS_OFFLINE ||
            m_Computer->pairState == NvComputer::PS_NOT_PAIRED) {
        emit computerLost();
        return;
    }

    // First, process additions/removals from the app list. This
    // is required because the new game may now be running, so
    // we can't check that first.
    if (computer->appList != m_AllApps) {
        updateAppList(computer->appList);
    }

    // Finally, process changes to the active app
    if (computer->currentGameId != m_CurrentGameId) {
        // First, invalidate the running state of newly running game
        for (int i = 0; i < m_VisibleApps.count(); i++) {
            if (m_VisibleApps[i].id == computer->currentGameId) {
                emit dataChanged(createIndex(i, 0),
                                 createIndex(i, 0),
                                 QVector<int>() << RunningRole);
                break;
            }
        }

        // Next, invalidate the running state of the old game (if it exists)
        if (m_CurrentGameId != 0) {
            for (int i = 0; i < m_VisibleApps.count(); i++) {
                if (m_VisibleApps[i].id == m_CurrentGameId) {
                    emit dataChanged(createIndex(i, 0),
                                     createIndex(i, 0),
                                     QVector<int>() << RunningRole);
                    break;
                }
            }
        }

        // Now update our internal state
        m_CurrentGameId = m_Computer->currentGameId;
    }
}

void AppModel::handleBoxArtLoaded(NvComputer* computer, NvApp app, QUrl /* image */)
{
    Q_ASSERT(computer == m_Computer);

    int index = m_VisibleApps.indexOf(app);

    // Make sure we're not delivering a callback to an app that's already been removed
    if (index >= 0) {
        // Let our view know the box art data has changed for this app
        emit dataChanged(createIndex(index, 0),
                         createIndex(index, 0),
                         QVector<int>() << BoxArtRole);
    }
    else {
        qWarning() << "App not found for box art callback:" << app.name;
    }
}

// ── VipleStream H.4: Steam profile dropdown helpers ─────────────────────

bool AppModel::peerSupportsSteamProfiles() const
{
    if (!m_Computer) return false;
    QReadLocker lock(&m_Computer->lock);
    return m_Computer->isVipleStreamPeer;
}

bool AppModel::refreshSteamProfiles()
{
    if (!m_Computer) return false;
    if (!peerSupportsSteamProfiles()) {
        m_SteamProfiles.clear();
        return false;
    }
    NvHTTP http(m_Computer);
    try {
        QList<SteamProfile> fresh = http.getSteamProfiles();
        bool changed = (fresh.size() != m_SteamProfiles.size());
        if (!changed) {
            for (int i = 0; i < fresh.size(); ++i) {
                if (fresh[i].steamId3 != m_SteamProfiles[i].steamId3 ||
                    fresh[i].current  != m_SteamProfiles[i].current) {
                    changed = true;
                    break;
                }
            }
        }
        m_SteamProfiles = fresh;
        return changed;
    } catch (const GfeHttpResponseException& e) {
        qWarning() << "[H.4] /steamprofiles failed:" << e.getStatusCode() << e.getStatusMessage();
        m_SteamProfiles.clear();
        return false;
    } catch (const QtNetworkReplyException& e) {
        qWarning() << "[H.4] /steamprofiles network error:" << e.toQString();
        m_SteamProfiles.clear();
        return false;
    }
}

int AppModel::steamProfileCount() const
{
    return m_SteamProfiles.size();
}

QString AppModel::steamProfilePersona(int index) const
{
    if (index < 0 || index >= m_SteamProfiles.size()) return {};
    return m_SteamProfiles[index].personaName;
}

QString AppModel::steamProfileAccount(int index) const
{
    if (index < 0 || index >= m_SteamProfiles.size()) return {};
    return m_SteamProfiles[index].accountName;
}

bool AppModel::steamProfileSwitchable(int index) const
{
    if (index < 0 || index >= m_SteamProfiles.size()) return false;
    return m_SteamProfiles[index].rememberPassword;
}

int AppModel::currentSteamProfileIndex() const
{
    for (int i = 0; i < m_SteamProfiles.size(); ++i) {
        if (m_SteamProfiles[i].current) return i;
    }
    return -1;
}

bool AppModel::requestSteamSwitch(QString accountName)
{
    m_LastSteamSwitchError.clear();
    if (!m_Computer) {
        m_LastSteamSwitchError = tr("No host selected");
        return false;
    }
    NvHTTP http(m_Computer);

    // 1. Kick off the async task on the host. Returns within ms.
    NvHTTP::SteamSwitchStatus status;
    try {
        status = http.startSteamSwitch(accountName);
    } catch (const GfeHttpResponseException& e) {
        m_LastSteamSwitchError = QString::fromUtf8(e.getStatusMessage());
        return false;
    } catch (const QtNetworkReplyException& e) {
        m_LastSteamSwitchError = e.toQString();
        return false;
    }

    emit steamSwitchProgress(status.state, status.message);

    // 2. Already-active short-circuit — nothing to wait for.
    if (status.state == QLatin1String("already_active")) {
        refreshSteamProfiles();
        return true;
    }

    // 3. Validate that we got a usable task id back.
    if (status.taskId.isEmpty()) {
        m_LastSteamSwitchError = tr("Server returned no task id");
        return false;
    }

    // 4. Poll loop.  150 s safety deadline matches the server's own
    //    15 s shutdown + 90 s login + slack budget.  Drives
    //    QCoreApplication::processEvents() between polls so QML stays
    //    responsive (busy overlay animation, signal delivery).
    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + 150000;
    int consecutiveErrors = 0;
    while (QDateTime::currentMSecsSinceEpoch() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        QThread::msleep(900);

        try {
            status = http.pollSteamSwitchStatus(status.taskId);
            consecutiveErrors = 0;
        } catch (const GfeHttpResponseException& e) {
            // 404 means the task was GC'd (server restart, or finished
            // >60 s ago).  Anything else is transient — retry a few times
            // before giving up.
            if (e.getStatusCode() == 404) {
                m_LastSteamSwitchError = tr("Switch task disappeared on host");
                return false;
            }
            if (++consecutiveErrors >= 5) {
                m_LastSteamSwitchError = QString::fromUtf8(e.getStatusMessage());
                return false;
            }
            continue;
        } catch (const QtNetworkReplyException& e) {
            if (++consecutiveErrors >= 5) {
                m_LastSteamSwitchError = e.toQString();
                return false;
            }
            continue;
        }

        emit steamSwitchProgress(status.state, status.message);

        if (status.isTerminal()) {
            if (status.state == QLatin1String("done")
             || status.state == QLatin1String("already_active")) {
                refreshSteamProfiles();
                return true;
            }
            m_LastSteamSwitchError = !status.error.isEmpty() ? status.error : status.message;
            return false;
        }
    }

    m_LastSteamSwitchError = tr("Switch timed out (>150s without server reaching a terminal state)");
    return false;
}

QString AppModel::lastSteamSwitchError() const
{
    return m_LastSteamSwitchError;
}
