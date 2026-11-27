#include "nvcomputer.h"
#include <QHostAddress>
#include "nvapp.h"
#include "settings/compatfetcher.h"
#include "settings/streamingpreferences.h"

#include <QUdpSocket>
#include <QHostInfo>
#include <QNetworkInterface>
#include <QNetworkProxy>
#include <QHash>
#include <QMutex>
#include <QSet>

namespace {
// serverinfo 建構子每次 poll 都會重跑，同內容的 log 會刷版面。
// 用「key → 上次印過的簽章」快取做去重；polling 執行緒每 host 一條，
// map 共用所以要鎖。回傳 true 表示這次該印（首次或內容有變）。
bool logOnceOnChange(const QString& key, const QString& signature)
{
    static QMutex s_Mutex;
    static QHash<QString, QString> s_LastLoggedSignature;

    QMutexLocker locker(&s_Mutex);
    // §REVIEW-1118：這張表以 uuid 當鍵、行程結束前都不清。區網一直冒出不同的 uuid 就會一直長大；
    // 單筆簽章也可能是超長的介面字串。放不下就整表丟掉（最多多印一輪），單筆先截短。
    const QString cacheKey = key.size() > 256 ? key.left(256) : key;
    const QString stored = signature.size() > 8192 ? signature.left(8192) : signature;
    if (s_LastLoggedSignature.size() >= 512 && !s_LastLoggedSignature.contains(cacheKey)) {
        s_LastLoggedSignature.clear();
    }
    if (s_LastLoggedSignature.value(cacheKey) == stored) {
        return false;
    }
    s_LastLoggedSignature.insert(cacheKey, stored);
    return true;
}

// §REVIEW-1119：排序進行中把模式固定住。比較函式若每次重讀，使用者中途改排序會讓同一次 stable_sort 的順序不一致。
thread_local const StreamingPreferences::AppSortMode* t_PinnedAppSortMode = nullptr;

struct PinnedAppSortMode
{
    explicit PinnedAppSortMode(StreamingPreferences::AppSortMode m)
        : mode(m)
    {
        t_PinnedAppSortMode = &mode;
    }
    ~PinnedAppSortMode()
    {
        t_PinnedAppSortMode = nullptr;
    }
    PinnedAppSortMode(const PinnedAppSortMode&) = delete;
    PinnedAppSortMode& operator=(const PinnedAppSortMode&) = delete;
    StreamingPreferences::AppSortMode mode;
};

// §REVIEW-1119：serverinfo 的位址沒有憑證。主機名稱會讓輪詢發 HTTP、喚醒做 DNS；
// 迴環、未指定、多播、鏈路本地（含 169.254.169.254）也不收。IPv4-mapped 先還原再判。
bool isUsableServerLiteral(const QString& text)
{
    QHostAddress ha;
    if (text.size() > 64 || !ha.setAddress(text)) {
        return false;
    }
    // §REVIEW-1119：toIPv4Address() 在「不是 IPv4」和 0.0.0.0 都回 0。不看 ok 的話，
    // ::ffff:0.0.0.0 不會還原，後面的單一位址比對就擋不到。帶 zone 的字面值換一台機器不能用。
    if (!ha.scopeId().isEmpty()) {
        return false;
    }
    if (ha.protocol() == QAbstractSocket::IPv6Protocol) {
        bool isV4 = false;
        const quint32 v4 = ha.toIPv4Address(&isV4);
        if (isV4) {
            ha = QHostAddress(v4);
        }
    }
    if (ha.isLoopback() || ha.isLinkLocal() || ha.isMulticast() ||
            ha == QHostAddress::Any || ha == QHostAddress::AnyIPv4 ||
            ha == QHostAddress::AnyIPv6 || ha == QHostAddress::Broadcast ||
            (ha.protocol() == QAbstractSocket::IPv4Protocol &&
             ha.isInSubnet(QHostAddress(QStringLiteral("0.0.0.0")), 8))) {
        return false;
    }
    return ha.protocol() == QAbstractSocket::IPv4Protocol ||
           ha.protocol() == QAbstractSocket::IPv6Protocol;
}
}

#define SER_NAME "hostname"
#define SER_UUID "uuid"
#define SER_MAC "mac"
#define SER_LOCALADDR "localaddress"
#define SER_LOCALPORT "localport"
#define SER_REMOTEADDR "remoteaddress"
#define SER_REMOTEPORT "remoteport"
#define SER_MANUALADDR "manualaddress"
#define SER_MANUALPORT "manualport"
#define SER_IPV6ADDR "ipv6address"
#define SER_IPV6PORT "ipv6port"
#define SER_VPNADDR "vpnaddress"
#define SER_VPNPORT "vpnport"
#define SER_LANADDRS "lanaddresses"
#define SER_LANPORT "lanport"
#define SER_STUNADDR "stunaddress"
#define SER_STUNPORT "stunport"
#define SER_STUNNATTYPE "stunnattype"
#define SER_APPLIST "apps"
#define SER_SRVCERT "srvcert"
#define SER_CUSTOMNAME "customname"
#define SER_NVIDIASOFTWARE "nvidiasw"
// VipleStream — per-host toggle: 點該 host tile 直接串流 Desktop，跳過 applist
#define SER_DIRECT_LAUNCH_DESKTOP "directlaunchdesktop"

