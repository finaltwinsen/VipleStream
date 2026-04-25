#pragma once

#include "identitymanager.h"
#include "nvapp.h"
#include "nvaddress.h"

#include <Limelight.h>

#include <QUrl>
#include <QNetworkAccessManager>
#include <QNetworkReply>

class NvComputer;

class NvDisplayMode
{
public:
    bool operator==(const NvDisplayMode& other) const
    {
        return width == other.width &&
                height == other.height &&
                refreshRate == other.refreshRate;
    }

    int width;
    int height;
    int refreshRate;
};
Q_DECLARE_TYPEINFO(NvDisplayMode, Q_PRIMITIVE_TYPE);

class GfeHttpResponseException : public std::exception
{
public:
    GfeHttpResponseException(int statusCode, QString message) :
        m_StatusCode(statusCode),
        m_StatusMessage(message.toUtf8())
    {

    }

    const char* what() const throw()
    {
        return m_StatusMessage.constData();
    }

    const char* getStatusMessage() const
    {
        return m_StatusMessage.constData();
    }

    int getStatusCode() const
    {
        return m_StatusCode;
    }

    QString toQString() const
    {
        return QString::fromUtf8(m_StatusMessage) + " (Error " + QString::number(m_StatusCode) + ")";
    }

private:
    int m_StatusCode;
    QByteArray m_StatusMessage;
};

class QtNetworkReplyException : public std::exception
{
public:
    QtNetworkReplyException(QNetworkReply::NetworkError error, QString errorText) :
        m_Error(error),
        m_ErrorText(errorText.toUtf8())
    {

    }

    const char* what() const throw()
    {
        return m_ErrorText.constData();
    }

    const char* getErrorText() const
    {
        return m_ErrorText.constData();
    }

    QNetworkReply::NetworkError getError() const
    {
        return m_Error;
    }

    QString toQString() const
    {
        return QString::fromUtf8(m_ErrorText) + " (Error " + QString::number(m_Error) + ")";
    }

private:
    QNetworkReply::NetworkError m_Error;
    QByteArray m_ErrorText;
};

// VipleStream H.4: a single Steam profile entry returned by
// /steamprofiles. Mirrors the server-side Sunshine `viple::steam::Profile`
// fields the client cares about.  `current=true` means this is the
// active Steam user on the host right now.
struct SteamProfile {
    QString steamId3;        // 32-bit account ID, used as the dropdown key
    QString steamId64;
    QString accountName;     // Steam username — passed to /steamswitch?account=
    QString personaName;     // Display name shown in the dropdown
    bool    rememberPassword = false;  // false ⇒ switch will fail with 409
    bool    mostRecent       = false;
    qint64  lastLogin        = 0;
    bool    current          = false;  // matches /steamprofiles `current_user`
};

class NvHTTP : public QObject
{
    Q_OBJECT

public:
    enum NvLogLevel {
        NVLL_NONE,
        NVLL_ERROR,
        NVLL_VERBOSE
    };

    explicit NvHTTP(NvAddress address, uint16_t httpsPort, QSslCertificate serverCert, QNetworkAccessManager* nam = nullptr);

    explicit NvHTTP(NvComputer* computer, QNetworkAccessManager* nam = nullptr);

    static
    int
    getCurrentGame(QString serverInfo);

    QString
    getServerInfo(NvLogLevel logLevel, bool fastFail = false);

    /**
     * VipleStream H.4: fetch /steamprofiles XML, parse into a profile
     * vector.  Throws GfeHttpResponseException on non-200 (e.g. 503 from
     * a vanilla Sunshine peer or a host with no Steam install).  The
     * caller (AppView) should catch and gracefully hide the dropdown in
     * that case.
     */
    QList<SteamProfile>
    getSteamProfiles();

    /**
     * VipleStream H.4 (v1.2.119): async switch.  Two endpoints replace
     * the old single blocking call:
     *
     * 1. `startSteamSwitch(account)` posts to /steamswitch and gets
     *    back a task id within ~50 ms — the host's HTTPS worker thread
     *    is freed immediately and won't starve /serverinfo polls.
     *
     * 2. `pollSteamSwitchStatus(taskId)` queries /steamswitch/status
     *    and returns the live state — caller polls every ~1 s until
     *    `state` is in a terminal value (done | error | already_active).
     *
     * The whole thing is wrapped by AppModel::requestSteamSwitch which
     * owns the poll loop and emits a progress signal for the UI.
     */
    struct SteamSwitchStatus {
        QString taskId;
        QString state;              // starting | shutting_down | logging_in | done | error | already_active
        QString message;            // human-readable progress text
        QString error;              // populated when state == error
        QString accountName;
        QString personaName;
        QString currentUserAfter;   // populated when state == done
        int     httpStatus = 0;     // final logical status (200 / 4xx / 5xx)
        qint64  elapsedMs  = -1;
        qint64  finishedMs = -1;    // -1 if still running

        bool isTerminal() const {
            return state == QLatin1String("done")
                || state == QLatin1String("error")
                || state == QLatin1String("already_active");
        }
    };

    SteamSwitchStatus
    startSteamSwitch(QString accountName);

    SteamSwitchStatus
    pollSteamSwitchStatus(QString taskId);

    static
    void
    verifyResponseStatus(QString xml);

    static
    QString
    getXmlString(QString xml,
                 QString tagName);

    static
    QByteArray
    getXmlStringFromHex(QString xml,
                        QString tagName);

    QString
    openConnectionToString(QUrl baseUrl,
                           QString command,
                           QString arguments,
                           int timeoutMs,
                           NvLogLevel logLevel = NvLogLevel::NVLL_VERBOSE);

    void setServerCert(QSslCertificate serverCert);

    void setAddress(NvAddress address);
    void setHttpsPort(uint16_t port);

    NvAddress address();

    QSslCertificate serverCert();

    uint16_t httpPort();

    uint16_t httpsPort();

    static
    QVector<int>
    parseQuad(QString quad);

    void
    quitApp();

    void
    startApp(QString verb,
             bool isGfe,
             int appId,
             PSTREAM_CONFIGURATION streamConfig,
             bool sops,
             bool localAudio,
             int gamepadMask,
             bool persistGameControllersOnDisconnect,
             QString& rtspSessionUrl);

    QVector<NvApp>
    getAppList();

    QImage
    getBoxArt(int appId);

    static
    QVector<NvDisplayMode>
    getDisplayModeList(QString serverInfo);

    QUrl m_BaseUrlHttp;
    QUrl m_BaseUrlHttps;
private:
    void
    handleSslErrors(QNetworkReply* reply, const QList<QSslError>& errors);

    QNetworkReply*
    openConnection(QUrl baseUrl,
                   QString command,
                   QString arguments,
                   int timeoutMs,
                   NvLogLevel logLevel);

    NvAddress m_Address;
    QNetworkAccessManager* m_Nam;
    QSslCertificate m_ServerCert;
};
