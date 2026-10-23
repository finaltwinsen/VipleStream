#include "videothumbs.h"

#include "computermanager.h"
#include "nvcomputer.h"
#include "nvhttp.h"
#include "path.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QQmlEngine>
#include <QQuickTextureFactory>
#include <QReadLocker>
#include <QRegularExpression>
#include <QThread>
#include <QThreadPool>
#include <QUrl>
#include <QUrlQuery>

#include <chrono>

#include "SDL_compat.h"

#ifdef HAVE_VR_PLAYER
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}
#endif

namespace {

constexpr int kThumbWidth = 320;
constexpr int kHostTimeoutMs = 25000;  // host 一次只做一張（最多等 20 s，含排隊），這邊等久一點

using CancelFlag = std::shared_ptr<std::atomic<bool>>;

#ifdef HAVE_VR_PLAYER
// FFmpeg 的中斷回呼：取消了、或超過期限就中斷（避免卡在讀不到的檔案上）
struct InterruptCtx {
    CancelFlag cancel;
    std::chrono::steady_clock::time_point deadline;
};
int interruptCb(void* opaque)
{
    auto* ctx = static_cast<InterruptCtx*>(opaque);
    return (ctx->cancel && ctx->cancel->load()) || std::chrono::steady_clock::now() > ctx->deadline ? 1 : 0;
}

// 頭盔本機檔：跳到片長 10% 的位置（片頭常是黑畫面）解一張，縮成寬 kThumbWidth 的 RGB
QImage extractLocalFrame(const QString& path, const CancelFlag& cancel)
{
    InterruptCtx ictx {cancel, std::chrono::steady_clock::now() + std::chrono::seconds(10)};
    AVFormatContext* fmt = avformat_alloc_context();
    if (fmt == nullptr) {
        return {};
    }
    fmt->interrupt_callback.callback = interruptCb;
    fmt->interrupt_callback.opaque = &ictx;
    if (avformat_open_input(&fmt, path.toUtf8().constData(), nullptr, nullptr) < 0) {
        return {};  // 失敗時 FFmpeg 已經釋放 fmt
    }
    QImage out;
    AVCodecContext* cc = nullptr;
    AVFrame* frame = av_frame_alloc();
    AVPacket* pkt = av_packet_alloc();
    do {
        if (frame == nullptr || pkt == nullptr || avformat_find_stream_info(fmt, nullptr) < 0) {
            break;
        }
        const AVCodec* dec = nullptr;
        const int si = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
        if (si < 0 || dec == nullptr) {
            break;
        }
        cc = avcodec_alloc_context3(dec);
        if (cc == nullptr || avcodec_parameters_to_context(cc, fmt->streams[si]->codecpar) < 0) {
            break;
        }
        cc->thread_count = 2;
        if (avcodec_open2(cc, dec, nullptr) < 0) {
            break;
        }
        if (fmt->duration > 0) {
            av_seek_frame(fmt, -1, fmt->duration / 10, AVSEEK_FLAG_BACKWARD);
        }
        bool got = false;
        for (int packets = 0; !got && packets < 600 && !interruptCb(&ictx) && av_read_frame(fmt, pkt) >= 0; packets++) {
            if (pkt->stream_index == si && avcodec_send_packet(cc, pkt) >= 0 && avcodec_receive_frame(cc, frame) >= 0) {
                got = true;
            }
            av_packet_unref(pkt);
        }
        if (!got && !interruptCb(&ictx)) {
            avcodec_send_packet(cc, nullptr);
            got = avcodec_receive_frame(cc, frame) >= 0;
        }
        if (!got || frame->width <= 0 || frame->height <= 0) {
            break;
        }
        const int tw = std::min(kThumbWidth, frame->width);
        const int th = std::max(2, frame->height * tw / frame->width);
        SwsContext* sws = sws_getContext(frame->width, frame->height, static_cast<AVPixelFormat>(frame->format), tw, th,
                                         AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (sws == nullptr) {
            break;
        }
        QImage img(tw, th, QImage::Format_RGB888);
        uint8_t* dst[4] = {img.bits(), nullptr, nullptr, nullptr};
        int dstStride[4] = {static_cast<int>(img.bytesPerLine()), 0, 0, 0};
        sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dst, dstStride);
        sws_freeContext(sws);
        out = img;
    } while (false);
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&cc);
    avformat_close_input(&fmt);
    return out;
}
#endif