NvComputer::NvComputer(QSettings& settings)
{
    this->name = settings.value(SER_NAME).toString();
    this->uuid = settings.value(SER_UUID).toString();
    this->hasCustomName = settings.value(SER_CUSTOMNAME).toBool();
    this->macAddress = settings.value(SER_MAC).toByteArray();
    this->localAddress = NvAddress(settings.value(SER_LOCALADDR).toString(),
                                   settings.value(SER_LOCALPORT, QVariant(DEFAULT_HTTP_PORT)).toUInt());
    this->remoteAddress = NvAddress(settings.value(SER_REMOTEADDR).toString(),
                                    settings.value(SER_REMOTEPORT, QVariant(DEFAULT_HTTP_PORT)).toUInt());
    this->ipv6Address = NvAddress(settings.value(SER_IPV6ADDR).toString(),
                                  settings.value(SER_IPV6PORT, QVariant(DEFAULT_HTTP_PORT)).toUInt());
    this->stunAddress = NvAddress(settings.value(SER_STUNADDR).toString(),
                                  settings.value(SER_STUNPORT, QVariant(0)).toUInt());
    this->stunNatType = settings.value(SER_STUNNATTYPE).toString();
    this->manualAddress = NvAddress(settings.value(SER_MANUALADDR).toString(),
                                    settings.value(SER_MANUALPORT, QVariant(DEFAULT_HTTP_PORT)).toUInt());
    this->vpnAddress = NvAddress(settings.value(SER_VPNADDR).toString(),
                                 settings.value(SER_VPNPORT, QVariant(DEFAULT_HTTP_PORT)).toUInt());
    {
        const uint16_t lanPort = settings.value(SER_LANPORT, QVariant(DEFAULT_HTTP_PORT)).toUInt();
        for (const QString& a : settings.value(SER_LANADDRS).toStringList()) {
            if (!a.isEmpty()) {
                this->lanAddresses.append(NvAddress(a, lanPort));
            }
        }
    }
    this->serverCert = QSslCertificate(settings.value(SER_SRVCERT).toByteArray());
    this->isNvidiaServerSoftware = settings.value(SER_NVIDIASOFTWARE).toBool();
    this->directLaunchDesktop = settings.value(SER_DIRECT_LAUNCH_DESKTOP, false).toBool();

    int appCount = settings.beginReadArray(SER_APPLIST);
    this->appList.reserve(appCount);
    for (int i = 0; i < appCount; i++) {
        settings.setArrayIndex(i);

        NvApp app(settings);
        this->appList.append(app);
    }
    settings.endArray();
    sortAppList();

    this->currentGameId = 0;
    this->pairState = PS_UNKNOWN;
    this->state = CS_UNKNOWN;
    this->gfeVersion = nullptr;
    this->appVersion = nullptr;
    this->maxLumaPixelsHEVC = 0;
    this->serverCodecModeSupport = 0;
    this->pendingQuit = false;
    this->gpuModel = nullptr;
    this->isSupportedServerVersion = true;
    this->externalPort = this->remoteAddress.port();
    this->activeHttpsPort = 0;
}

void NvComputer::setRemoteAddress(QHostAddress address)
{
    QWriteLocker lock(&this->lock);

    Q_ASSERT(this->externalPort != 0);

    this->remoteAddress = NvAddress(address, this->externalPort);
}

void NvComputer::serialize(QSettings& settings, bool serializeApps) const
{
    QReadLocker lock(&this->lock);

    settings.setValue(SER_NAME, name);
    settings.setValue(SER_CUSTOMNAME, hasCustomName);
    settings.setValue(SER_UUID, uuid);
    settings.setValue(SER_MAC, macAddress);
    settings.setValue(SER_LOCALADDR, localAddress.address());
    settings.setValue(SER_LOCALPORT, localAddress.port());
    settings.setValue(SER_REMOTEADDR, remoteAddress.address());
    settings.setValue(SER_REMOTEPORT, remoteAddress.port());
    settings.setValue(SER_IPV6ADDR, ipv6Address.address());
    settings.setValue(SER_IPV6PORT, ipv6Address.port());
    settings.setValue(SER_MANUALADDR, manualAddress.address());
    settings.setValue(SER_MANUALPORT, manualAddress.port());
    settings.setValue(SER_VPNADDR, vpnAddress.address());
    settings.setValue(SER_VPNPORT, vpnAddress.port());
    {
        QStringList lans;
        for (const NvAddress& a : lanAddresses) {
            lans.append(a.address());
        }
        settings.setValue(SER_LANADDRS, lans);
        settings.setValue(SER_LANPORT, lanAddresses.isEmpty() ? DEFAULT_HTTP_PORT : lanAddresses.first().port());
    }
    settings.setValue(SER_STUNADDR, stunAddress.address());
    settings.setValue(SER_STUNPORT, stunAddress.port());
    settings.setValue(SER_STUNNATTYPE, stunNatType);
    settings.setValue(SER_SRVCERT, serverCert.toPem());
    settings.setValue(SER_NVIDIASOFTWARE, isNvidiaServerSoftware);
    settings.setValue(SER_DIRECT_LAUNCH_DESKTOP, directLaunchDesktop);

    // Avoid deleting an existing applist if we couldn't get one
    if (!appList.isEmpty() && serializeApps) {
        settings.remove(SER_APPLIST);
        settings.beginWriteArray(SER_APPLIST);
        for (int i = 0; i < appList.count(); i++) {
            settings.setArrayIndex(i);
            appList.at(i).serialize(settings);
        }
        settings.endArray();
    }
}

bool NvComputer::isEqualSerialized(const NvComputer &that) const
{
    // §REVIEW-1104：stunAddress／stunNatType 有進 serialize，漏比的話只改這兩欄不會存檔
    return this->name == that.name &&
           this->hasCustomName == that.hasCustomName &&
           this->uuid == that.uuid &&
           this->macAddress == that.macAddress &&
           this->localAddress == that.localAddress &&
           this->remoteAddress == that.remoteAddress &&
           this->ipv6Address == that.ipv6Address &&
           this->manualAddress == that.manualAddress &&
           this->vpnAddress == that.vpnAddress &&
           this->lanAddresses == that.lanAddresses &&
           this->stunAddress == that.stunAddress &&
           this->stunNatType == that.stunNatType &&
           this->serverCert == that.serverCert &&
           this->isNvidiaServerSoftware == that.isNvidiaServerSoftware &&
           this->directLaunchDesktop == that.directLaunchDesktop &&
           this->appList == that.appList;
}

