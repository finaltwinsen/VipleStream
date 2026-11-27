#pragma once

#include "computermanager.h"
#include <QDir>
#include <QHash>
#include <QImage>
#include <QSet>
#include <QThreadPool>
#include <QRunnable>

class BoxArtManager : public QObject
{
    Q_OBJECT

    friend class NetworkBoxArtLoadTask;

public:
    explicit BoxArtManager(QObject *parent = nullptr);

    QUrl
    loadBoxArt(NvComputer* computer, NvApp& app);

    static
    void
    deleteBoxArt(NvComputer* computer);

signals:
    void
    boxArtLoadComplete(NvComputer* computer, NvApp app, QUrl image);

public slots:

private slots:
    void
    handleBoxArtLoadComplete(NvComputer* computer, NvApp app, QUrl image);

private:
    QUrl
    loadBoxArtFromNetwork(NvComputer* computer, int appId);

    QString
    getFilePathForBoxArt(NvComputer* computer, int appId);

    // §REVIEW-1127 qt_ui #3：同一張封面「進行中」的去重與失敗的負面快取（key＝主機指標/appId）。
    // 原本每次 data(BoxArtRole) 沒命中就再開一個下載工作，失敗也不記，delegate 重建時會重複打 host。
    // 兩者都只在主執行緒存取（loadBoxArt 由 model 呼叫，完成的 slot 經 queued connection 回到主執行緒）。
    static QString boxArtKey(NvComputer* computer, int appId);
    QSet<QString> m_InFlight;
    QHash<QString, qint64> m_FailedAtMs;  // 失敗的時刻（QDateTime::currentMSecsSinceEpoch）

    QDir m_BoxArtDir;
    QThreadPool m_ThreadPool;
};