// YouTube 網址 → 影片 id（watch?v=、youtu.be/、shorts/）
QString youtubeId(const QUrl& u)
{
    const QString host = u.host().toLower();
    if (host == QLatin1String("youtu.be")) {
        return u.path().mid(1).section(QLatin1Char('/'), 0, 0);
    }
    if (host.endsWith(QLatin1String("youtube.com"))) {
        const QString v = QUrlQuery(u).queryItemValue(QStringLiteral("v"));
        if (!v.isEmpty()) {
            return v;
        }
        static const QRegularExpression re(QStringLiteral("^/(shorts|embed|live)/([A-Za-z0-9_-]{6,})"));
        const auto m = re.match(u.path());
        if (m.hasMatch()) {
            return m.captured(2);
        }
    }
    return QString();
}

// 背景工作的結果排回 GUI 執行緒；provider 已經不在就丟掉
void post(const std::shared_ptr<bool>& alive, VideoThumbProvider* provider, const QString& key, const QImage& img, const QString& err,
          bool definitive, const char* kind)
{
    QMetaObject::invokeMethod(
        QCoreApplication::instance(),
        [alive, provider, key, img, err, definitive, kind]() {
            if (*alive) {
                provider->complete(key, img, err, definitive, kind);
            }
        },
        Qt::QueuedConnection);
}

class VideoThumbResponse : public QQuickImageResponse
{
public:
    VideoThumbResponse(VideoThumbProvider* provider, const QString& id) : m_Provider(provider), m_Alive(provider->alive()), m_Id(id) {}

    ~VideoThumbResponse() override
    {
        // 在 GUI 執行緒（這個物件搬到那裡了）；還掛在某個工作上就撤掉
        if (m_Joined && *m_Alive) {
            m_Provider->withdraw(m_Key, this);
        }
    }

    QQuickTextureFactory* textureFactory() const override
    {
        return m_Image.isNull() ? nullptr : QQuickTextureFactory::textureFactoryForImage(m_Image);
    }
    QString errorString() const override { return m_Error; }

    // QML 不要了（捲出畫面、換頁、切回清單）：可能從載圖執行緒呼叫，排回 GUI 執行緒撤掉
    void cancel() override
    {
        QMetaObject::invokeMethod(
            this,
            [this]() {
                if (m_Joined && *m_Alive) {
                    m_Provider->withdraw(m_Key, this);
                }
                m_Joined = false;
            },
            Qt::QueuedConnection);
    }

    // provider 交結果（GUI 執行緒）
    void deliver(const QImage& img, const QString& err)
    {
        m_Joined = false;
        m_Image = img;
        m_Error = img.isNull() ? (err.isEmpty() ? QStringLiteral("no thumbnail") : err) : QString();
        emit finished();
    }

    // GUI 執行緒
    void start()
    {
        const QString kind = m_Id.section(QLatin1Char('/'), 0, 0);
        const QString rest = m_Id.section(QLatin1Char('/'), 1);
        if (kind == QLatin1String("local")) {
            startLocal(VideoThumbProvider::decodeId(rest));
        }
        else if (kind == QLatin1String("host")) {
            startHost(rest.section(QLatin1Char('/'), 0, 0), rest.section(QLatin1Char('/'), 1, 1).toInt(),
                      rest.section(QLatin1Char('/'), 2, 2), VideoThumbProvider::decodeId(rest.section(QLatin1Char('/'), 3)));
        }
        else if (kind == QLatin1String("src")) {
            startSource(VideoThumbProvider::decodeId(rest));
        }
        else {
            deliver({}, QStringLiteral("unknown thumbnail id"));
        }
    }

private:
    // 快取命中、已知拿不到、或已經有人在抓：回 true（已處理）。否則回 false，m_Cancel 是新工作的取消旗標
    bool prepare(const QString& key)
    {
        m_Key = key;
        if (m_Provider->knownMissing(key)) {
            deliver({}, QStringLiteral("no thumbnail"));
            return true;
        }
        QImage img(m_Provider->cachePath(key));
        if (!img.isNull()) {
            deliver(img, QString());
            return true;
        }
        m_Joined = true;
        if (m_Provider->joinPending(key, this)) {
            return true;
        }
        m_Cancel = m_Provider->beginPending(key, this);
        return false;
    }

