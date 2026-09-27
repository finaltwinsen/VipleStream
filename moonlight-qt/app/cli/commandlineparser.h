#pragma once

#include "settings/streamingpreferences.h"
#include "cli/decodebench.h"
#include "cli/v4l2probe.h"
#include "cli/xrprobe.h"

#include <QMap>
#include <QString>

class GlobalCommandLineParser
{
public:
    enum ParseResult {
        NormalStartRequested,
        StreamRequested,
        QuitRequested,
        PairRequested,
        ListRequested,
        // VipleStream §FRUC-VALIDATE — 離線 FRUC 品質量測模式（dev-only）。
        FrucOfflineRequested,
        // VipleStream 2.0 §SF-PROBE（M2a）— Steam Frame 探測動作。main.cpp 在
        // QGuiApplication 之前就以 QCoreApplication 派發（見 earlyProbeAction），
        // 一般（QGuiApplication）路徑不會看到這三個值。
        XrProbeRequested,
        V4l2ProbeRequested,
        DecodeBenchRequested,
    };

    GlobalCommandLineParser();
    virtual ~GlobalCommandLineParser();

    ParseResult parse(const QStringList &args);

    // §SF-PROBE（M2a R1）：main() 最前面（還沒有任何 Qt application 物件）用的 argv
    // 掃描。規則與 parse() 相同：第一個不是選項、而且是已知動作名稱的字決定動作。
    // 決定的動作是三個探測動作之一時回傳其正規名稱（"xr-probe"、"v4l2-probe"、
    // "decode-bench"，指向靜態字串），否則回傳 nullptr。只用 C 字串比較，不碰 Qt 物件。
    static const char* earlyProbeAction(int argc, char* argv[]);

};

class QuitCommandLineParser
{
public:
    QuitCommandLineParser();
    virtual ~QuitCommandLineParser();

    void parse(const QStringList &args);

    QString getHost() const;

private:
    QString m_Host;
};

class PairCommandLineParser
{
public:
    PairCommandLineParser();
    virtual ~PairCommandLineParser();

    void parse(const QStringList &args);

    QString getHost() const;
    QString getPredefinedPin() const;

private:
    QString m_Host;
    QString m_PredefinedPin;
};

class StreamCommandLineParser
{
public:
    StreamCommandLineParser();
    virtual ~StreamCommandLineParser();

    void parse(const QStringList &args, StreamingPreferences *preferences);

    QString getHost() const;
    QString getAppName() const;

private:
    QString m_Host;
    QString m_AppName;
    QMap<QString, StreamingPreferences::WindowMode> m_WindowModeMap;
    QMap<QString, StreamingPreferences::AudioConfig> m_AudioConfigMap;
    QMap<QString, StreamingPreferences::VideoCodecConfig> m_VideoCodecMap;
    QMap<QString, StreamingPreferences::VideoDecoderSelection> m_VideoDecoderMap;
    QMap<QString, StreamingPreferences::CaptureSysKeysMode> m_CaptureSysKeysModeMap;
    QMap<QString, StreamingPreferences::FrucBackend> m_FrucBackendMap;
    QMap<QString, StreamingPreferences::FrucQuality> m_FrucQualityMap;
    QMap<QString, StreamingPreferences::RendererSelection> m_RendererSelectionMap;
    // VipleStream §K — MP-QUIC scheduler selector. mpQuicScheduler is plain int
    // (0=auto, 1=min-rtt, 2=aggregate, 3=redundant, 4=ecf — see QuicTransport.h).
    QMap<QString, int> m_QuicSchedulerMap;
};

class ListCommandLineParser
{
public:
    ListCommandLineParser();
    virtual ~ListCommandLineParser();

    void parse(const QStringList &args);

    QString getHost() const;
    bool isPrintCSV() const;
    bool isVerbose() const;

private:
    QString m_Host;
    bool m_PrintCSV;
    bool m_Verbose;
};

// VipleStream §FRUC-VALIDATE — 離線 FRUC 品質量測（dev-only）。
// 用法：viplestream fruc-offline <input_dir> <dump_dir> <width> <height>
// 把 input_dir 的 PNG 序列當輸入幀，餵真實 VkFrucRenderer（SW + FRUC + dump），
// dump 出 real/interp 幀到 dump_dir，供 fruc_metrics.py 對 GT 比對。
class FrucOfflineCommandLineParser
{
public:
    FrucOfflineCommandLineParser();
    virtual ~FrucOfflineCommandLineParser();

    void parse(const QStringList &args);

    QString getInputDir() const;
    QString getDumpDir() const;
    int getWidth() const;
    int getHeight() const;

private:
    QString m_InputDir;
    QString m_DumpDir;
    int m_Width = 0;
    int m_Height = 0;
};

// VipleStream 2.0 §SF-PROBE（M2a）— Steam Frame 探測動作的命令列（dev）。
// 只負責把命令列轉成各 probe 的選項結構（定義在 cli/<probe>.h），實際探測在 run*()。
// 命令列錯誤走 CommandLineParser::showError（結束碼 1 = ProbeUtil::kExitCliError）；
// --help／--version 走 fastExit（_exit），與其他動作相同。
//
// 用法：viplestream xr-probe [--session] [--xr-runtime-json <path>] [--loader-debug] [--json <path>]
class XrProbeCommandLineParser
{
public:
    void parse(const QStringList &args);
    const XrProbeOptions& options() const;

private:
    XrProbeOptions m_Options;
};

// 用法：viplestream v4l2-probe [--device </dev/videoN>] [--header-test h264|hevc|all [--expbuf]] [--json <path>]
class V4l2ProbeCommandLineParser
{
public:
    void parse(const QStringList &args);
    const V4l2ProbeOptions& options() const;

private:
    V4l2ProbeOptions m_Options;
};

// 用法：viplestream decode-bench <file> [--codec …] [--decoder …] [--out …] [--fps N] …（見 --help）
class DecodeBenchCommandLineParser
{
public:
    void parse(const QStringList &args);
    const DecodeBenchOptions& options() const;

private:
    DecodeBenchOptions m_Options;
};
