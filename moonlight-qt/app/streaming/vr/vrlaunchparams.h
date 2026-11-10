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
    int fovea = 100;               // §VR-FOVEA：正前方的像素密度是均勻取樣的幾 %（100＝關）
    // §VR-FOVEA-TILES：「每眼兩塊影像」的大小（1 small、2 medium、3 large＝設定頁的小／中／大；0＝這一場不分塊，
    // dev 的 --vr-fovea-tiles off）；-1＝不帶 vrFoveaTiles（注視點關著），host 照它自己的 vr_fovea_tiles
    int foveaTilesReq = -1;
    // §VR-HALF-RATE：頭盔的顯示更新率（Hz）。半速串流時 refreshHz 是串流的更新率（顯示的一半），host 光看
    // vrHz 分不出「60 Hz 的顯示器」和「120 Hz 顯示器跑半速」。0 或等於 refreshHz＝不帶 vrDispHz
    int displayHz = 0;
    bool force = false;

    // §VR-PLAYAREA（2026-10-30）：真實遊玩區與坐姿零點（未知時 0／false，不帶參數）
    int playAreaWidthMm = 0;       // 0＝未知（不帶 vrPlayArea）
    int playAreaDepthMm = 0;
    bool hasSeated = false;        // true＝已知坐姿零點（帶 vrSeated）
    std::array<int, 3> seatedPosMm = {}; // x, y, z (mm)
    int seatedYawMdeg = 0;         // 繞 +Y 的 yaw（毫度，deg×1000）

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
    int fovea = 100;          // §VR-FOVEA：server 回了 fovea=N 才是 N（否則 100：影像沒有壓縮，不必還原）
    int foveaInsetU0 = 0;     // §VR-FOVEA-INSET：server 回了 foveaInset=N（u0×10000）＝分段曲線；0＝三次曲線
    // §VR-FOVEA-TILES：server 回了 foveaTiles=WxH 與 foveaRender=WxH＝每眼兩塊影像（packed＝2W×2H：上排左右眼的全視野、
    // 下排左右眼的注視點裁切，裁切＝每眼 foveaRender 尺寸的影像裡 W×H 的一塊）。tileW＝0＝沒有分塊
    int tileW = 0;
    int tileH = 0;
    int tileRenderW = 0;
    int tileRenderH = 0;
    bool tilesInvalid = false;  // server 回了分塊但尺寸對不上（valid 會是 false：照畫一定是錯的）
    bool recoveryIntra = false;
    bool multilink = false;   // §VR-MULTILINK：server 回了 multilink=1（雙方都支援）
    bool gazeFovea = false;   // §VR-GAZE-FOVEA：server 回了 gazeFovea=1
    int irFrames = 0;
    QString transport;
    QString universeId;
    QString sessionGuid;
    QString mode;

    static VrSessionInfo parse(const QString& text);
    QString describe() const;
};