    void startLocal(QString path)
    {
        // 本機檔只接受一般路徑或 file:（code review：rtsp://、udp:// 這類來源交給 FFmpeg 會一直卡在連線上）
        static const QRegularExpression scheme(QStringLiteral("^[A-Za-z][A-Za-z0-9+.-]*:"));
        if (path.startsWith(QLatin1String("file:"), Qt::CaseInsensitive)) {
            path = QUrl(path).toLocalFile();
        }
        else if (scheme.match(path).hasMatch() && !(path.size() > 2 && path.at(1) == QLatin1Char(':'))) {
            deliver({}, QStringLiteral("no thumbnail for this kind of source"));
            return;
        }
        const QFileInfo fi(path);
        if (!fi.isFile()) {
            deliver({}, QStringLiteral("file not found"));
            return;
        }
        if (prepare(QStringLiteral("local|%1|%2|%3").arg(fi.absoluteFilePath()).arg(fi.size()).arg(fi.lastModified().toSecsSinceEpoch()))) {
            return;
        }
#ifdef HAVE_VR_PLAYER
        const auto alive = m_Alive;
        VideoThumbProvider* provider = m_Provider;
        const QString key = m_Key;
        const CancelFlag cancel = m_Cancel;
        m_Provider->decodePool()->start([alive, provider, key, cancel, path]() {
            if (cancel->load()) {
                return;  // 沒人等了
            }
            const QImage img = extractLocalFrame(path, cancel);
            if (!cancel->load()) {
                post(alive, provider, key, img, QStringLiteral("could not decode a frame"), true, "local");
            }
        });
#else
        m_Provider->complete(m_Key, {}, QStringLiteral("local thumbnails need the video player build"), true, "local");
#endif
    }

    void startHost(const QString& uuid, int root, const QString& stamp, const QString& path)
    {
        // 戳記（大小_修改時間）放進快取鍵：主機上同名檔案換掉之後會重新產生（code review）
        auto* cm = m_Provider->engine()->singletonInstance<ComputerManager*>("ComputerManager", "ComputerManager");
        NvAddress address;
        uint16_t httpsPort = 0;
        QSslCertificate serverCert;
        bool online = false;
        if (cm != nullptr) {
            for (NvComputer* x : cm->getComputers()) {
                QReadLocker lock(&x->lock);
                if (x->uuid == uuid) {
                    // 背景工作不拿 NvComputer*（主機可能在工作排隊時被刪掉）：在這裡把連線需要的東西複製下來
                    online = x->state == NvComputer::CS_ONLINE && x->pairState == NvComputer::PS_PAIRED && !x->activeAddress.isNull();
                    address = x->activeAddress;
                    httpsPort = x->activeHttpsPort;
                    serverCert = x->serverCert;
                    break;
                }
            }
        }
        if (!online) {
            // 沒在線上不記成拿不到：主機上線後再捲回來要能補上
            deliver({}, QStringLiteral("the PC is not online"));
            return;
        }
        if (prepare(QStringLiteral("host|%1|%2|%3|%4").arg(uuid).arg(root).arg(stamp).arg(path))) {
            return;
        }
        const auto alive = m_Alive;
        VideoThumbProvider* provider = m_Provider;
        const QString key = m_Key;
        const CancelFlag cancel = m_Cancel;
        const QString query = QStringLiteral("root=%1&path=%2").arg(root).arg(QString::fromLatin1(QUrl::toPercentEncoding(path)));
        m_Provider->hostPool()->start([alive, provider, key, cancel, query, address, httpsPort, serverCert]() {
            if (cancel->load()) {
                return;
            }
            QImage img;
            QString err;
            bool definitive = false;
            try {
                NvHTTP http(address, httpsPort, serverCert);
                const QByteArray data = http.openConnectionToBytes(http.httpsBaseUrl(), QStringLiteral("viple/media/thumb"), query,
                                                                   kHostTimeoutMs, NvHTTP::NvLogLevel::NVLL_NONE);
                img = QImage::fromData(data);
                if (img.isNull()) {
                    // 舊 server 對不認得的路徑回 HTTP 200＋404 的 XML：確定沒有
                    err = QStringLiteral("bad image data");
                    definitive = true;
                }
            } catch (const QtNetworkReplyException& e) {
                err = QString::fromUtf8(e.what());
                definitive = e.getError() == QNetworkReply::ContentNotFoundError;
            } catch (const std::exception& e) {
                err = QString::fromUtf8(e.what());
            } catch (...) {
                err = QStringLiteral("request failed");
            }
            if (!cancel->load() || !img.isNull()) {
                post(alive, provider, key, img, err, definitive, "host");
            }
        });
    }

