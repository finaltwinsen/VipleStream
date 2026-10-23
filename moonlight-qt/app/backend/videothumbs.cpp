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
#include <QRunnable>
#include <QThreadPool>
#include <QUrl>
#include <QUrlQuery>

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
constexpr int kHostTimeoutMs = 15000;  // host 一次只做一張，一頁幾十張時後面的要排隊

#ifdef HAVE_VR_PLAYER
// 頭盔本機檔：跳到片長 10% 的位置（片頭常是黑畫面）解一張，縮成寬 kThumbWidth 的 RGB
QImage extractLocalFrame(const QString& path)
{
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.toUtf8().constData(), nullptr, nullptr) < 0) {
        return {};
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
        for (int packets = 0; !got && packets < 600 && av_read_frame(fmt, pkt) >= 0; packets++) {
            if (pkt->stream_index == si && avcodec_send_packet(cc, pkt) >= 0 && avcodec_receive_frame(cc, frame) >= 0) {
                got = true;
            }
            av_packet_unref(pkt);
        }
        if (!got) {
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

bool computerOnline(NvComputer* c)
{
    QReadLocker lock(&c->lock);
    return c->state == NvComputer::CS_ONLINE && c->pairState == NvComputer::PS_PAIRED && !c->activeAddress.isNull();
}

class VideoThumbResponse : public QQuickImageResponse
{
public:
    VideoThumbResponse(VideoThumbProvider* provider, const QString& id) : m_Provider(provider), m_Id(id) {}

    QQuickTextureFactory* textureFactory() const override
    {
        return m_Image.isNull() ? nullptr : QQuickTextureFactory::textureFactoryForImage(m_Image);
    }
    QString errorString() const override { return m_Error; }

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
                      VideoThumbProvider::decodeId(rest.section(QLatin1Char('/'), 2)));
        }
        else if (kind == QLatin1String("src")) {
            startSource(VideoThumbProvider::decodeId(rest));
        }
        else {
            finish({}, QStringLiteral("unknown thumbnail id"), nullptr);
        }
    }

private:
    // 快取命中就直接回；回 true＝已處理（命中或已知拿不到）
    bool fromCache()
    {
        if (m_Provider->knownMissing(m_Key)) {
            finish({}, QStringLiteral("no thumbnail"), nullptr);
            return true;
        }
        QImage img(m_Provider->cachePath(m_Key));
        if (!img.isNull()) {
            finish(img, QString(), nullptr);
            return true;
        }
        return false;
    }

public:
    // 背景結果（任何執行緒）排回 GUI 執行緒；QPointer 只在 GUI 執行緒上看（回應可能已被 QML 取消、刪掉）
    static void deliver(QPointer<VideoThumbResponse> self, const QImage& img, const QString& err, const char* kind)
    {
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [self, img, err, kind]() {
                if (self) {
                    self->finish(img, err, kind);
                }
            },
            Qt::QueuedConnection);
    }