// VipleStream H Phase 2.2: shared app-comparator reads the user's
// Apps-view sort-mode preference (QSettings-backed) every call. Kept
// here rather than in a separate header so both NvComputer::sortAppList
// and AppModel::updateAppList (its inline insertion-sort in the
// "process additions" pass) can share it — any comparator drift between
// the two causes the AppModel to silently re-sort the NvComputer-sorted
// list back into its own order, which is what burned Phase 2 originally.
//
// Contract:
//   returns true iff `a` should appear BEFORE `b` in the Apps view.
//
// Order is stable: manual entries (source=="") always pin to the top,
// then auto-imported entries (source=="steam" etc.) sort according to
// the current mode. Alphabetical-by-name is the final tiebreaker in
// every mode so the UI never has two apps "swap" unpredictably between
// refreshes.
bool NvComputer::appLessThan(const NvApp& a, const NvApp& b)
{
    // manual (source=="") always pins to the top, regardless of mode.
    if (a.source.isEmpty() != b.source.isEmpty()) {
        return a.source.isEmpty();
    }

    // source bucketing for non-empty: "steam" before "xbox" alphabetical,
    // so the order within auto-imported sources is stable when multiple
    // providers show up (future-proof for G.x TODOs).
    if (a.source != b.source) {
        return a.source < b.source;
    }

    // §REVIEW-1119：sortAppList 會把模式釘住。沒釘住時（AppModel 的插入排序）仍每次讀，維持原行為。
    StreamingPreferences::AppSortMode mode = StreamingPreferences::ASM_RECENT;
    if (t_PinnedAppSortMode != nullptr) {
        mode = *t_PinnedAppSortMode;
    }
    else {
        StreamingPreferences* prefs = StreamingPreferences::get(nullptr);
        if (prefs != nullptr) {
            mode = prefs->appSortMode;
        }
    }

    auto byName = [](const NvApp& x, const NvApp& y) {
        return x.name.toLower() < y.name.toLower();
    };

    switch (mode) {
        case StreamingPreferences::ASM_RECENT:
            // Never-played games (lastPlayed==0) fall to bottom of their
            // bucket alphabetical. Otherwise higher lastPlayed first.
            if ((a.lastPlayed == 0) != (b.lastPlayed == 0)) {
                return a.lastPlayed != 0;  // played before unplayed
            }
            if (a.lastPlayed != b.lastPlayed) {
                return a.lastPlayed > b.lastPlayed;  // desc
            }
            return byName(a, b);

        case StreamingPreferences::ASM_PLAYTIME:
            if ((a.playtimeMinutes == 0) != (b.playtimeMinutes == 0)) {
                return a.playtimeMinutes != 0;
            }
            if (a.playtimeMinutes != b.playtimeMinutes) {
                return a.playtimeMinutes > b.playtimeMinutes;
            }
            return byName(a, b);

        case StreamingPreferences::ASM_NAME:
            return byName(a, b);

        case StreamingPreferences::ASM_DEFAULT:
        default:
            return byName(a, b);
    }
}

void NvComputer::sortAppList()
{
    // VipleStream H Phase 2 + 2.2: pinning + configurable sort mode.
    // See appLessThan() for the ordering rules.
    // §REVIEW-1119：同一次排序只看開始時的模式。中途被改掉會破壞嚴格弱序。
    StreamingPreferences* prefs = StreamingPreferences::get(nullptr);
    const PinnedAppSortMode pin(prefs ? prefs->appSortMode : StreamingPreferences::ASM_RECENT);
    std::stable_sort(appList.begin(), appList.end(), appLessThan);
}

