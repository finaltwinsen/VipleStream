/**
 * @file streaming/vr/vrlaunchparams.h
 * @brief VipleStream 2.0 §VR：/launch 的 VR 參數編碼與 <VipleStreamVRSession> 解析。
 *
 * 直連（NvHTTP::startApp）與 relay（Session::tryRelayLaunch）共用這一份，格式見
 * docs/vr_protocol.md §4.2：全部是十進位定點數（vrCaps 是十六進位），不用 base64。
 * server 端的嚴格解析在 Sunshine/src/vr/vr_session.cpp，兩邊要一起改。
 */
#pragma once

#include <QString>

#include <array>
#include <cstdint>

struct VrLaunchConfig {
    int eyeWidth = 1728;
    int eyeHeight = 1728;
    int refreshHz = 90;
    qint64 periodNs = 0;           // 0：由 refreshHz 推算
    std::array<int, 8> fovTan;     // tan×10000：左眼 left,right,up,down，右眼同順序
    int ipd = 6300;                // mm×100
    std::array<int, 14> eyeToHead; // 每眼 pos(mm×100)×3＋quat x,y,z,w(×10000)×4，左眼在前
    uint32_t caps = 0;             // VIPLE_VR_CLIENT_CAP_*
    uint32_t codecs = 0;           // VIPLE_VR_CODEC_*
    QString controller = QStringLiteral("touch");
    int overscan = 0;              // deg×10
    bool force = false;

    // 預設值：對稱 90° FOV、IPD 63 mm、兩眼只有水平位移
    VrLaunchConfig();

    // 以 IPD 重算 eyeToHead 的位置（旋轉維持單位四元數）
    void setIpd(int ipdMmX100);

    // "&vr=1&vrEye=…"，直接接在 /launch 的 query 後面
    QString toQuery() const;
};

// server 在 launch 回應裡的 <VipleStreamVRSession>，內容是 ';' 分隔的 key=value
struct VrSessionInfo {
    bool valid = false;
    int proto = 0;
    int packedWidth = 0;
    int packedHeight = 0;
    int refreshHz = 0;
    QString codec;
    QString layout;
    int overscan = 0;
    bool recoveryIntra = false;
    int irFrames = 0;
    QString transport;
    QString universeId;
    QString sessionGuid;
    QString mode;

    static VrSessionInfo parse(const QString& text);
    QString describe() const;
};
