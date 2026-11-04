#include "compatfetcher.h"
#include "path.h"

#include <QNetworkReply>
#include <QSettings>

#define COMPAT_VERSION "v1"
#define COMPAT_KEY "latestsupportedversion-"

CompatFetcher::CompatFetcher(QObject *parent) :
    QObject(parent)
{
    m_Nam = new QNetworkAccessManager(this);

    // Never communicate over HTTP
    m_Nam->setStrictTransportSecurityEnabled(true);

    // Allow HTTP redirects
    m_Nam->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);

    connect(m_Nam, &QNetworkAccessManager::finished,
            this, &CompatFetcher::handleCompatInfoFetched);
}

void CompatFetcher::start()
{
    // VipleStream: disable compat check against official Moonlight server
    return;

    if (!m_Nam) {
        Q_ASSERT(m_Nam);
        return;
    }

#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0) && QT_VERSION < QT_VERSION_CHECK(5, 15, 1) && !defined(QT_NO_BEARERMANAGEMENT)
    // HACK: Set network accessibility to work around QTBUG-80947 (introduced in Qt 5.14.0 and fixed in Qt 5.15.1)
    QT_WARNING_PUSH
    QT_WARNING_DISABLE_DEPRECATED
    m_Nam->setNetworkAccessible(QNetworkAccessManager::Accessible);
    QT_WARNING_POP
#endif

    QUrl url("https://moonlight-stream.org/compatibility/" COMPAT_VERSION);
    QNetworkRequest request(url);

#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, true);
#else
    request.setAttribute(QNetworkRequest::HTTP2AllowedAttribute, true);
#endif

    // We'll get a callback when this is finished
    m_Nam->get(request);
}

bool CompatFetcher::isGfeVersionSupported(QString gfeVersion)
{
    // §REVIEW-1104：遠端相容性檢查已停用，殘留的 latestsupportedversion-v1 仍會把較新的 GfeVersion 拒開串流
    Q_UNUSED(gfeVersion);
    QSettings settings;
    settings.remove(COMPAT_KEY COMPAT_VERSION);
    return true;
}

void CompatFetcher::handleCompatInfoFetched(QNetworkReply* reply)
{
    Q_ASSERT(reply->isFinished());

    // Delete the QNetworkAccessManager to free resources and
    // prevent the bearer plugin from polling in the background.
    m_Nam->deleteLater();
    m_Nam = nullptr;

    if (reply->error() == QNetworkReply::NoError) {
        // Queue the reply for deletion
        reply->deleteLater();

        QString version = QString(reply->readAll()).trimmed();

        QSettings settings;
        settings.setValue(COMPAT_KEY COMPAT_VERSION, version);

        qInfo() << "Latest supported GFE server:" << version;
    }
    else {
        qWarning() << "Failed to download latest compatibility data:" << reply->error();
        reply->deleteLater();
    }
}