NvComputer::NvComputer(NvHTTP& http, QString serverInfo)
{
    // §REVIEW-1118：/serverinfo 整包讀進記憶體後沒有長度上限（輪詢最多等 5 秒，能灌多少算多少）。
    // 太大就不要逐個標籤再掃一次，也不要把超長的名稱、uuid 存進設定。
    if (serverInfo.size() > (1 << 20)) {
        serverInfo.clear();
    }

    this->serverCert = http.serverCert();

    this->hasCustomName = false;
    this->name = NvHTTP::getXmlString(serverInfo, "hostname");
    if (this->name.size() > 256) {
        this->name.truncate(256);
    }
    if (this->name.isEmpty()) {
        this->name = "UNKNOWN";
    }

    this->uuid = NvHTTP::getXmlString(serverInfo, "uniqueid");
    // §REVIEW-1118：正式 host 的 uniqueid 是 36 字元的 UUID。更長的不要留，避免被當成另一台主機的鍵。
    if (this->uuid.size() > 128) {
        this->uuid.clear();
    }
    QString newMacString = NvHTTP::getXmlString(serverInfo, "mac");
    if (newMacString != "00:00:00:00:00:00") {
        QStringList macOctets = newMacString.split(':');
        for (const QString& macOctet : std::as_const(macOctets)) {
            this->macAddress.append((char) macOctet.toInt(nullptr, 16));
        }
        // §REVIEW-1111：不是 6 個位元組的不要留（會存進設定，之後按「喚醒」組出壞掉的封包）
        if (this->macAddress.size() != 6) {
            this->macAddress.clear();
        }
    }

    QString codecSupport = NvHTTP::getXmlString(serverInfo, "ServerCodecModeSupport");
    if (!codecSupport.isEmpty()) {
        this->serverCodecModeSupport = codecSupport.toInt();
    }
    else {
        // Assume H.264 is always supported
        this->serverCodecModeSupport = SCM_H264;
    }

    QString maxLumaPixelsHEVC = NvHTTP::getXmlString(serverInfo, "MaxLumaPixelsHEVC");
    if (!maxLumaPixelsHEVC.isEmpty()) {
        this->maxLumaPixelsHEVC = maxLumaPixelsHEVC.toInt();
    }
    else {
        this->maxLumaPixelsHEVC = 0;
    }

    this->displayModes = NvHTTP::getDisplayModeList(serverInfo);
    // §REVIEW-1118：寬高、更新率來自未驗證的 XML。負值讓下面的 uint64 比較不是嚴格弱序（排序是未定義行為），
    // 也會在「自動解析度」把負負得正當成最大模式。不合理的先拿掉，剩下的再排。
    {
        QVector<NvDisplayMode> kept;
        kept.reserve(this->displayModes.size());
        for (const NvDisplayMode& m : std::as_const(this->displayModes)) {
            if (m.width <= 0 || m.height <= 0 || m.refreshRate <= 0 ||
                    m.width > 16384 || m.height > 16384) {
                continue;
            }
            kept.append(m);
        }
        this->displayModes.swap(kept);
    }
    std::stable_sort(this->displayModes.begin(), this->displayModes.end(),
                     [](const NvDisplayMode& mode1, const NvDisplayMode& mode2) {
        return (uint64_t)mode1.width * mode1.height * mode1.refreshRate <
                (uint64_t)mode2.width * mode2.height * mode2.refreshRate;
    });

    // §MP-ADV: 解析伺服器廣告的網路介面清單
    this->serverAdvertisedInterfaces = NvHTTP::getNetworkInterfaceList(serverInfo);
    {
        // §REVIEW-1119：位址沒驗過就會進多路徑候選與日誌。不是單播字面值的整筆丟掉。
        // 名稱與類型截短並拿掉控制字元，避免每次輪詢把換行灌進日誌。
        auto clipField = [](QString s, int maxLen) {
            QString out;
            out.reserve(qMin(s.size(), maxLen));
            for (const QChar ch : s) {
                if (out.size() >= maxLen) {
                    break;
                }
                const ushort u = ch.unicode();
                if (u >= 0x20 && u != 0x7F) {
                    out.append(ch);
                }
            }
            return out;
        };
        QVector<NvNetworkInterface> kept;
        kept.reserve(this->serverAdvertisedInterfaces.size());
        for (NvNetworkInterface iface : std::as_const(this->serverAdvertisedInterfaces)) {
            if (!isUsableServerLiteral(iface.address)) {
                continue;
            }
            iface.name = clipField(iface.name, 64);
            iface.type = clipField(iface.type, 32);
            kept.append(iface);
        }
        this->serverAdvertisedInterfaces.swap(kept);
    }
    if (!this->serverAdvertisedInterfaces.isEmpty()) {
        // 內容變化才印（見 logOnceOnChange）
        QString ifaceSig;
        for (const auto& iface : std::as_const(this->serverAdvertisedInterfaces)) {
            ifaceSig += iface.name + QLatin1Char('|') + iface.type + QLatin1Char('|') + iface.address + QLatin1Char(';');
        }
        if (logOnceOnChange(QStringLiteral("srv-ifaces:") + this->uuid, ifaceSig)) {
            qInfo() << "Server advertised" << this->serverAdvertisedInterfaces.size() << "network interface(s):";
            for (const auto& iface : std::as_const(this->serverAdvertisedInterfaces)) {
                qInfo() << "  " << iface.name << "(" << iface.type << "):" << iface.address;
            }
        }
    }

    // §VPN-ADDR：host 廣告的 VPN 位址，只收 Tailscale 等 CGNAT 範圍（100.64.0.0/10；Cloudflare WARP 之類的本機位址不收）
    {
        const QStringList vpnAddrs = NvHTTP::getXmlStringList(serverInfo, "VipleStreamVpnAddress");
        for (const QString& a : vpnAddrs) {
            const QHostAddress ha(a);
            if (ha.protocol() == QAbstractSocket::IPv4Protocol && ha.isInSubnet(QHostAddress(QStringLiteral("100.64.0.0")), 10)) {
                this->vpnAddress = NvAddress(a, http.httpPort());
                break;
            }
        }
    }

    // §LAN-PREF：host 廣告的區網位址，只收私有 IPv4（10/8、172.16/12、192.168/16），順序照 host 給的（有線先）
    {
        const QStringList lanAddrs = NvHTTP::getXmlStringList(serverInfo, "VipleStreamLanAddress");
        for (const QString& a : lanAddrs) {
            const QHostAddress ha(a);
            if (ha.protocol() == QAbstractSocket::IPv4Protocol &&
                    (ha.isInSubnet(QHostAddress(QStringLiteral("10.0.0.0")), 8) ||
                     ha.isInSubnet(QHostAddress(QStringLiteral("172.16.0.0")), 12) ||
                     ha.isInSubnet(QHostAddress(QStringLiteral("192.168.0.0")), 16))) {
                this->lanAddresses.append(NvAddress(a, http.httpPort()));
                if (this->lanAddresses.size() >= 16) {
                    break;  // §REVIEW-1111（第二輪）：清單來自網路，設上限
                }
            }
        }
    }

    // We can get an IPv4 loopback address if we're using the GS IPv6 Forwarder.
    // §REVIEW-1119：只收單播 IP 字面值（見 isUsableServerLiteral）。127. 前綴擋不到 ::1、主機名稱與鏈路本地。
    const QString localIp = NvHTTP::getXmlString(serverInfo, "LocalIP");
    this->localAddress = isUsableServerLiteral(localIp) ? NvAddress(localIp, http.httpPort()) : NvAddress();

    QString httpsPort = NvHTTP::getXmlString(serverInfo, "HttpsPort");
    if (httpsPort.isEmpty() || (this->activeHttpsPort = httpsPort.toUShort()) == 0) {
        this->activeHttpsPort = DEFAULT_HTTPS_PORT;
    }

    // This is an extension which is not present in GFE. It is present for Sunshine to be able
    // to support dynamic HTTP WAN ports without requiring the user to manually enter the port.
    QString remotePortStr = NvHTTP::getXmlString(serverInfo, "ExternalPort");
    if (remotePortStr.isEmpty() || (this->externalPort = remotePortStr.toUShort()) == 0) {
        this->externalPort = http.httpPort();
    }

    const QString remoteIp = NvHTTP::getXmlString(serverInfo, "ExternalIP");
    this->remoteAddress = isUsableServerLiteral(remoteIp) ? NvAddress(remoteIp, this->externalPort) : NvAddress();

    // VipleStream: Parse STUN endpoint from serverInfo
    // Format: "IP:port" or "[IPv6]:port" (port is UDP control port, but we need HTTP port for API)
    QString stunEndpoint = NvHTTP::getXmlString(serverInfo, "StunEndpoint");
    if (!stunEndpoint.isEmpty() && stunEndpoint.size() <= 80) {
        int colonIdx = stunEndpoint.lastIndexOf(':');
        if (colonIdx > 0) {
            QString stunIp = stunEndpoint.left(colonIdx);
            if (stunIp.startsWith(QLatin1Char('[')) && stunIp.endsWith(QLatin1Char(']'))) {
                stunIp = stunIp.mid(1, stunIp.size() - 2);
            }
            // §REVIEW-1119：切出來的位址不是單播字面值就整筆不要（含打洞埠）。超長或帶換行的原文也不要印。
            if (isUsableServerLiteral(stunIp)) {
                this->stunAddress = NvAddress(stunIp, http.httpPort());
                // §REVIEW-1104（使用者決策 B）：冒號後面就是 host 控制埠在公網上的實際埠，打洞要打這個
                bool portOk = false;
                const uint punchPort = stunEndpoint.mid(colonIdx + 1).toUInt(&portOk);
                if (portOk && punchPort > 0 && punchPort < 65536) {
                    this->stunPunchPort = static_cast<uint16_t>(punchPort);
                }
                this->stunNatType = NvHTTP::getXmlString(serverInfo, "StunNatType");
                if (this->stunNatType.size() > 32) {
                    this->stunNatType.truncate(32);
                }
                if (logOnceOnChange(QStringLiteral("stun:") + this->uuid,
                                    this->stunAddress.address() + QLatin1Char('|') + this->stunNatType)) {
                    qInfo() << "STUN endpoint:" << this->stunAddress.toString()
                            << "NAT:" << this->stunNatType;
                }
            }
        }
    }

    // VipleStream capability marker. Vanilla Sunshine / GFE don't emit this
    // element; only VipleStream-Server does. Empty string ⇒ not a Viple peer.
    this->vipleStreamProtocol = NvHTTP::getXmlString(serverInfo, "VipleStreamProtocol");
    this->isVipleStreamPeer = !this->vipleStreamProtocol.isEmpty();

    // VipleStream MP-QUIC capability
    this->vipleStreamMPQUIC = NvHTTP::getXmlString(serverInfo, "VipleStreamMPQUIC");
    this->isMpQuicCapable = !this->vipleStreamMPQUIC.isEmpty();

    // VipleStream server build version
    this->vipleStreamVersion = NvHTTP::getXmlString(serverInfo, "VipleStreamVersion");

    // VipleStream 2.0 §VR 能力（沒有元素時 toInt() 回 0）
    this->vipleStreamVr = NvHTTP::getXmlString(serverInfo, "VipleStreamVR").toInt();
    this->vipleStreamInput = NvHTTP::getXmlString(serverInfo, "VipleStreamInput").toInt();  // §LAT-2D
    this->vipleStreamVrProto = NvHTTP::getXmlString(serverInfo, "VipleStreamVRProto").toInt();
    this->displayModeSwitchable = NvHTTP::getXmlString(serverInfo, "DisplayModeSwitchable") == "1";

    // Real Nvidia host software (GeForce Experience and RTX Experience) both use the 'Mjolnir'
    // codename in the state field and no version of Sunshine does. We can use this to bypass
    // some assumptions about Nvidia hardware that don't apply to Sunshine hosts.
    this->isNvidiaServerSoftware = NvHTTP::getXmlString(serverInfo, "state").contains("MJOLNIR");

    this->pairState = NvHTTP::getXmlString(serverInfo, "PairStatus") == "1" ?
                PS_PAIRED : PS_NOT_PAIRED;
    this->currentGameId = NvHTTP::getCurrentGame(serverInfo);

    // §M.2: 從 serverinfo 解析 session owner 資訊（VipleStream 擴充欄位）
    this->sessionOwnerUuid = NvHTTP::getXmlString(serverInfo, "SessionOwnerUuid");
    this->sessionOwnerName = NvHTTP::getXmlString(serverInfo, "SessionOwnerName");
    // §M.2-SELF（2026-11-14）：SessionOwnerUuid 是 host 那邊配對紀錄的編號，和我們的 uniqueid 不是同一種東西，比不出
    // 「是不是自己」。host 依 TLS 憑證判斷之後放在這一欄；2.0.191 以前的 host 沒有這一欄
    this->sessionOwnedByCaller = NvHTTP::getXmlString(serverInfo, "SessionOwnedByCaller") == "1";

    this->appVersion = NvHTTP::getXmlString(serverInfo, "appversion");
    this->gfeVersion = NvHTTP::getXmlString(serverInfo, "GfeVersion");
    this->gpuModel = NvHTTP::getXmlString(serverInfo, "gputype");
    this->activeAddress = http.address();
    this->state = NvComputer::CS_ONLINE;
    this->pendingQuit = false;
    this->isSupportedServerVersion = CompatFetcher::isGfeVersionSupported(this->gfeVersion);
}