    void startSource(const QString& source)
    {
        const QUrl u(source);
        if (u.scheme().startsWith(QLatin1String("http"), Qt::CaseInsensitive)) {
            const QString yt = youtubeId(u);
            if (!yt.isEmpty()) {
                startYoutube(yt);
                return;
            }
            if (u.path() == QLatin1String("/viple/media/file")) {
                // 主機分享的檔案：用網址的位址找到那台主機，換成跟「電腦」分頁同一套快取鍵（沒有戳記）
                const QUrlQuery q(u);
                const int root = q.queryItemValue(QStringLiteral("root")).toInt();
                const QString path = q.queryItemValue(QStringLiteral("path"), QUrl::FullyDecoded);
                const QString addr = u.host();
                auto* cm = m_Provider->engine()->singletonInstance<ComputerManager*>("ComputerManager", "ComputerManager");
                if (cm != nullptr) {
                    for (NvComputer* x : cm->getComputers()) {
                        QReadLocker lock(&x->lock);
                        bool match = false;
                        for (const NvAddress& a : {x->activeAddress, x->localAddress, x->remoteAddress, x->ipv6Address,
                                                   x->manualAddress, x->vpnAddress, x->stunAddress}) {
                            match = match || (!a.isNull() && a.address().compare(addr, Qt::CaseInsensitive) == 0);
                        }
                        // 播放網址可能是當時的另一個位址（例如在外面用 Tailscale、在家用區網）：主機自己廣告的網卡位址也算
                        for (const NvNetworkInterface& nic : x->serverAdvertisedInterfaces) {
                            match = match || nic.address.compare(addr, Qt::CaseInsensitive) == 0;
                        }
                        if (match) {
                            const QString uuid = x->uuid;
                            lock.unlock();
                            startHost(uuid, root, QString(), path);
                            return;
                        }
                    }
                }
                deliver({}, QStringLiteral("unknown PC"));
                return;
            }
            deliver({}, QStringLiteral("no thumbnail for this link"));
            return;
        }
        startLocal(source);
    }

    void startYoutube(const QString& videoId)
    {
        if (prepare(QStringLiteral("yt|%1").arg(videoId))) {
            return;
        }
        QNetworkRequest req(QUrl(QStringLiteral("https://i.ytimg.com/vi/%1/mqdefault.jpg").arg(videoId)));
        req.setTransferTimeout(10000);
        QNetworkReply* reply = m_Provider->network()->get(req);
        const auto alive = m_Alive;
        VideoThumbProvider* provider = m_Provider;
        const QString key = m_Key;
        // 回應物件可能先被刪掉：結果交給 provider（以 reply 為 context，provider 在就處理）
        QObject::connect(reply, &QNetworkReply::finished, reply, [alive, provider, key, reply]() {
            reply->deleteLater();
            if (!*alive) {
                return;
            }
            QImage img;
            bool definitive = reply->error() == QNetworkReply::ContentNotFoundError;
            if (reply->error() == QNetworkReply::NoError) {
                img = QImage::fromData(reply->readAll());
                if (img.isNull()) {
                    definitive = true;
                }
                else if (img.width() > kThumbWidth) {
                    img = img.scaledToWidth(kThumbWidth, Qt::SmoothTransformation);
                }
            }
            provider->complete(key, img, reply->errorString(), definitive, "youtube");
        });
    }

    VideoThumbProvider* m_Provider;
    std::shared_ptr<bool> m_Alive;
    QString m_Id;
    QString m_Key;
    CancelFlag m_Cancel;
    bool m_Joined = false;
    QImage m_Image;
    QString m_Error;
};

}  // namespace

VideoThumbProvider::VideoThumbProvider(QQmlEngine* engine)
    : m_Engine(engine), m_DecodePool(new QThreadPool()), m_HostPool(new QThreadPool()), m_Alive(std::make_shared<bool>(true))
{
    // 頭盔上解碼吃 CPU，一次一張；主機那邊本來就一次只做一張，這邊最多兩個請求在路上
    m_DecodePool->setMaxThreadCount(1);
    m_HostPool->setMaxThreadCount(2);
    QDir dir(Path::getBoxArtCacheDir());
    dir.cdUp();
    dir.mkpath(QStringLiteral("vthumbs"));
    m_CacheDir = dir.absoluteFilePath(QStringLiteral("vthumbs"));
}

