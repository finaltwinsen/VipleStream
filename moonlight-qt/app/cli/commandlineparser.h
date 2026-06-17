#pragma once

#include "settings/streamingpreferences.h"

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
    };

    GlobalCommandLineParser();
    virtual ~GlobalCommandLineParser();

    ParseResult parse(const QStringList &args);

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