bool NvComputer::wake() const
{
    QByteArray wolPayload;
    // §REVIEW-1119：下面送封包時已經放開鎖。name 會被改名與 update() 換掉，解鎖後再讀 QString 是資料競爭。
    QString hostName;
    QString manualHost;

    {
        QReadLocker readLocker(&lock);
        hostName = name;
        manualHost = manualAddress.address();

        if (state == NvComputer::CS_ONLINE) {
            qWarning() << hostName << "is already online";
            return true;
        }

        if (macAddress.size() != 6) {
            qWarning() << hostName << "has no MAC address stored";
            return false;
        }

        // Create the WoL payload
        wolPayload.append(QByteArray::fromHex("FFFFFFFFFFFF"));
        for (int i = 0; i < 16; i++) {
            wolPayload.append(macAddress);
        }
        Q_ASSERT(wolPayload.size() == 102);
    }

    // Ports used as-is
    const quint16 STATIC_WOL_PORTS[] = {
        9, // Standard WOL port (privileged port)
        47009, // Port opened by Moonlight Internet Hosting Tool for WoL (non-privileged port)
    };

    // Ports offset by the HTTP base port for hosts using alternate ports
    const quint16 DYNAMIC_WOL_PORTS[] = {
        47998, 47999, 48000, 48002, 48010, // Ports opened by GFE
    };

    // Add the addresses that we know this host to be
    // and broadcast addresses for this link just in
    // case the host has timed out in ARP entries.
    QMap<QString, quint16> addressMap;
    QSet<quint16> basePortSet;
    const auto uniqueHostAddresses = uniqueAddresses();
    for (const NvAddress& addr : uniqueHostAddresses) {
        addressMap.insert(addr.address(), addr.port());
        // §REVIEW-1119：埠 0（存檔預設）或低埠配上動態偏移，會打到 DNS／NTP 那些埠。
        // 標準 WoL 的 9 走下面的固定清單，不從這裡進。
        if (addr.port() >= 1024) {
            basePortSet.insert(addr.port());
        }
    }
    addressMap.insert("255.255.255.255", 0);

    // Try to broadcast on all available NICs
    uint32_t wolInterfaces = 0;
    const auto allInterfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface& nic : allInterfaces) {
        // Ensure the interface is up and skip the loopback adapter
        if ((nic.flags() & QNetworkInterface::IsUp) == 0 ||
                (nic.flags() & QNetworkInterface::IsLoopBack) != 0) {
            continue;
        }
        ++wolInterfaces;

        QHostAddress allNodesMulticast("FF02::1");
        const auto allInterfaceAddresses = nic.addressEntries();
        for (const QNetworkAddressEntry& addr : allInterfaceAddresses) {
            // Store the scope ID for this NIC if IPv6 is enabled
            if (!addr.ip().scopeId().isEmpty()) {
                allNodesMulticast.setScopeId(addr.ip().scopeId());
            }

            // Skip IPv6 which doesn't support broadcast
            if (!addr.broadcast().isNull()) {
                addressMap.insert(addr.broadcast().toString(), 0);
            }
        }

        if (!allNodesMulticast.scopeId().isEmpty()) {
            addressMap.insert(allNodesMulticast.toString(), 0);
        }
    }

    // Try all unique address strings or host names
    bool success = false;
    // §LOGHYG-WOL（2026-11-27）：每個介面 × 每個埠各印一行的「Sent WoL packet」在單檔占了 98 行
    // （使用者回報「開場 WoL 洗版」）。成功的不再逐筆印，改成結尾一行摘要；失敗很少見，照舊逐筆印。
    uint32_t sentPackets = 0;
    uint32_t failedPackets = 0;
    QSet<QString> sentAddresses;
    QSet<quint16> sentPorts;
    for (auto i = addressMap.constBegin(); i != addressMap.constEnd(); i++) {
        QHostAddress literalAddress;
        QList<QHostAddress> addressList;

        // If this is an IPv4/IPv6 literal, don't use QHostInfo::fromName() because that will
        // try to perform a reverse DNS lookup that leads to delays sending WoL packets.
        if (literalAddress.setAddress(i.key())) {
            addressList.append(literalAddress);
        }
        else if (!manualHost.isEmpty() && i.key() == manualHost) {
            // §REVIEW-1119：serverinfo 來的名稱不做 DNS。只有使用者自己填的 manualAddress 才解析。
            QHostInfo hostInfo = QHostInfo::fromName(i.key());
            if (hostInfo.error() != QHostInfo::NoError) {
                qWarning() << "Error resolving" << i.key() << ":" << hostInfo.errorString();
                continue;
            }

            addressList.append(hostInfo.addresses());
        }
        else {
            continue;
        }

        // Try all IP addresses that this string resolves to
        for (QHostAddress& address : addressList) {
            QUdpSocket sock;

            // Send to all static ports
            for (quint16 port : STATIC_WOL_PORTS) {
                if (sock.writeDatagram(wolPayload, address, port) == wolPayload.size()) {  // REVIEW-1111 (round 2): -1 (failure) is also "true"
                    ++sentPackets;
                    sentAddresses.insert(address.toString());
                    sentPorts.insert(port);
                    success = true;
                }
                else {
                    ++failedPackets;
                    qWarning().nospace().noquote() << "WoL send failed to " << hostName << " via "
                                                   << address.toString() << ":" << port << ": " << sock.error();
                }
            }

            QList<quint16> basePorts;
            if (i.value() != 0) {
                // If we have a known base port for this address, use only that port
                basePorts.append(i.value());
            }
            else {
                // If this is a broadcast address without a known HTTP port, try all of them
                basePorts.append(basePortSet.values());
            }

            // Send to all dynamic ports using the HTTP port offset(s) for this address
            for (quint16 basePort : basePorts) {
                for (quint16 port : DYNAMIC_WOL_PORTS) {
                    // §REVIEW-1119：ExternalPort 很大時 uint16 相加會繞回，變成對任意 UDP 埠送喚醒包。
                    // 很小的 ExternalPort 不用溢位也會打進 1024 以下的埠，一樣跳過。
                    const int mapped = int(port) - 47989 + int(basePort);
                    if (basePort < 1024 || mapped < 1024 || mapped > 65535) {
                        continue;
                    }

                    if (sock.writeDatagram(wolPayload, address, quint16(mapped)) == wolPayload.size()) {  // REVIEW-1111 (round 2): -1 (failure) is also "true"
                        ++sentPackets;
                        sentAddresses.insert(address.toString());
                        sentPorts.insert(quint16(mapped));
                        success = true;
                    }
                    else {
                        ++failedPackets;
                        qWarning().nospace().noquote() << "WoL send failed to " << hostName << " via "
                                                       << address.toString() << ":" << mapped << ": " << sock.error();
                    }
                }
            }
        }
    }

    // §LOGHYG-WOL：一行摘要取代逐筆成功行。失敗幾筆就印幾行（很少見；失敗時才需要對哪個位址／埠有問題）。
    if (sentPackets > 0) {
        qInfo().nospace().noquote() << "WoL: sent " << sentPackets << " packet(s) to " << hostName
                                    << " via " << wolInterfaces << " interface(s), "
                                    << sentAddresses.size() << " address(es), "
                                    << sentPorts.size() << " port(s)"
                                    << (failedPackets > 0 ? QStringLiteral(" (%1 failed)").arg(failedPackets) : QString());
    }

    return success;
}

