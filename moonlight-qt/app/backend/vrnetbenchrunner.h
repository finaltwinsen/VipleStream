#pragma once

#include "backend/computermanager.h"
#include "backend/vrlauncher.h"
#include "streaming/vr/vrnetbench.h"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QVariantList>


// §VR-NETBENCH（2026-11-09）：設定頁「測試網路」的執行器（GUI 執行緒）。全程自動、使用者不必操作：
//   1. 找主機（線上、有 PCVR 的那一台）與頭盔網路服務的狀態（viplestream-netmode.service，見 vrnetmode.h）
//   2. 依序 B（家用 Wi-Fi＋轉接器）→ C（只用轉接器：請服務暫停家用 Wi-Fi，期間每秒送心跳）→ B（接回家用 Wi-Fi）
//   3. 每個狀態跑解析度階梯（NetBench::ladderEyes()），一階一個子行程（VrLauncher::launchNetBenchStep），
//      撐不住 144 Hz 就不再往上
//   4. NetBench::compareLadders() 選網路，存成這個地點（家用 Wi-Fi 名稱的雜湊）的選擇——設定「自動」時串流照這個選擇
// 取消：目前這一階跑完（子行程自己會結束）就停，交還家用 Wi-Fi。
class VrNetBenchRunner : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool running READ running NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(int progress READ progress NOTIFY changed)        // 0～100
    Q_PROPERTY(QVariantList rows READ rows NOTIFY changed)       // 每一階一列：state、eye、held、deficit、mtp95、mbps、note
    Q_PROPERTY(QString verdict READ verdict NOTIFY changed)      // 結果說明（空＝還沒有）
    Q_PROPERTY(QString lastSaved READ lastSaved NOTIFY changed)  // 這台頭盔上次測試的結果（日期＋選擇）
    Q_PROPERTY(bool devAutoStart READ devAutoStart CONSTANT)     // dev（--dev-netbench）：VR 介面一有主機就自動開始

public:
    explicit VrNetBenchRunner(QObject* parent = nullptr);
    ~VrNetBenchRunner() override;

    bool running() const { return m_Phase != Phase::Idle; }
    QString status() const { return m_Status; }
    int progress() const;
    QVariantList rows() const;
    QString verdict() const { return m_Verdict; }
    QString lastSaved() const;
    bool devAutoStart() const { return s_DevAutoStart; }
    static void setDevAutoStart(bool on) { s_DevAutoStart = on; }

    // launcher＝QML 的 VrLauncher 單例、computerManager＝QML 的 ComputerManager 單例
    Q_INVOKABLE bool start(QObject* launcher, QObject* computerManager);
    Q_INVOKABLE void cancel();

signals:
    void changed();

private:
    enum class Phase { Idle, SwitchNet, Settle, Step, Finish };
    struct NetStatus {
        bool fresh = false;
        bool apStation = false;
        QString applied, loc, wlan0, err;
    };

    void tick();
    void beginState();
    void launchStep();
    void onChildFinished(int exitCode, const QString& error);
    void finish(const QString& failure);
    void setStatus(const QString& s);
    void writeRequest(bool adapterOnly);
    NetStatus readStatus() const;
    QString hostFor(bool adapterOnly) const;
    QString resultPath() const;
    static void log(const QString& msg);

    QPointer<VrLauncher> m_Launcher;
    QPointer<ComputerManager> m_Cm;
    NvComputer* m_Computer = nullptr;
    QString m_Dir;
    QTimer m_Timer;

    Phase m_Phase = Phase::Idle;
    int m_StateIdx = 0;  // 0＝B、1＝C、2＝B
    int m_EyeIdx = 0;
    qint64 m_PhaseStartMs = 0;
    bool m_AdapterOnly = false;
    bool m_Cancel = false;
    int m_AdapterLostTicks = 0;  // 只用轉接器的一階進行中，連續幾次看到 helper 不在 adapter-only
    bool m_Retried = false;  // 這一階已經重跑過一次
    QString m_Loc;
    QString m_Status;
    QString m_Verdict;
    std::vector<NetBench::Ladder> m_Ladders;
    static inline bool s_DevAutoStart = false;
};
