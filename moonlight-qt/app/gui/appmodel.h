#pragma once

#include "backend/boxartmanager.h"
#include "backend/computermanager.h"
#include "backend/nvhttp.h"   // VipleStream H.4: SteamProfile struct
#include "streaming/session.h"

#include <QAbstractListModel>
#include <QList>

class AppModel : public QAbstractListModel
{
    Q_OBJECT

    enum Roles
    {
        NameRole = Qt::UserRole,
        RunningRole,
        BoxArtRole,
        HiddenRole,
        AppIdRole,
        DirectLaunchRole,
        AppCollectorGameRole,
    };

public:
    explicit AppModel(QObject *parent = nullptr);

    // Must be called before any QAbstractListModel functions
    Q_INVOKABLE void initialize(ComputerManager* computerManager, int computerIndex, bool showHiddenGames);

    Q_INVOKABLE Session* createSessionForApp(int appIndex);

    Q_INVOKABLE int getDirectLaunchAppIndex();

    Q_INVOKABLE int getRunningAppId();

    Q_INVOKABLE QString getRunningAppName();

    Q_INVOKABLE void quitRunningApp();

    Q_INVOKABLE void setAppHidden(int appIndex, bool hidden);

    Q_INVOKABLE void setAppDirectLaunch(int appIndex, bool directLaunch);

    // VipleStream: §03 Bold library-hero helpers. Prefer the
    // currently-running app, fall back to the direct-launch app,
    // fall back to the first visible app, -1 if the list is empty.
    Q_INVOKABLE int featuredAppIndex() const;
    Q_INVOKABLE QString nameAt(int index) const;

    // VipleStream H.4: Steam profile dropdown helpers.  All Q_INVOKABLE
    // because AppView.qml drives them directly — there's no persistent
    // ProfileModel; the dropdown re-fetches each time the view opens.
    //
    // peerSupportsSteamProfiles() returns true only for VipleStream-Server
    // peers (cheap check, no network).  AppView uses it to decide whether
    // to show the dropdown at all.
    //
    // refreshSteamProfiles() does a /steamprofiles HTTPS call and stores
    // the result in m_SteamProfiles.  Returns true if the list changed.
    // Synchronous — keep the call site in a non-blocking Component.onCompleted
    // or behind a button press; the call is fast (cached server-side).
    //
    // The dropdown reads via steamProfileCount(), steamProfilePersona(idx),
    // steamProfileAccount(idx), steamProfileSwitchable(idx),
    // currentSteamProfileIndex() (-1 if no profile is "active" or peer
    // hasn't been refreshed yet).
    //
    // requestSteamSwitch(account) is the trigger.  v1.2.119 turned this
    // into an async-on-server poll loop: client calls /steamswitch which
    // returns within ms with a task id, then polls /steamswitch/status
    // every ~1 s (driving QCoreApplication::processEvents in between so
    // QML stays responsive and the steamSwitchProgress signal fires for
    // each state transition).  Returns true on success; on failure
    // caller should grab lastSteamSwitchError() for the toast.
    Q_INVOKABLE bool peerSupportsSteamProfiles() const;
    Q_INVOKABLE bool refreshSteamProfiles();
    Q_INVOKABLE int  steamProfileCount() const;
    Q_INVOKABLE QString steamProfilePersona(int index) const;
    Q_INVOKABLE QString steamProfileAccount(int index) const;
    Q_INVOKABLE bool steamProfileSwitchable(int index) const;
    Q_INVOKABLE int  currentSteamProfileIndex() const;
    Q_INVOKABLE bool requestSteamSwitch(QString accountName);
    Q_INVOKABLE QString lastSteamSwitchError() const;

    QVariant data(const QModelIndex &index, int role) const override;

    int rowCount(const QModelIndex &parent) const override;

    virtual QHash<int, QByteArray> roleNames() const override;

private slots:
    void handleComputerStateChanged(NvComputer* computer);

    void handleBoxArtLoaded(NvComputer* computer, NvApp app, QUrl image);

signals:
    void computerLost();

    // VipleStream H.4: emitted when m_Computer->isVipleStreamPeer flips
    // (typically false → true after the first /serverinfo poll completes).
    // QML uses this to retry populating the Steam profile dropdown when
    // the AppView opened before the polling cycle finished.
    void peerIsVipleChanged();

    // VipleStream H.4 (v1.2.119): emitted on each /steamswitch/status
    // poll while requestSteamSwitch is in flight.  `state` is the raw
    // server state ("starting" / "shutting_down" / "logging_in" / etc),
    // `message` is a human-readable progress string the AppView busy
    // overlay can display unmodified.
    void steamSwitchProgress(const QString &state, const QString &message);

private:
    void updateAppList(QVector<NvApp> newList);

    QVector<NvApp> getVisibleApps(const QVector<NvApp>& appList);

    bool isAppCurrentlyVisible(const NvApp& app);

    NvComputer* m_Computer;
    BoxArtManager m_BoxArtManager;
    ComputerManager* m_ComputerManager;
    QVector<NvApp> m_VisibleApps, m_AllApps;
    int m_CurrentGameId;
    bool m_ShowHiddenGames;

    // VipleStream H.4: Steam profile cache + last switch error string.
    QList<SteamProfile> m_SteamProfiles;
    QString m_LastSteamSwitchError;
    bool    m_LastKnownPeerIsViple = false;  // for change detection in
                                              // handleComputerStateChanged
};