NvComputer::ReachabilityType NvComputer::getActiveAddressReachability(int* mtuOut, QHostAddress* peerOut) const
{
    NvAddress copyOfActiveAddress;
    // §REVIEW-1119：探測在放開鎖之後才打 log。uuid 在鎖內先抄出來。
    QString hostUuid;

    if (mtuOut != nullptr) {
        *mtuOut = 0;
    }
    if (peerOut != nullptr) {
        *peerOut = QHostAddress();
    }

    {
        QReadLocker readLocker(&lock);
        hostUuid = uuid;

        if (activeAddress.isNull()) {
            return ReachabilityType::RI_UNKNOWN;
        }

        // Grab a copy of the active address to avoid having to hold
        // the computer lock while doing socket operations
        copyOfActiveAddress = activeAddress;
    }

    QTcpSocket s;
    s.setProxy(QNetworkProxy::NoProxy);
    s.connectToHost(copyOfActiveAddress.address(), copyOfActiveAddress.port());
    // §REVIEW-1111：這個探測在每次開串流、每次結束串流時都會做。對方的 TCP 埠連不上（只靠 relay、路由黑洞）時
    // 原本各等滿 3 秒。區網與一般 VPN 一個往返內就會連上；等不到就當成「不知道」（串流設定用自動）。
    if (s.waitForConnected(800)) {
        Q_ASSERT(!s.localAddress().isNull());
        Q_ASSERT(!s.peerAddress().isNull());

        const auto allInterfaces = QNetworkInterface::allInterfaces();
        for (const QNetworkInterface& nic : allInterfaces) {
            // Ensure the interface is up
            if ((nic.flags() & QNetworkInterface::IsUp) == 0) {
                continue;
            }

            const auto allInterfaceAddresses = nic.addressEntries();
            for (const QNetworkAddressEntry& addr : allInterfaceAddresses) {
                if (addr.ip() == s.localAddress()) {
                    // 每次可達性檢查都會走到這裡；介面資訊同值不重印
                    QString nicSig = nic.humanReadableName() + QLatin1Char('|') + nic.hardwareAddress()
                                   + QLatin1Char('|') + QString::number(int(nic.flags()));
#if QT_VERSION >= QT_VERSION_CHECK(5, 11, 0)
                    nicSig += QLatin1Char('|') + QString::number(int(nic.type()))
                            + QLatin1Char('|') + QString::number(nic.maximumTransmissionUnit());
#endif
                    const bool logNic = logOnceOnChange(QStringLiteral("reach-nic:") + hostUuid, nicSig);
                    if (logNic) {
                        qInfo() << "Found matching interface:" << nic.humanReadableName() << nic.hardwareAddress() << nic.flags();
                    }

                    // §PERF-1113 A22：對端位址與這張介面的 MTU 交給呼叫端（在下面任何一個 return 之前填）
                    if (peerOut != nullptr) {
                        *peerOut = s.peerAddress();
                    }

#if QT_VERSION >= QT_VERSION_CHECK(5, 11, 0)
                    if (mtuOut != nullptr) {
                        *mtuOut = nic.maximumTransmissionUnit();
                    }

                    if (logNic) {
                        qInfo() << "Interface Type:" << nic.type();
                        qInfo() << "Interface MTU:" << nic.maximumTransmissionUnit();
                    }

                    if (nic.type() == QNetworkInterface::Virtual ||
                            nic.type() == QNetworkInterface::Ppp) {
                        // Treat PPP and virtual interfaces as likely VPNs
                        return ReachabilityType::RI_VPN;
                    }

                    if (nic.maximumTransmissionUnit() != 0 && nic.maximumTransmissionUnit() < 1500) {
                        // Treat MTUs under 1500 as likely VPNs
                        return ReachabilityType::RI_VPN;
                    }
#endif

                    if (nic.flags() & QNetworkInterface::IsPointToPoint) {
                        // Treat point-to-point links as likely VPNs.
                        // This check detects OpenVPN on Unix-like OSes.
                        return ReachabilityType::RI_VPN;
                    }

#ifdef Q_OS_WINDOWS
                    if (nic.name().startsWith("iftype53_") || nic.name().startsWith("iftype131_")) {
                        // Match by NDIS interface type. These values are Microsoft's recommended values for VPN connections:
                        // https://learn.microsoft.com/en-US/troubleshoot/windows-client/networking/windows-connection-manager-disconnects-wlan#more-information
                        //
                        // The following VPNs use IF_TYPE_PROP_VIRTUAL under Windows:
                        //  - WireguardNT VPNs
                        //  - All WinTun-based VPNs (such as Slack Nebula)
                        //  - OpenVPN with tap-windows6
                        return ReachabilityType::RI_VPN;
                    }
#endif

                    if (nic.hardwareAddress().startsWith("00:FF", Qt::CaseInsensitive)) {
                        // OpenVPN TAP interfaces have a MAC address starting with 00:FF on Windows
                        return ReachabilityType::RI_VPN;
                    }

                    if (nic.humanReadableName().startsWith("ZeroTier")) {
                        // ZeroTier interfaces always start with "ZeroTier"
                        return ReachabilityType::RI_VPN;
                    }

                    if (nic.humanReadableName().contains("VPN")) {
                        // This one is just a final VPN heuristic if all else fails
                        return ReachabilityType::RI_VPN;
                    }

                    // Didn't meet any of our VPN heuristics. Let's see if the peer address is on-link.
                    Q_ASSERT(addr.prefixLength() >= 0);
                    if (addr.prefixLength() >= 0 && s.localAddress().isInSubnet(s.peerAddress(), addr.prefixLength())) {
                        return ReachabilityType::RI_LAN;
                    }

                    // Default to unknown if nothing else matched
                    return ReachabilityType::RI_UNKNOWN;
                }
            }
        }

        qWarning() << "No match found for address:" << s.localAddress();
        return ReachabilityType::RI_UNKNOWN;
    }
    else {
        // If we fail to connect, just pretend that it's not a VPN
        qWarning() << "Unable to check for reachability within 3 seconds";
        return ReachabilityType::RI_UNKNOWN;
    }
}

