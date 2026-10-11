#pragma once

#include "vrpreflight.h"

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QThread>
#include <QTimer>

class QTcpSocket;

// VipleStream 2.0 §VR-PREFLIGHT：量一次到主機的往返時間——對主機已知的位址與埠做一次 TCP 連線，
// 從送出 SYN 到連線建立＝一次往返。不送任何資料，連上就關。
//
// 活在 VrPreflightCheck 開的執行緒裡，那條執行緒的事件迴圈沒有別的事：GUI 執行緒忙（畫面、同步的 HTTP）
// 的時候量到的時間不會被拖長。連線本身是非同步的，不會卡住任何執行緒。
class VrRttProbe : public QObject
{
    Q_OBJECT

public:
    explicit VrRttProbe(QObject* parent = nullptr);

public slots:
    // 開始量一次。上一次還沒結束的直接丟掉（不回報）
    void start(quint64 id, const QString& host, quint16 port, int timeoutMs);

    // 丟掉進行中的那一次（不回報）
    void abort();

signals:
    // ok＝連上了，rttMs 是往返時間（四捨五入到毫秒）；否則 why 是原因（"timeout"、"refused"…）
    void finished(quint64 id, bool ok, int rttMs, const QString& why);

private:
    void finish(QTcpSocket* socket, bool ok, const QString& why);

    QTcpSocket* m_Socket = nullptr;   // 進行中的那一次；nullptr＝沒有
    QTimer* m_Timeout = nullptr;
    QElapsedTimer m_Elapsed;
    quint64 m_Id = 0;
};

// §VR-PREFLIGHT：開 VR 串流子行程之前的往返時間檢查。判斷規則在 vrpreflight.h（Gate），這裡負責量測、計時
// 與 log；結果用 proceed()（可以開了）或 cancelled() 通知，介面要顯示的狀態變了就發 changed()。
// 全部在 GUI 執行緒使用。
class VrPreflightCheck : public QObject
{
    Q_OBJECT

public:
    explicit VrPreflightCheck(QObject* parent = nullptr);
    ~VrPreflightCheck() override;

    // dev（--vr-test-preflight）：之後的每一次檢查都照這份腳本給結果，不連線。空字串＝關。
    // 格式不對回 false（*error 是英文說明）、維持原狀。只存在行程裡，不寫進設定。
    static bool setTestScript(const QString& spec, QString* error = nullptr);

    bool busy() const { return m_Gate.busy(); }
    bool checking() const { return m_Gate.phase() == vrpreflight::Phase::Checking; }   // 開頭那幾次還在量
    bool waiting() const { return m_Gate.phase() == vrpreflight::Phase::Waiting; }     // 太慢：等使用者決定，同時繼續量
    int lastRttMs() const { return m_Gate.lastRttMs(); }           // 最近一次量到的；量不到或還沒量＝-1
    int measuredRttMs() const { return m_Gate.measuredRttMs(); }   // 判定太慢時的往返時間；沒有＝-1
    int goodStreak() const { return m_Gate.goodStreak(); }

    // 開始檢查 host:port。已經在檢查中回 false（什麼都不做）；否則回 true，之後一定會發 proceed() 或
    // cancelled() 其中一個、恰好一次（沒有可以量的目標時在這個呼叫裡就發 proceed()）
    bool begin(const QString& host, int port);

    // 使用者選「仍要開始」；不在檢查中時不做事
    void startAnyway();

    // 這一次不開了；who 只給 log 用。不在檢查中時不做事
    void cancel(const char* who);

signals:
    void changed();
    void proceed();
    void cancelled();

    // 給量測執行緒（內部使用）
    void probeRequested(quint64 id, const QString& host, quint16 port, int timeoutMs);
    void probeAbortRequested();

private:
    void apply(const vrpreflight::Step& step);
    void startProbe();
    void stopProbing();
    void onProbeFinished(quint64 id, bool ok, int rttMs, const QString& why);
    void ensureWorker();
    qint64 nowMs() const { return m_Clock.elapsed(); }

    vrpreflight::Gate m_Gate;
    vrpreflight::TestScript m_Script;   // 這一次檢查用的腳本（dev）；沒開＝真的量
    QThread m_Thread;
    VrRttProbe* m_Probe = nullptr;      // 活在 m_Thread；第一次真的要量時才建立
    QTimer m_NextProbe;                 // Waiting 時等下一次量
    QElapsedTimer m_Clock;
    QString m_Host;
    quint16 m_Port = 0;
    quint64 m_ProbeId = 0;              // 最近發出去的那一次；回來的 id 不同＝已經作廢
    bool m_ProbePending = false;
};