private:
    void finish(const QImage& img, const QString& err, const char* kind)
    {
        m_Image = img;
        m_Error = img.isNull() ? (err.isEmpty() ? QStringLiteral("no thumbnail") : err) : QString();
        if (kind != nullptr) {
            // 新產生的：存快取或記下拿不到
            if (!img.isNull()) {
                img.save(m_Provider->cachePath(m_Key), "JPG", 82);
            }
            else if (!m_Key.isEmpty()) {
                m_Provider->markMissing(m_Key);
            }
            m_Provider->noteResult(!img.isNull(), kind);
            if (img.isNull() && !m_LoggedFail) {
                m_LoggedFail = true;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-THUMB] %s: no thumbnail (%s)", kind, qPrintable(m_Error));
            }
        }
        emit finished();
    }

    void startLocal(const QString& path)
    {
        const QFileInfo fi(path);
        m_Key = QStringLiteral("local|%1|%2|%3").arg(fi.absoluteFilePath()).arg(fi.size()).arg(fi.lastModified().toSecsSinceEpoch());
        if (fromCache()) {
            return;
        }
#ifdef HAVE_VR_PLAYER
        QPointer<VideoThumbResponse> self(this);
        m_Provider->decodePool()->start([self, path]() {
            const QImage img = extractLocalFrame(path);
            deliver(self, img, QStringLiteral("could not decode a frame"), "local");
        });
#else
        finish({}, QStringLiteral("local thumbnails need the video player build"), "local");
#endif
    }

    void startHost(const QString& uuid, int root, const QString& path)
    {
        m_Key = QStringLiteral("host|%1|%2|%3").arg(uuid).arg(root).arg(path);
        if (fromCache()) {
            return;
        }
        auto* cm = m_Provider->engine()->singletonInstance<ComputerManager*>("ComputerManager", "ComputerManager");
        NvComputer* c = nullptr;
        if (cm != nullptr) {
            for (NvComputer* x : cm->getComputers()) {
                QReadLocker lock(&x->lock);
                if (x->uuid == uuid) {
                    c = x;
                    break;
                }
            }
        }
        if (c == nullptr || !computerOnline(c)) {
            // 沒在線上不記成拿不到：主機上線後再捲回來要能補上
            finish({}, QStringLiteral("the PC is not online"), nullptr);
            return;
        }
        QPointer<VideoThumbResponse> self(this);
        const QString query = QStringLiteral("root=%1&path=%2").arg(root).arg(QString::fromLatin1(QUrl::toPercentEncoding(path)));
        m_Provider->hostPool()->start([self, c, query]() {
            QImage img;
            QString err;
            try {
                NvHTTP http(c);
                const QByteArray data = http.openConnectionToBytes(http.httpsBaseUrl(), QStringLiteral("viple/media/thumb"), query,
                                                                   kHostTimeoutMs, NvHTTP::NvLogLevel::NVLL_NONE);
                img = QImage::fromData(data);
                if (img.isNull()) {
                    err = QStringLiteral("bad image data");
                }
            } catch (const std::exception& e) {
                err = QString::fromUtf8(e.what());
            } catch (...) {
                err = QStringLiteral("request failed");
            }
            deliver(self, img, err, "host");
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
                // 主機分享的檔案：用網址的位址找到那台主機，換成跟「電腦」分頁同一個快取鍵
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
                        // （2026-10-23 頭盔：區網位址的最近播放全部 unknown PC）
                        for (const NvNetworkInterface& nic : x->serverAdvertisedInterfaces) {
                            match = match || nic.address.compare(addr, Qt::CaseInsensitive) == 0;
                        }
                        if (match) {
                            const QString uuid = x->uuid;
                            lock.unlock();
                            startHost(uuid, root, path);
                            return;
                        }
                    }
                }
                finish({}, QStringLiteral("unknown PC"), nullptr);
                return;
            }
            finish({}, QStringLiteral("no thumbnail for this link"), nullptr);
            return;
        }
        startLocal(source);
    }

    void startYoutube(const QString& videoId)
    {
        m_Key = QStringLiteral("yt|%1").arg(videoId);
        if (fromCache()) {
            return;
        }
        QNetworkRequest req(QUrl(QStringLiteral("https://i.ytimg.com/vi/%1/mqdefault.jpg").arg(videoId)));
        req.setTransferTimeout(10000);
        QNetworkReply* reply = m_Provider->network()->get(req);
        connect(reply, &QNetworkReply::finished, this, [this, reply]() {
            reply->deleteLater();
            QImage img;
            if (reply->error() == QNetworkReply::NoError) {
                img = QImage::fromData(reply->readAll());
                if (img.width() > kThumbWidth) {
                    img = img.scaledToWidth(kThumbWidth, Qt::SmoothTransformation);
                }
            }
            finish(img, reply->errorString(), "youtube");
        });
    }

    VideoThumbProvider* m_Provider;
    QString m_Id;
    QString m_Key;
    QImage m_Image;
    QString m_Error;
    bool m_LoggedFail = false;
};

}  // namespace

VideoThumbProvider::VideoThumbProvider(QQmlEngine* engine)
    : m_Engine(engine), m_DecodePool(new QThreadPool()), m_HostPool(new QThreadPool())
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
    m_DecodePool->clear();
    m_HostPool->clear();
    m_DecodePool->waitForDone(2000);
    m_HostPool->waitForDone(2000);
    delete m_DecodePool;
    delete m_HostPool;
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

void VideoThumbProvider::noteResult(bool ok, const char* kind)
{
    (ok ? m_StatOk : m_StatFail)++;
    if ((m_StatOk + m_StatFail) % 20 == 1) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-THUMB] generated ok=%d failed=%d (last: %s %s)", m_StatOk, m_StatFail,
                    kind, ok ? "ok" : "failed");
    }
}