bool NvComputer::updateAppList(QVector<NvApp> newAppList) {
    // §REVIEW-1118：已配對的 host 回的 applist 沒有筆數上限，整包會進記憶體與設定檔。
    // 正常收藏庫到不了這個數；再多只留前面，避免設定檔被灌爆。
    if (newAppList.size() > 8192) {
        newAppList.resize(8192);
    }
    // §REVIEW-1119：單筆字串沒有上限，超長名稱會跟著設定檔長大。
    for (NvApp& app : newAppList) {
        if (app.name.size() > 256) {
            app.name.truncate(256);
        }
        if (app.source.size() > 64) {
            app.source.truncate(64);
        }
        if (app.steamAppId.size() > 64) {
            app.steamAppId.truncate(64);
        }
        if (app.steamOwners.size() > 1024) {
            app.steamOwners.truncate(1024);
        }
    }

    // §REVIEW-1119：雙層比對在 8192 筆時會在寫鎖裡做數千萬次。用 id 沿用本機的隱藏與直接啟動。
    QHash<int, quint8> localFlags;
    localFlags.reserve(appList.size());
    for (const NvApp& existingApp : std::as_const(appList)) {
        localFlags.insert(existingApp.id,
                          static_cast<quint8>((existingApp.hidden ? 1 : 0) | (existingApp.directLaunch ? 2 : 0)));
    }
    for (NvApp& newApp : newAppList) {
        const auto it = localFlags.constFind(newApp.id);
        if (it == localFlags.constEnd()) {
            continue;
        }
        newApp.hidden = (it.value() & 1) != 0;
        newApp.directLaunch = (it.value() & 2) != 0;
    }

    {
        StreamingPreferences* prefs = StreamingPreferences::get(nullptr);
        const PinnedAppSortMode pin(prefs ? prefs->appSortMode : StreamingPreferences::ASM_RECENT);
        std::stable_sort(newAppList.begin(), newAppList.end(), appLessThan);
    }
    // §REVIEW-1119：合併、排序之後相同就不要回報變更，避免每一輪輪詢都重寫設定。
    if (appList == newAppList) {
        return false;
    }

    appList = newAppList;
    return true;
}

QVector<NvAddress> NvComputer::uniqueAddresses() const
{
    QReadLocker readLocker(&lock);
    QVector<NvAddress> uniqueAddressList;

    // Start with addresses correctly ordered
    uniqueAddressList.append(activeAddress);
    uniqueAddressList.append(localAddress);
    uniqueAddressList.append(remoteAddress);
    uniqueAddressList.append(stunAddress);  // VipleStream: STUN endpoint as fallback
    uniqueAddressList.append(ipv6Address);
    uniqueAddressList.append(manualAddress);
    uniqueAddressList.append(vpnAddress);  // §VPN-ADDR：其他都連不到時（離開區網）

    // Prune duplicates (always giving precedence to the first)
    for (int i = 0; i < uniqueAddressList.count(); i++) {
        if (uniqueAddressList[i].isNull()) {
            uniqueAddressList.remove(i);
            i--;
            continue;
        }
        for (int j = i + 1; j < uniqueAddressList.count(); j++) {
            if (uniqueAddressList[i] == uniqueAddressList[j]) {
                // Always remove the later occurrence
                uniqueAddressList.remove(j);
                j--;
            }
        }
    }

    // §REVIEW-1119：非單播字面值拒收之後，六個會存檔的位址可以全空（區網位址不在這份清單）。
    // Debug 在這裡斷言會讓輪詢那台主機時整個行程中止。空清單交給呼叫端跳過。
    return uniqueAddressList;
}