VideoThumbProvider::~VideoThumbProvider()
{
    // 之後排回來的結果都丟掉；還在排隊的工作清掉，進行中的設取消旗標
    *m_Alive = false;
    for (auto it = m_Pending.begin(); it != m_Pending.end(); ++it) {
        it->cancel->store(true);
    }
    m_Pending.clear();
    m_DecodePool->clear();
    m_HostPool->clear();
    // 結束程式時不要等卡住的連線（code review：~QThreadPool 會無限等）：短暫等一下，做不完就讓它們跟著行程結束
    const bool decodeDone = m_DecodePool->waitForDone(1000);
    const bool hostDone = m_HostPool->waitForDone(1000);
    if (decodeDone) {
        delete m_DecodePool;
    }
    if (hostDone) {
        delete m_HostPool;
    }
    delete m_Network;
}

QQuickImageResponse* VideoThumbProvider::requestImageResponse(const QString& id, const QSize& requestedSize)
{
    Q_UNUSED(requestedSize)
    // 可能在 QML 的載圖執行緒被呼叫：回應物件搬到 GUI 執行緒，在那裡開始（快取、網路、ComputerManager 都在 GUI 執行緒）
    auto* r = new VideoThumbResponse(this, id);
    r->moveToThread(QCoreApplication::instance()->thread());
    QMetaObject::invokeMethod(r, [r]() { r->start(); }, Qt::QueuedConnection);
    return r;
}

QString VideoThumbProvider::encodeId(const QString& s)
{
    QString e = QString::fromLatin1(QUrl::toPercentEncoding(s));
    e.replace(QLatin1Char('~'), QLatin1String("%7E"));
    e.replace(QLatin1Char('%'), QLatin1Char('~'));
    return e;
}

QString VideoThumbProvider::decodeId(const QString& s)
{
    QString e = s;
    e.replace(QLatin1Char('~'), QLatin1Char('%'));
    return QUrl::fromPercentEncoding(e.toLatin1());
}

QNetworkAccessManager* VideoThumbProvider::network()
{
    if (m_Network == nullptr) {
        m_Network = new QNetworkAccessManager();
    }
    return m_Network;
}

QString VideoThumbProvider::cachePath(const QString& key) const
{
    const QByteArray h = QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex();
    return m_CacheDir + QLatin1Char('/') + QString::fromLatin1(h) + QStringLiteral(".jpg");
}

bool VideoThumbProvider::joinPending(const QString& key, QQuickImageResponse* r)
{
    auto it = m_Pending.find(key);
    if (it == m_Pending.end()) {
        return false;
    }
    it->waiters.append(QPointer<QQuickImageResponse>(r));
    return true;
}

std::shared_ptr<std::atomic<bool>> VideoThumbProvider::beginPending(const QString& key, QQuickImageResponse* r)
{
    Pending p;
    p.waiters.append(QPointer<QQuickImageResponse>(r));
    p.cancel = std::make_shared<std::atomic<bool>>(false);
    m_Pending.insert(key, p);
    return p.cancel;
}

void VideoThumbProvider::withdraw(const QString& key, QQuickImageResponse* r)
{
    auto it = m_Pending.find(key);
    if (it == m_Pending.end()) {
        return;
    }
    it->waiters.removeIf([r](const QPointer<QQuickImageResponse>& w) { return w.isNull() || w.data() == r; });
    if (it->waiters.isEmpty()) {
        // 沒人等了：還沒開始的工作會直接跳過，進行中的做完也不交
        it->cancel->store(true);
        m_Pending.erase(it);
    }
}

void VideoThumbProvider::complete(const QString& key, const QImage& img, const QString& err, bool definitive, const char* kind)
{
    if (!img.isNull()) {
        img.save(cachePath(key), "JPG", 82);
    }
    else if (definitive) {
        m_Missing.insert(key);
    }
    (img.isNull() ? m_StatFail : m_StatOk)++;
    if ((m_StatOk + m_StatFail) % 20 == 1) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-THUMB] generated ok=%d failed=%d (last: %s %s)", m_StatOk, m_StatFail, kind,
                    img.isNull() ? "failed" : "ok");
    }
    if (img.isNull() && m_StatFail <= 3) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-THUMB] %s: no thumbnail (%s%s)", kind, qPrintable(err),
                    definitive ? "" : "; will retry later");
    }
    const auto it = m_Pending.find(key);
    if (it == m_Pending.end()) {
        return;  // 沒人等了（全都取消）
    }
    const QList<QPointer<QQuickImageResponse>> waiters = it->waiters;
    m_Pending.erase(it);
    for (const auto& w : waiters) {
        if (!w.isNull()) {
            static_cast<VideoThumbResponse*>(w.data())->deliver(img, err);
        }
    }
}
