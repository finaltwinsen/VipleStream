#pragma once

#include <QObject>
#include <QVariant>

class ComputerManager;
class NvComputer;
class Session;
class StreamingPreferences;

namespace CliStartStream
{

class Event;
class LauncherPrivate;

class Launcher : public QObject
{
    Q_OBJECT
    Q_DECLARE_PRIVATE_D(m_DPtr, Launcher)

public:
    explicit Launcher(QString computer, QString app,
                      StreamingPreferences* preferences,
                      QObject *parent = nullptr);
    ~Launcher();
    Q_INVOKABLE void execute(ComputerManager *manager);
    Q_INVOKABLE void quitRunningApp();
    Q_INVOKABLE bool isExecuted() const;

    // --takeover（§VR-LAUNCHER）：主機被其他裝置佔用時照樣開 session（Session 帶 takeover），
    // 不走「先結束別的 app」的確認
    void setTakeover(bool takeover);

signals:
    void searchingComputer();
    void searchingApp();
    void sessionCreated(QString appName, Session *session);
    void failed(QString text);
    void appQuitRequired(QString appName);

private slots:
    void onComputerFound(NvComputer *computer);
    void onComputerUpdated(NvComputer *computer);
    void onTimeout();
    void onQuitAppCompleted(QVariant error);

private:
    QScopedPointer<LauncherPrivate> m_DPtr;
};

}