bool NvComputer::update(const NvComputer& that)
{
    // Lock us for write and them for read
    QWriteLocker thisLock(&this->lock);
    QReadLocker thatLock(&that.lock);
    // §REVIEW-1104：update 自己會拿寫鎖，relay 要在同一把鎖裡還原時改走 updateUnlocked
    return updateUnlocked(that);
}

bool NvComputer::updateUnlocked(const NvComputer& that)
{
    bool changed = false;

    // UUID may not change or we're talking to a new PC
    Q_ASSERT(this->uuid == that.uuid);

#define ASSIGN_IF_CHANGED(field)       \
    if (this->field != that.field) {   \
        this->field = that.field;      \
        changed = true;                \
    }

#define ASSIGN_IF_CHANGED_AND_NONEMPTY(field) \
    if (!that.field.isEmpty() &&              \
        this->field != that.field) {          \
        this->field = that.field;             \
        changed = true;                       \
    }

#define ASSIGN_IF_CHANGED_AND_NONNULL(field)  \
    if (!that.field.isNull() &&               \
        this->field != that.field) {          \
        this->field = that.field;             \
        changed = true;                       \
    }

    if (!hasCustomName) {
        // Only overwrite the name if it's not custom
        ASSIGN_IF_CHANGED(name);
    }
    ASSIGN_IF_CHANGED_AND_NONEMPTY(macAddress);
    ASSIGN_IF_CHANGED_AND_NONNULL(localAddress);
    ASSIGN_IF_CHANGED_AND_NONNULL(remoteAddress);
    ASSIGN_IF_CHANGED_AND_NONNULL(ipv6Address);
    ASSIGN_IF_CHANGED_AND_NONNULL(manualAddress);
    ASSIGN_IF_CHANGED_AND_NONNULL(vpnAddress);
    ASSIGN_IF_CHANGED_AND_NONEMPTY(lanAddresses);  // 舊 host 不廣告：不清掉已存的
    // §REVIEW-1104：poll 的 STUN 位址與 NAT 類型沒寫回，會一直對舊端點打洞；空值不覆蓋，避免沒送 StunEndpoint 的 poll 把已存值清掉
    ASSIGN_IF_CHANGED_AND_NONNULL(stunAddress);
    ASSIGN_IF_CHANGED_AND_NONEMPTY(stunNatType);
    if (that.stunPunchPort != 0) {  // §REVIEW-1104：只在執行期、不影響存檔與畫面，不算「有變更」
        stunPunchPort = that.stunPunchPort;
    }
    ASSIGN_IF_CHANGED(activeHttpsPort);
    ASSIGN_IF_CHANGED(externalPort);
    ASSIGN_IF_CHANGED(pairState);
    ASSIGN_IF_CHANGED(serverCodecModeSupport);
    ASSIGN_IF_CHANGED(currentGameId);
    ASSIGN_IF_CHANGED(sessionOwnerUuid);
    ASSIGN_IF_CHANGED(sessionOwnerName);
    ASSIGN_IF_CHANGED(sessionOwnedByCaller);
    ASSIGN_IF_CHANGED(activeAddress);
    ASSIGN_IF_CHANGED(state);
    ASSIGN_IF_CHANGED(gfeVersion);
    ASSIGN_IF_CHANGED(appVersion);
    ASSIGN_IF_CHANGED(isSupportedServerVersion);
    ASSIGN_IF_CHANGED(isNvidiaServerSoftware);
    // VipleStream H.5: capability flag must propagate via update() too,
    // otherwise hosts paired before the marker shipped (or app reboots
    // that hydrate NvComputer from QSettings before any /serverinfo
    // poll completes) keep isVipleStreamPeer == false forever and the
    // PcView badge / Steam profile dropdown never appear.
    ASSIGN_IF_CHANGED(isVipleStreamPeer);
    ASSIGN_IF_CHANGED(vipleStreamProtocol);
    // VipleStream §K / §Q 2026-05-23 — Same root cause as the H.5 fix
    // above:  MP-QUIC capability flag was parsed in the serverInfo ctor
    // but never propagated through update(), so any poll after the
    // QSettings-loaded NvComputer (which has the default
    // isMpQuicCapable=false from nvcomputer.h:134) silently discarded
    // the live serverinfo value.  Session::exec() then saw
    // isMpQuicCapable==false at session.cpp:2274 and skipped the entire
    // #ifdef VIPLE_MPQUIC config block even when the host advertised
    // <VipleStreamMPQUIC>1</...>.  Found independently on Windows when
    // the v1.5.30 build's [VIPLE-MPQUIC-DIAG] trace showed the field
    // arriving as "" despite a 831-byte serverinfo with the tag.
    ASSIGN_IF_CHANGED(isMpQuicCapable);
    ASSIGN_IF_CHANGED(vipleStreamMPQUIC);
    ASSIGN_IF_CHANGED(vipleStreamVersion);
    ASSIGN_IF_CHANGED(vipleStreamVr);       // §VR：漏加會讓 poll 回來的值被丟掉（見上方事故註解）
    ASSIGN_IF_CHANGED(vipleStreamVrProto);
    ASSIGN_IF_CHANGED(vipleStreamInput);  // §LAT-2D
    ASSIGN_IF_CHANGED(displayModeSwitchable);  // §H.4-SWITCH
    ASSIGN_IF_CHANGED(maxLumaPixelsHEVC);
    ASSIGN_IF_CHANGED(gpuModel);
    ASSIGN_IF_CHANGED_AND_NONNULL(serverCert);
    ASSIGN_IF_CHANGED_AND_NONEMPTY(displayModes);
    ASSIGN_IF_CHANGED_AND_NONEMPTY(serverAdvertisedInterfaces);

    if (!that.appList.isEmpty()) {
        // updateAppList() handles merging client-side attributes
        // §REVIEW-1111（第二輪）：只有 app 清單變了（其他欄位都沒變）也要回報有變更，否則不會存檔、介面也不更新
        changed |= updateAppList(that.appList);
    }

    return changed;
}
