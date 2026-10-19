// VipleStream 2.0 §VR-PLAYER — 見 mediapipeline.h。

#include "mediapipeline.h"

#include <SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/spherical.h>
#include <libavutil/stereo3d.h>
#include <libswresample/swresample.h>
}

namespace player {

namespace {

// 聲音環狀緩衝：0.5 秒（48 kHz 立體聲）。太大會讓 seek／暫停後的聲音晚到，太小在網路抖一下就斷
constexpr size_t kRingFrames = 24000;
constexpr int kOutRate = 48000;
// 封包佇列：領先播放位置最多這麼多秒、或這麼多位元組（兩條合計）
constexpr double kMaxQueueSec = 10.0;
constexpr int64_t kMaxQueueBytes = 64ll * 1024 * 1024;
// v4l2m2m 的 capture buffer：參考幀（HEVC／VP9 最多 8～16，實際片源多半 ≤ 6）＋幀佇列＋呈現端與 XrVideo 手上的
constexpr int kCaptureBuffers = 6 + MediaPipeline::kMaxFrames + 4;
// 連續這麼多次讀取錯誤（重連也救不回來）就放棄
constexpr int kMaxReadErrors = 40;
// 連續這麼多個封包解碼出錯就重開影像解碼器
constexpr int kMaxDecodeErrRun = 30;
// 聲音時鐘的平滑：每次回呼修正差距的這個比例；差距超過這麼多秒就直接對齊
constexpr double kClockGain = 0.05;
constexpr double kClockResetSec = 0.06;
// 聲音的時間和環狀緩衝寫入端差超過這麼多就補靜音或丟掉重疊（比一個 AAC 幀 21 ms 大）
constexpr double kAudioGapSec = 0.03;

std::string avErr(int e)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(e, buf, sizeof(buf));
    return buf;
}

bool isNetworkUrl(const std::string& url)
{
    const size_t p = url.find("://");
    if (p == std::string::npos || p == 0) {
        return false;
    }
    return url.compare(0, p, "file") != 0;
}

enum AVPixelFormat drmGetFormat(AVCodecContext*, const enum AVPixelFormat* fmts)
{
    for (const enum AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_DRM_PRIME) {
            return *p;
        }
    }
    for (const enum AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; p++) {
        const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(*p);
        if (d != nullptr && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
            return *p;
        }
    }
    return fmts[0];
}

}  // namespace

uint64_t steadyNowNs()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// §VR-PLAYER（V3）：host 分享的檔案。不交給 FFmpeg 的 http 自己處理續傳：server 每次最多回 8 MB，FFmpeg 把比要求短
// 的 206 當成連線中斷、用遞增的延遲重連，seek 時又會卡在沒讀完的舊連線上（2026-10-19 builder 實測 seek 要 5～10 秒或卡死）。
// 這裡自己當 AVIOContext：每一段明確要求「offset 到 offset＋8 MB」（http 的 offset／end_offset 選項，一段一條連線），
// 讀完這一段才開下一段；seek 只改位置，下一次讀的時候從新的位置開一段。
struct RangeReader {
    static constexpr int64_t kChunk = 8ll * 1024 * 1024;  // 同 server 的 k_max_chunk
    std::string url;
    AVDictionary* base = nullptr;   // 每一段都用的選項（憑證、逾時…）
    AVIOInterruptCB interrupt = {};
    AVIOContext* sub = nullptr;     // 目前這一段
    int64_t pos = 0;                // 下一個要讀的位置
    int64_t subEnd = 0;             // 這一段的結尾（不含）
    int64_t size = -1;

    ~RangeReader()
    {
        avio_closep(&sub);
        av_dict_free(&base);
    }

    bool openAt(int64_t at)
    {
        avio_closep(&sub);
        AVDictionary* o = nullptr;
        av_dict_copy(&o, base, 0);
        const int64_t end = size > 0 ? std::min(size, at + kChunk) : at + kChunk;
        av_dict_set_int(&o, "offset", at, 0);
        av_dict_set_int(&o, "end_offset", end, 0);
        const int e = avio_open2(&sub, url.c_str(), AVIO_FLAG_READ, &interrupt, &o);
        av_dict_free(&o);
        if (e < 0) {
            sub = nullptr;
            return false;
        }
        if (size < 0) {
            const int64_t sz = avio_size(sub);
            if (sz > 0) {
                size = sz;
            }
        }
        subEnd = size > 0 ? std::min(size, end) : end;
        return true;
    }

    static int read(void* opaque, uint8_t* buf, int n)
    {
        RangeReader* r = static_cast<RangeReader*>(opaque);
        for (int attempt = 0; attempt < 4; attempt++) {
            if (r->size > 0 && r->pos >= r->size) {
                return AVERROR_EOF;
            }
            if (r->sub == nullptr || r->pos >= r->subEnd) {
                if (!r->openAt(r->pos)) {
                    continue;
                }
            }
            const int want = static_cast<int>(std::min<int64_t>(n, r->subEnd - r->pos));
            const int got = avio_read_partial(r->sub, buf, want);
            if (got > 0) {
                r->pos += got;
                return got;
            }
            avio_closep(&r->sub);  // 這一段斷了或讀完了：下一圈從目前位置再開一段
        }
        return AVERROR(EIO);
    }

    static int64_t seek(void* opaque, int64_t offset, int whence)
    {
        RangeReader* r = static_cast<RangeReader*>(opaque);
        if (whence & AVSEEK_SIZE) {
            return r->size > 0 ? r->size : AVERROR(ENOSYS);
        }
        whence &= ~AVSEEK_FORCE;
        int64_t np = whence == SEEK_SET ? offset : whence == SEEK_CUR ? r->pos + offset : (r->size > 0 ? r->size + offset : -1);
        if (np < 0) {
            return AVERROR(EINVAL);
        }
        if (np != r->pos) {
            avio_closep(&r->sub);  // 不在目前這一段的讀取位置上：下一次讀的時候開新的一段
            r->pos = np;
        }
        return np;
    }
};

struct MediaPipeline::Input {
    AVFormatContext* fmt = nullptr;
    bool eof = false;
    int64_t lastUs = INT64_MIN;  // 最近讀到的封包時間（兩條輸入時，先讀落後的那一條）
    int readErrors = 0;
    RangeReader* reader = nullptr;  // §VR-PLAYER（V3）：自己的分段讀取（host 分享的檔案）
    AVIOContext* pb = nullptr;
};

void MediaPipeline::PacketQueue::clear()
{
    for (PacketItem& it : q) {
        av_packet_free(&it.pkt);
    }
    q.clear();
    bytes = 0;
    durationSec = 0.0;
}

MediaPipeline::MediaPipeline() = default;

MediaPipeline::~MediaPipeline()
{
    close();
}

int MediaPipeline::interruptCb(void* opaque)
{
    return static_cast<MediaPipeline*>(opaque)->m_Abort.load(std::memory_order_acquire) ? 1 : 0;
}

void MediaPipeline::abort()
{
    m_Abort.store(true, std::memory_order_release);
    // 每一個等待的條件都在自己的鎖裡檢查 m_Abort：先拿一下那把鎖再叫醒，避免「剛檢查完、還沒睡」的那一條漏掉這次喚醒
    { std::lock_guard<std::mutex> lk(m_PktMutex); }
    { std::lock_guard<std::mutex> lk(m_FrameMutex); }
    { std::lock_guard<std::mutex> lk(m_AudioMutex); }
    m_PktCv.notify_all();
    m_DemuxCv.notify_all();
    m_FrameCv.notify_all();
    m_FrameSpaceCv.notify_all();
    m_AudioSpaceCv.notify_all();
}

bool MediaPipeline::openInput(Input& in, const std::string& url, std::string* error)
{
    in.fmt = avformat_alloc_context();
    if (in.fmt == nullptr) {
        *error = "avformat_alloc_context failed";
        return false;
    }
    in.fmt->interrupt_callback.callback = &MediaPipeline::interruptCb;
    in.fmt->interrupt_callback.opaque = this;
    AVDictionary* opts = nullptr;
    if (isNetworkUrl(url)) {
        // 網路來源：斷線自動重連、15 秒沒有資料就當成錯誤（不然會永遠卡在讀取）
        av_dict_set(&opts, "reconnect", "1", 0);
        av_dict_set(&opts, "reconnect_streamed", "1", 0);
        av_dict_set(&opts, "reconnect_on_network_error", "1", 0);
        av_dict_set(&opts, "reconnect_delay_max", "4", 0);
        av_dict_set(&opts, "rw_timeout", "15000000", 0);
        if (!m_Opt.userAgent.empty()) {
            av_dict_set(&opts, "user_agent", m_Opt.userAgent.c_str(), 0);
        }
        if (!m_Opt.httpHeaders.empty()) {
            av_dict_set(&opts, "headers", m_Opt.httpHeaders.c_str(), 0);
        }
        if (!m_Opt.tlsCertFile.empty()) {
            // host 的分享端點（V3）：server 用配對憑證驗我們；server 自己的憑證是自簽的（名稱也對不上 IP），FFmpeg 9 預設
            // 會驗它而失敗，所以只有這種連線不驗（一般 https 網址照樣驗）。讀取走 RangeReader（見上面）
            in.reader = new RangeReader();
            in.reader->url = url;
            in.reader->interrupt = in.fmt->interrupt_callback;
            av_dict_set(&in.reader->base, "cert_file", m_Opt.tlsCertFile.c_str(), 0);
            av_dict_set(&in.reader->base, "key_file", m_Opt.tlsKeyFile.c_str(), 0);
            av_dict_set(&in.reader->base, "tls_verify", "0", 0);
            av_dict_set(&in.reader->base, "rw_timeout", "15000000", 0);
            if (!in.reader->openAt(0)) {
                *error = "cannot connect to the PC's shared folder";
                return false;
            }
            const int kBuf = 256 * 1024;
            in.pb = avio_alloc_context(static_cast<unsigned char*>(av_malloc(kBuf)), kBuf, 0, in.reader, &RangeReader::read,
                                       nullptr, &RangeReader::seek);
            in.fmt->pb = in.pb;
            in.fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
            av_dict_free(&opts);  // 這些是給 http 的；自訂讀取時 demuxer 用不到
        }
    }
    int e = avformat_open_input(&in.fmt, url.c_str(), nullptr, &opts);
    av_dict_free(&opts);
    if (e < 0) {
        in.fmt = nullptr;  // avformat_open_input 失敗時已經釋放
        *error = "cannot open the video (" + avErr(e) + ")";
        return false;
    }
    e = avformat_find_stream_info(in.fmt, nullptr);
    if (e < 0) {
        *error = "cannot read the stream information (" + avErr(e) + ")";
        return false;
    }
    return true;
}

bool MediaPipeline::open(const MediaOptions& options, std::string* error)
{
    m_Opt = options;
    m_Volume.store(std::clamp(options.volume, 0.0f, 2.0f), std::memory_order_relaxed);
    avformat_network_init();

    m_Inputs.push_back(new Input());
    if (!openInput(*m_Inputs[0], options.url, error)) {
        return false;
    }
    AVFormatContext* fmt0 = m_Inputs[0]->fmt;
    m_VideoStream = av_find_best_stream(fmt0, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (m_VideoStream >= 0 && (fmt0->streams[m_VideoStream]->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
        m_VideoStream = -1;  // 音樂檔的封面圖不是影像
    }
    if (m_VideoStream >= 0) {
        m_VideoInput = 0;
    }
    if (options.enableAudio) {
        if (!options.audioUrl.empty()) {
            m_Inputs.push_back(new Input());
            if (!openInput(*m_Inputs[1], options.audioUrl, error)) {
                *error = "audio: " + *error;
                return false;
            }
            m_AudioStream = av_find_best_stream(m_Inputs[1]->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
            if (m_AudioStream >= 0) {
                m_AudioInput = 1;
            }
        }
        else {
            m_AudioStream = av_find_best_stream(fmt0, AVMEDIA_TYPE_AUDIO, -1, m_VideoStream, nullptr, 0);
            if (m_AudioStream >= 0) {
                m_AudioInput = 0;
            }
        }
    }
    if (m_VideoStream < 0) {
        *error = "the file has no video stream";
        return false;
    }

    // ── 資訊 ──
    AVStream* vs = fmt0->streams[m_VideoStream];
    const AVCodecParameters* vp = vs->codecpar;
    m_Info.hasVideo = true;
    m_Info.width = vp->width;
    m_Info.height = vp->height;
    const AVRational sar = av_guess_sample_aspect_ratio(fmt0, vs, nullptr);
    const double sarV = sar.num > 0 && sar.den > 0 ? av_q2d(sar) : 1.0;
    m_Info.frameAspect = vp->height > 0 ? static_cast<float>(vp->width * sarV / vp->height) : 16.0f / 9.0f;
    const AVRational fr = av_guess_frame_rate(fmt0, vs, nullptr);
    m_Info.fps = fr.num > 0 && fr.den > 0 ? av_q2d(fr) : 0.0;
    m_Info.container = fmt0->iformat != nullptr && fmt0->iformat->name != nullptr ? fmt0->iformat->name : "?";
    m_Info.videoCodec = avcodec_get_name(vp->codec_id);
    const char* prof = avcodec_profile_name(vp->codec_id, vp->profile);
    m_Info.videoProfile = prof != nullptr ? prof : "";
    const AVPixFmtDescriptor* pd = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(vp->format));
    m_Info.videoBits = pd != nullptr ? pd->comp[0].depth : 8;
    m_Info.durationSec = fmt0->duration > 0 ? static_cast<double>(fmt0->duration) / AV_TIME_BASE : 0.0;
    m_Info.seekable = m_Info.durationSec > 0.0 && (fmt0->pb == nullptr || (fmt0->pb->seekable & AVIO_SEEKABLE_NORMAL) != 0 ||
                                                   std::strcmp(m_Info.container.c_str(), "hls") == 0);
    const AVDictionaryEntry* t = av_dict_get(fmt0->metadata, "title", nullptr, 0);
    if (t != nullptr && t->value != nullptr) {
        m_Info.title = t->value;
    }
    m_VideoTb = av_q2d(vs->time_base);
    // 360°／立體中繼資料
    if (const AVPacketSideData* sd = av_packet_side_data_get(vp->coded_side_data, vp->nb_coded_side_data, AV_PKT_DATA_SPHERICAL)) {
        const AVSphericalMapping* sm = reinterpret_cast<const AVSphericalMapping*>(sd->data);
        m_Info.hints.haveSpherical = true;
        if (sm->projection == AV_SPHERICAL_EQUIRECTANGULAR || sm->projection == AV_SPHERICAL_EQUIRECTANGULAR_TILE) {
            m_Info.hints.sphericalEquirect = true;
            const double lr = (static_cast<double>(sm->bound_left) + static_cast<double>(sm->bound_right)) / 4294967296.0;
            m_Info.hints.spherical180 = lr > 0.4;
        }
        else if (sm->projection == AV_SPHERICAL_HALF_EQUIRECTANGULAR) {
            m_Info.hints.sphericalEquirect = true;
            m_Info.hints.spherical180 = true;
        }
    }
    if (const AVPacketSideData* sd = av_packet_side_data_get(vp->coded_side_data, vp->nb_coded_side_data, AV_PKT_DATA_STEREO3D)) {
        const AVStereo3D* s3 = reinterpret_cast<const AVStereo3D*>(sd->data);
        m_Info.hints.haveStereo = true;
        m_Info.hints.stereo = s3->type == AV_STEREO3D_SIDEBYSIDE ? playerfmt::Stereo::LeftRight
                              : s3->type == AV_STEREO3D_TOPBOTTOM ? playerfmt::Stereo::TopBottom
                                                                  : playerfmt::Stereo::Mono;
    }
    if (m_AudioStream >= 0) {
        AVStream* as = m_Inputs[static_cast<size_t>(m_AudioInput)]->fmt->streams[m_AudioStream];
        m_Info.hasAudio = true;
        m_Info.audioCodec = avcodec_get_name(as->codecpar->codec_id);
        m_AudioTb = av_q2d(as->time_base);
    }

    // ── 解碼器 ──
    if (!openVideoDecoder(options.hwDecode, error)) {
        return false;
    }
    if (m_AudioStream >= 0) {
        std::string aerr;
        if (!openAudioDecoder(&aerr) || !openAudioDevice(&aerr)) {
            // 聲音開不起來不擋播放：當成沒有聲音的片
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-PLAYER] audio disabled: %s", aerr.c_str());
            avcodec_free_context(&m_AudioCtx);
            m_AudioStream = -1;
            m_AudioInput = -1;
            m_Info.hasAudio = false;
        }
    }

    // 起始位置
    if (options.startSec > 0.0 && m_Info.seekable) {
        m_SeekReq = true;
        m_SeekTarget = std::min(options.startSec, std::max(0.0, m_Info.durationSec - 1.0));
        m_SeekSerial = 0;
        m_PausedAt = m_SeekTarget;
        m_DropBefore = m_SeekTarget;
        m_DropSerial = 0;
        m_AudioDropBefore = m_SeekTarget;
        m_AudioDropSerial = 0;
    }

    m_Opened = true;
    m_DemuxThread = std::thread([this] { demuxThread(); });
    m_VideoThread = std::thread([this] { videoThread(); });
    if (m_Info.hasAudio) {
        m_AudioThread = std::thread([this] { audioThread(); });
        SDL_PauseAudioDevice(m_AudioDev, 0);
    }
    return true;
}

bool MediaPipeline::openVideoDecoder(bool allowHw, std::string* error)
{
    AVStream* vs = m_Inputs[0]->fmt->streams[m_VideoStream];
    const AVCodecParameters* vp = vs->codecpar;
    std::vector<const AVCodec*> candidates;
#if defined(__linux__)
    // 頭盔的硬體解碼器（iris）：H.264／HEVC 8-bit、VP9 profile 0；10-bit 實際用不了（vr 文件：Frame 解不了 Main10）
    const bool hwOk = allowHw && m_Info.videoBits <= 8 &&
                      (vp->codec_id == AV_CODEC_ID_H264 || vp->codec_id == AV_CODEC_ID_HEVC ||
                       (vp->codec_id == AV_CODEC_ID_VP9 && vp->profile <= 0));
    if (hwOk) {
        const std::string name = std::string(avcodec_get_name(vp->codec_id)) + "_v4l2m2m";
        if (const AVCodec* c = avcodec_find_decoder_by_name(name.c_str())) {
            candidates.push_back(c);
        }
    }
#else
    (void)allowHw;
#endif
    if (vp->codec_id == AV_CODEC_ID_AV1) {
        if (const AVCodec* c = avcodec_find_decoder_by_name("libdav1d")) {
            candidates.push_back(c);
        }
    }
    if (const AVCodec* c = avcodec_find_decoder(vp->codec_id)) {
        candidates.push_back(c);
    }
    std::string why;
    for (const AVCodec* codec : candidates) {
        AVCodecContext* ctx = avcodec_alloc_context3(codec);
        if (ctx == nullptr) {
            continue;
        }
        avcodec_parameters_to_context(ctx, vp);
        ctx->pkt_timebase = vs->time_base;
        AVDictionary* opts = nullptr;
        const bool m2m = std::strstr(codec->name, "_v4l2m2m") != nullptr;
        if (m2m) {
            ctx->get_format = drmGetFormat;
            ctx->pix_fmt = AV_PIX_FMT_DRM_PRIME;
            ctx->thread_count = 1;
            av_dict_set_int(&opts, "num_capture_buffers", kCaptureBuffers, 0);
            av_dict_set_int(&opts, "num_output_buffers", 8, 0);
        }
        else {
            ctx->thread_count = 0;  // 自動：CPU 數
            ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        }
        const int e = avcodec_open2(ctx, codec, &opts);
        av_dict_free(&opts);
        if (e < 0) {
            why += std::string(codec->name) + ": " + avErr(e) + "; ";
            avcodec_free_context(&ctx);
            continue;
        }
        m_VideoCtx = ctx;
        m_VideoHw = m2m;
        m_Info.decoder = codec->name;
        if (m2m && vp->codec_id == AV_CODEC_ID_VP9) {
            // VP9 的「超級幀」（一包裡一張隱藏的參考幀＋一張要顯示的幀，YouTube 的串流幾乎每包都是）拆成一包一張再給硬體
            // 解碼器：iris 吃整包時會多吐出隱藏的那張（時間戳和前一張相同），從片中 seek 之後還會一直出錯
            const AVBitStreamFilter* bf = av_bsf_get_by_name("vp9_superframe_split");
            if (bf != nullptr && av_bsf_alloc(bf, &m_VideoBsf) >= 0) {
                avcodec_parameters_copy(m_VideoBsf->par_in, vp);
                m_VideoBsf->time_base_in = vs->time_base;
                if (av_bsf_init(m_VideoBsf) < 0) {
                    av_bsf_free(&m_VideoBsf);
                }
            }
        }
        return true;
    }
    *error = std::string("no usable decoder for ") + avcodec_get_name(vp->codec_id) + (why.empty() ? "" : " (" + why + ")");
    return false;
}

bool MediaPipeline::openAudioDecoder(std::string* error)
{
    AVStream* as = m_Inputs[static_cast<size_t>(m_AudioInput)]->fmt->streams[m_AudioStream];
    const AVCodec* codec = avcodec_find_decoder(as->codecpar->codec_id);
    if (codec == nullptr) {
        *error = std::string("no decoder for ") + avcodec_get_name(as->codecpar->codec_id);
        return false;
    }
    m_AudioCtx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(m_AudioCtx, as->codecpar);
    m_AudioCtx->pkt_timebase = as->time_base;
    const int e = avcodec_open2(m_AudioCtx, codec, nullptr);
    if (e < 0) {
        *error = std::string("avcodec_open2(") + codec->name + "): " + avErr(e);
        return false;
    }
    return true;
}

bool MediaPipeline::openAudioDevice(std::string* error)
{
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        *error = std::string("SDL audio: ") + SDL_GetError();
        return false;
    }
    SDL_AudioSpec want = {}, have = {};
    want.freq = kOutRate;
    want.format = AUDIO_F32SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = &MediaPipeline::sdlAudioCallback;
    want.userdata = this;
    m_AudioDev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (m_AudioDev == 0) {
        *error = std::string("SDL_OpenAudioDevice: ") + SDL_GetError();
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return false;
    }
    m_DevBufferFrames = have.samples > 0 ? have.samples : 1024;
    m_Ring.assign(kRingFrames * 2, 0.0f);
    return true;
}

void MediaPipeline::close()
{
    if (!m_Opened && m_Inputs.empty()) {
        return;
    }
    abort();
    if (m_AudioDev != 0) {
        SDL_PauseAudioDevice(m_AudioDev, 1);
    }
    for (std::thread* t : {&m_DemuxThread, &m_VideoThread, &m_AudioThread}) {
        if (t->joinable()) {
            t->join();
        }
    }
    if (m_AudioDev != 0) {
        SDL_CloseAudioDevice(m_AudioDev);
        m_AudioDev = 0;
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    {
        std::lock_guard<std::mutex> lk(m_PktMutex);
        m_VideoQ.clear();
        m_AudioQ.clear();
    }
    {
        std::lock_guard<std::mutex> lk(m_FrameMutex);
        for (FrameItem& f : m_Frames) {
            av_frame_free(&f.frame);
        }
        m_Frames.clear();
    }
    avcodec_free_context(&m_VideoCtx);
    av_bsf_free(&m_VideoBsf);
    avcodec_free_context(&m_AudioCtx);
    swr_free(&m_Swr);
    for (Input* in : m_Inputs) {
        if (in->fmt != nullptr) {
            avformat_close_input(&in->fmt);  // 自訂讀取（AVFMT_FLAG_CUSTOM_IO）時不會關 pb
        }
        if (in->pb != nullptr) {
            av_freep(&in->pb->buffer);
            avio_context_free(&in->pb);
        }
        delete in->reader;
        delete in;
    }
    m_Inputs.clear();
    m_Opened = false;
    // 關完可以再 open 一次（例：網站給的影像網址被拒，重新解析後再開）
    m_Abort.store(false, std::memory_order_release);
}

// ── 封包佇列 ──

void MediaPipeline::pushPacket(PacketQueue& pq, AVPacket* pkt, int serial, double durSec)
{
    PacketItem it;
    it.pkt = pkt;
    it.serial = serial;
    pq.q.push_back(it);
    if (pkt != nullptr) {
        pq.bytes += pkt->size;
        pq.durationSec += durSec;
    }
}

bool MediaPipeline::popPacket(PacketQueue& pq, PacketItem* out, std::condition_variable& cv)
{
    std::unique_lock<std::mutex> lk(m_PktMutex);
    cv.wait(lk, [&] { return m_Abort.load() || !pq.q.empty(); });
    if (m_Abort.load()) {
        return false;
    }
    *out = pq.q.front();
    pq.q.pop_front();
    if (out->pkt != nullptr) {
        pq.bytes -= out->pkt->size;
        const double tb = &pq == &m_VideoQ ? m_VideoTb : m_AudioTb;
        pq.durationSec = std::max(0.0, pq.durationSec - (out->pkt->duration > 0 ? out->pkt->duration * tb : 0.0));
    }
    lk.unlock();
    m_DemuxCv.notify_one();
    return true;
}

void MediaPipeline::doSeek(double sec, int serial)
{
    const int64_t ts = static_cast<int64_t>(sec * AV_TIME_BASE);
    const uint64_t t0 = steadyNowNs();
    for (Input* in : m_Inputs) {
        // 往前找關鍵幀（max_ts＝目標），之後精準 seek 把目標之前的幀丟掉
        const int e = avformat_seek_file(in->fmt, -1, INT64_MIN, ts, ts, 0);
        if (e < 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-PLAYER] seek to %.2f s failed: %s", sec, avErr(e).c_str());
        }
        in->eof = false;
        in->lastUs = INT64_MIN;
        in->readErrors = 0;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-PLAYER] seek to %.2f s: demuxer seek took %llu ms", sec,
                static_cast<unsigned long long>((steadyNowNs() - t0) / 1000000ull));
    {
        std::lock_guard<std::mutex> lk(m_PktMutex);
        m_VideoQ.clear();
        m_AudioQ.clear();
        PacketItem flush;
        flush.serial = serial;
        flush.flush = true;
        m_VideoQ.q.push_back(flush);
        if (m_Info.hasAudio) {
            m_AudioQ.q.push_back(flush);
        }
        m_DemuxEof = false;
    }
    m_PktCv.notify_all();
}

void MediaPipeline::demuxThread()
{
    int serial = 0;
    AVPacket* pkt = av_packet_alloc();
    while (!m_Abort.load()) {
        // seek 要求
        bool doIt = false;
        double target = 0.0;
        {
            std::lock_guard<std::mutex> lk(m_PktMutex);
            if (m_SeekReq) {
                m_SeekReq = false;
                doIt = true;
                target = m_SeekTarget;
                serial = m_SeekSerial;
            }
        }
        if (doIt) {
            doSeek(target, serial);
            continue;
        }
        // 佇列滿了就等（或等 seek）
        {
            std::unique_lock<std::mutex> lk(m_PktMutex);
            m_DemuxCv.wait(lk, [&] {
                if (m_Abort.load() || m_SeekReq) {
                    return true;
                }
                const int64_t bytes = m_VideoQ.bytes + m_AudioQ.bytes;
                const bool videoFull = m_VideoQ.durationSec > kMaxQueueSec || m_VideoQ.q.size() > 2000;
                const bool audioFull = !m_Info.hasAudio || m_AudioQ.durationSec > kMaxQueueSec || m_AudioQ.q.size() > 4000;
                // 兩條都滿、或位元組超過上限才等；只有一條滿時繼續讀（另一條可能很稀疏）
                return !(bytes > kMaxQueueBytes || (videoFull && audioFull));
            });
            if (m_Abort.load()) {
                break;
            }
            if (m_SeekReq) {
                continue;
            }
        }
        // 兩條輸入時先讀落後的那一條
        int pick = -1;
        for (size_t i = 0; i < m_Inputs.size(); i++) {
            if (m_Inputs[i]->eof) {
                continue;
            }
            if (pick < 0 || m_Inputs[i]->lastUs < m_Inputs[static_cast<size_t>(pick)]->lastUs) {
                pick = static_cast<int>(i);
            }
        }
        if (pick < 0) {
            // 全部讀完：等 seek 或結束
            std::unique_lock<std::mutex> lk(m_PktMutex);
            m_DemuxEof = true;
            m_DemuxCv.wait(lk, [&] { return m_Abort.load() || m_SeekReq; });
            continue;
        }
        Input& in = *m_Inputs[static_cast<size_t>(pick)];
        const int r = av_read_frame(in.fmt, pkt);
        if (r < 0) {
            if (m_Abort.load()) {
                break;
            }
            const bool eof = r == AVERROR_EOF || (in.fmt->pb != nullptr && avio_feof(in.fmt->pb));
            if (!eof && ++in.readErrors < kMaxReadErrors) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            if (!eof) {
                std::lock_guard<std::mutex> lk(m_StatsMutex);
                m_Fatal = "reading the video failed (" + avErr(r) + ")";
            }
            in.eof = true;
            std::lock_guard<std::mutex> lk(m_PktMutex);
            if (pick == m_VideoInput) {
                pushPacket(m_VideoQ, nullptr, serial, 0.0);
            }
            if (pick == m_AudioInput) {
                pushPacket(m_AudioQ, nullptr, serial, 0.0);
            }
            m_PktCv.notify_all();
            continue;
        }
        in.readErrors = 0;
        {
            std::lock_guard<std::mutex> lk(m_StatsMutex);
            m_Stats.demuxBytes += static_cast<uint64_t>(pkt->size);
        }
        const AVStream* st = in.fmt->streams[pkt->stream_index];
        const int64_t ts = pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
        if (ts != AV_NOPTS_VALUE) {
            in.lastUs = av_rescale_q(ts, st->time_base, AVRational{1, 1000000});
        }
        const bool isVideo = pick == m_VideoInput && pkt->stream_index == m_VideoStream;
        const bool isAudio = pick == m_AudioInput && pkt->stream_index == m_AudioStream;
        if (!isVideo && !isAudio) {
            av_packet_unref(pkt);
            continue;
        }
        AVPacket* copy = av_packet_alloc();
        av_packet_move_ref(copy, pkt);
        const double dur = copy->duration > 0 ? copy->duration * av_q2d(st->time_base) : 0.0;
        {
            std::lock_guard<std::mutex> lk(m_PktMutex);
            if (m_SeekReq) {
                av_packet_free(&copy);  // 已經要 seek 了，這一包作廢
                continue;
            }
            pushPacket(isVideo ? m_VideoQ : m_AudioQ, copy, serial, dur);
        }
        {
            std::lock_guard<std::mutex> lk(m_StatsMutex);
            (isVideo ? m_Stats.videoPackets : m_Stats.audioPackets)++;
        }
        m_PktCv.notify_all();
    }
    av_packet_free(&pkt);
}

// ── 影像解碼 ──

void MediaPipeline::videoThread()
{
    AVFrame* f = av_frame_alloc();
    int cur = -1;
    double lastPts = -1.0;
    const double frameDur = m_Info.fps > 1.0 ? 1.0 / m_Info.fps : 1.0 / 30.0;
    uint64_t deliverWaitNs = 0;  // 這一包解出的幀等佇列空位的時間（不算進解碼時間）
    bool sentSinceFlush = false;  // 上一次 flush 之後送過封包
    bool waitKey = true;          // 等關鍵幀（開頭與每次 seek 之後）
    int errRun = 0;               // 連續解碼錯誤的次數
    auto deliver = [&](AVFrame* src) {
        int64_t ts = src->best_effort_timestamp;
        if (ts == AV_NOPTS_VALUE) {
            ts = src->pts;
        }
        double pts = ts != AV_NOPTS_VALUE ? ts * m_VideoTb : (lastPts >= 0.0 ? lastPts + frameDur : 0.0);
        lastPts = pts;
        {
            std::unique_lock<std::mutex> lk(m_FrameMutex);
            if (m_DropSerial == cur && pts < m_DropBefore - frameDur * 0.5) {
                lk.unlock();
                av_frame_unref(src);
                std::lock_guard<std::mutex> sl(m_StatsMutex);
                m_Stats.seekDropped++;
                return;
            }
            const uint64_t w0 = steadyNowNs();
            m_FrameSpaceCv.wait(lk, [&] {
                return m_Abort.load() || static_cast<int>(m_Frames.size()) < kMaxFrames || m_Serial.load() != cur;
            });
            deliverWaitNs += steadyNowNs() - w0;
            if (m_Abort.load() || m_Serial.load() != cur) {
                av_frame_unref(src);
                return;
            }
            FrameItem it;
            it.frame = av_frame_alloc();
            av_frame_move_ref(it.frame, src);
            it.pts = pts;
            it.serial = cur;
            m_Frames.push_back(it);
        }
        {
            std::lock_guard<std::mutex> sl(m_StatsMutex);
            m_Stats.decoded++;
        }
        m_FrameCv.notify_all();
        openStartGateIfReady();
    };
    while (!m_Abort.load()) {
        PacketItem it;
        if (!popPacket(m_VideoQ, &it, m_PktCv)) {
            break;
        }
        if (it.serial != m_Serial.load()) {
            av_packet_free(&it.pkt);
            continue;
        }
        if (it.flush || it.serial != cur) {
            // 還沒送過任何封包時不 flush：v4l2m2m 在還沒開始解碼前 flush 會進到不正常的狀態（開頭就 seek 的那一次）
            if (sentSinceFlush) {
                avcodec_flush_buffers(m_VideoCtx);
            }
            if (m_VideoBsf != nullptr) {
                av_bsf_flush(m_VideoBsf);
            }
            sentSinceFlush = false;
            waitKey = true;
            cur = it.serial;
            lastPts = -1.0;
            {
                std::lock_guard<std::mutex> lk(m_FrameMutex);
                m_VideoEof = false;
            }
            if (it.flush) {
                continue;
            }
        }
        // seek 之後從關鍵幀開始送：demuxer 不一定剛好落在關鍵幀上（沒有索引的 webm 是照位元組估的），中間的幀送進去只會出錯，
        // 硬體解碼器還可能因此一直錯下去
        if (waitKey && it.pkt != nullptr) {
            if (!(it.pkt->flags & AV_PKT_FLAG_KEY)) {
                av_packet_free(&it.pkt);
                std::lock_guard<std::mutex> sl(m_StatsMutex);
                m_Stats.seekDropped++;
                continue;
            }
            waitKey = false;
        }
        if (it.pkt != nullptr) {
            sentSinceFlush = true;
        }
        const uint64_t t0 = steadyNowNs();
        deliverWaitNs = 0;
        auto decodeOne = [&](AVPacket* pkt) {
            bool sent = false;
            int spins = 0;
            while (!sent && !m_Abort.load()) {
                const int r = avcodec_send_packet(m_VideoCtx, pkt);  // nullptr＝結尾，把剩下的幀吐出來
                if (r == AVERROR(EAGAIN)) {
                    // 解碼器滿了：先取幀再重送
                }
                else {
                    sent = true;
                    if (r < 0 && r != AVERROR_EOF) {
                        std::lock_guard<std::mutex> sl(m_StatsMutex);
                        m_Stats.decodeErrors++;
                        errRun++;
                    }
                }
                bool got = false;
                while (!m_Abort.load()) {
                    const int rr = avcodec_receive_frame(m_VideoCtx, f);
                    if (rr == 0) {
                        got = true;
                        errRun = 0;
                        deliver(f);
                        continue;
                    }
                    if (rr == AVERROR_EOF) {
                        {
                            // 這一段結尾可能是 seek 之前的序號：那時 seek() 已經把旗標清掉，不可以設到新的序號上
                            std::lock_guard<std::mutex> lk(m_FrameMutex);
                            if (m_Serial.load() == cur) {
                                m_VideoEof = true;
                            }
                        }
                        m_FrameCv.notify_all();
                        openStartGateIfReady();  // 會自己拿 m_FrameMutex，不可以在上面那個區塊裡呼叫
                        avcodec_flush_buffers(m_VideoCtx);
                    }
                    break;
                }
                if (!sent && !got && ++spins > 2) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));  // v4l2m2m：capture 還沒好
                }
            }
        };
        if (m_VideoBsf != nullptr && it.pkt != nullptr) {
            // VP9 給硬體解碼器之前先拆超級幀（見 openVideoDecoder）
            if (av_bsf_send_packet(m_VideoBsf, it.pkt) >= 0) {
                AVPacket* split = av_packet_alloc();
                while (av_bsf_receive_packet(m_VideoBsf, split) == 0) {
                    decodeOne(split);
                    av_packet_unref(split);
                }
                av_packet_free(&split);
            }
        }
        else {
            decodeOne(it.pkt);
        }
        if (errRun >= kMaxDecodeErrRun) {
            // 連續出錯：改用軟體解碼，從下一個關鍵幀接著解。2026-10-19 頭盔：YouTube 的 4K VP9 從片中 seek 之後，硬體
            // 解碼器每一包都出錯、重開也一樣，軟體解碼同一條串流完全正常
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-PLAYER] %d decode errors in a row with %s - switching to software decoding",
                        errRun, m_Info.decoder.c_str());
            avcodec_free_context(&m_VideoCtx);
            av_bsf_free(&m_VideoBsf);
            std::string derr;
            if (!openVideoDecoder(false, &derr)) {
                std::lock_guard<std::mutex> sl(m_StatsMutex);
                m_Fatal = "the video decoder stopped working (" + derr + ")";
                av_packet_free(&it.pkt);
                break;
            }
            errRun = 0;
            waitKey = true;
            sentSinceFlush = false;
        }
        const uint64_t spent = steadyNowNs() - t0;
        const uint32_t us = static_cast<uint32_t>((spent > deliverWaitNs ? spent - deliverWaitNs : 0) / 1000ull);
        if (it.pkt != nullptr) {
            std::lock_guard<std::mutex> sl(m_StatsMutex);
            m_Stats.decodeUsMax = std::max(m_Stats.decodeUsMax, us);
        }
        av_packet_free(&it.pkt);
    }
    av_frame_free(&f);
}

// ── 聲音 ──

void MediaPipeline::audioThread()
{
    AVFrame* f = av_frame_alloc();
    int cur = -1;
    std::vector<float> buf;
    AVChannelLayout swrIn = {};
    int swrInFmt = -1, swrInRate = 0;
    auto writeRing = [&](const float* data, int frames, double pts) {
        int off = 0;
        while (off < frames && !m_Abort.load()) {
            std::unique_lock<std::mutex> lk(m_AudioMutex);
            if (m_Serial.load() != cur) {
                return;
            }
            if (m_RingSerial != cur) {
                m_RingSerial = cur;
                m_RingRead = 0;
                m_RingFill = 0;
                m_RingReadPts = pts + off / static_cast<double>(kOutRate);
            }
            else if (off == 0) {
                // 跟環狀緩衝的寫入端對時間：中間少了一段（解碼錯誤、來源本身有空隙）就補靜音，重疊就丟掉重疊的部分。
                // 不這樣做的話時鐘會一直以為聲音還連續，影像和聲音從此差一截
                const double writePts = m_RingReadPts + static_cast<double>(m_RingFill) / kOutRate;
                const double gap = pts - writePts;
                if (gap > kAudioGapSec && gap < 2.0 && m_RingFill + static_cast<size_t>(gap * kOutRate) < kRingFrames) {
                    const size_t silence = static_cast<size_t>(gap * kOutRate);
                    size_t w = (m_RingRead + m_RingFill) % kRingFrames;
                    for (size_t i = 0; i < silence; i++) {
                        m_Ring[w * 2] = 0.0f;
                        m_Ring[w * 2 + 1] = 0.0f;
                        w = (w + 1) % kRingFrames;
                    }
                    m_RingFill += silence;
                }
                else if (gap < -kAudioGapSec) {
                    const int skip = std::min(frames, static_cast<int>(-gap * kOutRate));
                    off = skip;
                    if (off >= frames) {
                        return;
                    }
                }
            }
            m_AudioSpaceCv.wait(lk, [&] { return m_Abort.load() || m_RingFill < kRingFrames || m_Serial.load() != cur; });
            if (m_Abort.load() || m_Serial.load() != cur) {
                return;
            }
            const size_t space = kRingFrames - m_RingFill;
            const size_t n = std::min(space, static_cast<size_t>(frames - off));
            size_t w = (m_RingRead + m_RingFill) % kRingFrames;
            for (size_t i = 0; i < n; i++) {
                m_Ring[w * 2] = data[(off + i) * 2];
                m_Ring[w * 2 + 1] = data[(off + i) * 2 + 1];
                w = (w + 1) % kRingFrames;
            }
            m_RingFill += n;
            off += static_cast<int>(n);
        }
    };
    while (!m_Abort.load()) {
        PacketItem it;
        if (!popPacket(m_AudioQ, &it, m_PktCv)) {
            break;
        }
        if (it.serial != m_Serial.load()) {
            av_packet_free(&it.pkt);
            continue;
        }
        if (it.flush || it.serial != cur) {
            avcodec_flush_buffers(m_AudioCtx);
            cur = it.serial;
            {
                std::lock_guard<std::mutex> lk(m_AudioMutex);
                m_AudioEof = false;
            }
            if (it.flush) {
                continue;
            }
        }
        int r = avcodec_send_packet(m_AudioCtx, it.pkt);
        av_packet_free(&it.pkt);
        if (r < 0 && r != AVERROR_EOF && r != AVERROR(EAGAIN)) {
            continue;
        }
        while (!m_Abort.load()) {
            r = avcodec_receive_frame(m_AudioCtx, f);
            if (r == AVERROR_EOF) {
                std::lock_guard<std::mutex> lk(m_AudioMutex);
                m_AudioEof = true;
                avcodec_flush_buffers(m_AudioCtx);
                break;
            }
            if (r < 0) {
                break;
            }
            // 輸入格式變了（或第一次）就重建 swr
            if (m_Swr == nullptr || f->format != swrInFmt || f->sample_rate != swrInRate ||
                av_channel_layout_compare(&f->ch_layout, &swrIn) != 0) {
                swr_free(&m_Swr);
                AVChannelLayout out = AV_CHANNEL_LAYOUT_STEREO;
                av_channel_layout_uninit(&swrIn);
                av_channel_layout_copy(&swrIn, &f->ch_layout);
                if (swrIn.order == AV_CHANNEL_ORDER_UNSPEC) {
                    av_channel_layout_default(&swrIn, swrIn.nb_channels > 0 ? swrIn.nb_channels : 2);
                }
                swrInFmt = f->format;
                swrInRate = f->sample_rate;
                if (swr_alloc_set_opts2(&m_Swr, &out, AV_SAMPLE_FMT_FLT, kOutRate, &swrIn, static_cast<AVSampleFormat>(f->format),
                                        f->sample_rate, 0, nullptr) < 0 ||
                    swr_init(m_Swr) < 0) {
                    swr_free(&m_Swr);
                    av_frame_unref(f);
                    continue;
                }
            }
            const int outMax = swr_get_out_samples(m_Swr, f->nb_samples) + 32;
            buf.resize(static_cast<size_t>(outMax) * 2);
            uint8_t* outPtr = reinterpret_cast<uint8_t*>(buf.data());
            const int got = swr_convert(m_Swr, &outPtr, outMax, const_cast<const uint8_t**>(f->extended_data), f->nb_samples);
            int64_t ts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
            const double pts = ts != AV_NOPTS_VALUE ? ts * m_AudioTb : 0.0;
            av_frame_unref(f);
            if (got <= 0) {
                continue;
            }
            // 精準 seek：目標之前的樣本丟掉
            int skip = 0;
            {
                std::lock_guard<std::mutex> lk(m_AudioMutex);
                if (m_AudioDropSerial == cur && pts < m_AudioDropBefore) {
                    skip = std::min(got, static_cast<int>((m_AudioDropBefore - pts) * kOutRate));
                }
            }
            if (skip < got) {
                writeRing(buf.data() + static_cast<size_t>(skip) * 2, got - skip, pts + skip / static_cast<double>(kOutRate));
            }
        }
    }
    av_channel_layout_uninit(&swrIn);
    av_frame_free(&f);
}

void MediaPipeline::sdlAudioCallback(void* userdata, uint8_t* stream, int len)
{
    static_cast<MediaPipeline*>(userdata)->audioCallback(reinterpret_cast<float*>(stream), len / static_cast<int>(sizeof(float) * 2));
}

void MediaPipeline::audioCallback(float* out, int samples)
{
    const uint64_t now = steadyNowNs();
    // 這一批的第一個樣本大約什麼時候被聽到：裝置裡還有一批在放（SDL 的雙緩衝）
    const uint64_t latencyNs = static_cast<uint64_t>(m_DevBufferFrames) * 1000000000ull / kOutRate * 2;
    const float vol = m_Volume.load(std::memory_order_relaxed);
    const int serial = m_Serial.load();
    bool anchor = false, stopClock = false;
    double anchorPts = 0.0;
    int n = 0;
    {
        std::lock_guard<std::mutex> lk(m_AudioMutex);
        const bool play = !m_Paused.load() && m_StartGate && m_RingSerial == serial;
        if (play) {
            n = static_cast<int>(std::min(m_RingFill, static_cast<size_t>(samples)));
            anchorPts = m_RingReadPts;
            for (int i = 0; i < n; i++) {
                out[i * 2] = m_Ring[m_RingRead * 2] * vol;
                out[i * 2 + 1] = m_Ring[m_RingRead * 2 + 1] * vol;
                m_RingRead = (m_RingRead + 1) % kRingFrames;
            }
            m_RingFill -= static_cast<size_t>(n);
            m_RingReadPts += n / static_cast<double>(kOutRate);
            if (n > 0) {
                anchor = true;
            }
            else if (!m_AudioEof) {
                stopClock = true;  // 沒有聲音可放（網路卡住）：時鐘停住，影像跟著等
            }
            if (n < samples && !m_AudioEof) {
                std::lock_guard<std::mutex> sl(m_StatsMutex);
                m_Stats.audioUnderruns++;
            }
        }
    }
    for (int i = n; i < samples; i++) {
        out[i * 2] = 0.0f;
        out[i * 2 + 1] = 0.0f;
    }
    if (n > 0) {
        m_AudioSpaceCv.notify_one();
    }
    if (anchor || stopClock) {
        std::lock_guard<std::mutex> lk(m_ClockMutex);
        if (anchor && m_Paused.load()) {
            // 這一批是在暫停之前拿的：不要把 setPaused() 剛停住的時鐘又打開（下一次回呼就會輸出靜音）
        }
        else if (anchor) {
            // 回呼的時刻有抖動（音訊伺服器一次拿一批），每次都硬對齊的話時鐘會前後跳幾毫秒、影像跟著偶爾多一格或少一格。
            // 照「量到的」與「時鐘目前推算的」差多少慢慢修（每次 5%）；差超過 kClockResetSec（開頭、seek、斷音之後）才直接對齊
            const uint64_t at = now + latencyNs;
            const bool same = m_ClockRunning && m_ClockSerial == serial;
            const double predicted =
                same ? m_AnchorMedia + static_cast<double>(static_cast<int64_t>(at - m_AnchorSteadyNs)) / 1e9 : 0.0;
            const double err = anchorPts - predicted;
            m_AnchorMedia = !same || std::fabs(err) > kClockResetSec ? anchorPts : predicted + err * kClockGain;
            m_AnchorSteadyNs = at;
            m_ClockRunning = true;
            m_ClockSerial = serial;
            m_ClockErrMaxMs = std::max(m_ClockErrMaxMs, same ? static_cast<float>(std::fabs(err) * 1000.0) : 0.0f);
        }
        else if (m_ClockRunning && m_ClockSerial == serial) {
            m_PausedAt = m_AnchorMedia + static_cast<double>(static_cast<int64_t>(now - m_AnchorSteadyNs)) / 1e9;
            m_ClockRunning = false;
        }
    }
}

void MediaPipeline::openStartGateIfReady()
{
    // 開頭與 seek 之後：第一張影像解出來（或影像已經結束）才讓聲音開始、時鐘開始走
    const int serial = m_Serial.load();
    bool ready = false;
    bool haveFrame = false;
    double firstPts = 0.0;
    {
        std::lock_guard<std::mutex> lk(m_FrameMutex);
        for (const FrameItem& f : m_Frames) {
            if (f.serial == serial) {
                haveFrame = true;
                firstPts = f.pts;
                break;
            }
        }
        ready = haveFrame || m_VideoEof;
    }
    if (!ready) {
        return;
    }
    bool opened = false;
    {
        // 上面查的時候之後可能又 seek 了：序號不同就不開（新的序號由它自己的第一張來開）
        std::lock_guard<std::mutex> lk(m_AudioMutex);
        if (!m_StartGate && m_Serial.load() == serial) {
            m_StartGate = true;
            opened = true;
        }
    }
    if (opened && !m_Info.hasAudio && !m_Paused.load()) {
        // 沒有聲音的片：時鐘從現在起算，第一張 50 ms 之後出現（沒有幀、影像已經結束時從 seek 的目標起算）
        std::lock_guard<std::mutex> lk(m_ClockMutex);
        if (m_Serial.load() == serial) {
            m_AnchorSteadyNs = steadyNowNs() + 50000000ull;
            m_AnchorMedia = haveFrame ? firstPts : m_PausedAt;
            m_ClockRunning = true;
            m_ClockSerial = serial;
        }
    }
}

// ── 呈現端 ──

bool MediaPipeline::peekVideo(double* pts, int* serial, bool* haveNext, double* nextPts)
{
    std::lock_guard<std::mutex> lk(m_FrameMutex);
    const int cur = m_Serial.load();
    while (!m_Frames.empty() && m_Frames.front().serial != cur) {
        av_frame_free(&m_Frames.front().frame);
        m_Frames.pop_front();
        m_FrameSpaceCv.notify_all();
    }
    if (m_Frames.empty()) {
        return false;
    }
    *pts = m_Frames.front().pts;
    *serial = m_Frames.front().serial;
    *haveNext = m_Frames.size() > 1 && m_Frames[1].serial == cur;
    *nextPts = *haveNext ? m_Frames[1].pts : 0.0;
    return true;
}

AVFrame* MediaPipeline::takeVideo()
{
    AVFrame* f = nullptr;
    {
        std::lock_guard<std::mutex> lk(m_FrameMutex);
        if (m_Frames.empty()) {
            return nullptr;
        }
        f = m_Frames.front().frame;
        m_Frames.pop_front();
    }
    m_FrameSpaceCv.notify_all();
    return f;
}

void MediaPipeline::dropVideo()
{
    AVFrame* f = takeVideo();
    av_frame_free(&f);
}

void MediaPipeline::waitVideo(int timeoutMs)
{
    std::unique_lock<std::mutex> lk(m_FrameMutex);
    m_FrameCv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return m_Abort.load() || !m_Frames.empty() || m_VideoEof; });
}

bool MediaPipeline::mediaToSteady(double pts, uint64_t* steadyNs) const
{
    std::lock_guard<std::mutex> lk(m_ClockMutex);
    if (!m_ClockRunning || m_ClockSerial != m_Serial.load()) {
        return false;
    }
    const double dNs = (pts - m_AnchorMedia) * 1e9 + static_cast<double>(m_Opt.avOffsetMs) * 1e6;
    *steadyNs = static_cast<uint64_t>(static_cast<int64_t>(m_AnchorSteadyNs) + static_cast<int64_t>(dNs));
    return true;
}

double MediaPipeline::position() const
{
    std::lock_guard<std::mutex> lk(m_ClockMutex);
    if (!m_ClockRunning || m_ClockSerial != m_Serial.load()) {
        return m_PausedAt;
    }
    const double p = m_AnchorMedia + static_cast<double>(static_cast<int64_t>(steadyNowNs() - m_AnchorSteadyNs)) / 1e9;
    // 播完之後時鐘還在走：不超過片長
    return std::max(0.0, m_Info.durationSec > 0.0 ? std::min(p, m_Info.durationSec) : p);
}

void MediaPipeline::rebaseVideoClock(double pts, uint64_t steadyNs)
{
    if (m_Info.hasAudio) {
        return;
    }
    std::lock_guard<std::mutex> lk(m_ClockMutex);
    m_AnchorSteadyNs = steadyNs;
    m_AnchorMedia = pts;
    m_ClockRunning = !m_Paused.load();
    m_ClockSerial = m_Serial.load();
}

// ── 控制 ──

void MediaPipeline::setPaused(bool paused)
{
    if (m_Paused.exchange(paused) == paused) {
        return;
    }
    std::lock_guard<std::mutex> lk(m_ClockMutex);
    if (paused) {
        if (m_ClockRunning && m_ClockSerial == m_Serial.load()) {
            m_PausedAt = std::max(0.0, m_AnchorMedia + static_cast<double>(static_cast<int64_t>(steadyNowNs() - m_AnchorSteadyNs)) / 1e9);
        }
        m_ClockRunning = false;
    }
    else if (!m_Info.hasAudio) {
        // 有聲音時由下一次音訊回呼重新對時；沒有聲音就從停住的那一點接著走
        m_AnchorSteadyNs = steadyNowNs();
        m_AnchorMedia = m_PausedAt;
        m_ClockRunning = true;
        m_ClockSerial = m_Serial.load();
    }
    m_FrameCv.notify_all();
}

void MediaPipeline::seek(double sec)
{
    if (!m_Info.seekable) {
        return;
    }
    sec = std::max(0.0, std::min(sec, std::max(0.0, m_Info.durationSec - 0.5)));
    const int serial = m_Serial.fetch_add(1) + 1;
    {
        std::lock_guard<std::mutex> lk(m_PktMutex);
        m_SeekReq = true;
        m_SeekTarget = sec;
        m_SeekSerial = serial;
        m_VideoQ.clear();
        m_AudioQ.clear();
    }
    {
        std::lock_guard<std::mutex> lk(m_FrameMutex);
        for (FrameItem& f : m_Frames) {
            av_frame_free(&f.frame);
        }
        m_Frames.clear();
        m_VideoEof = false;
        m_DropBefore = sec;
        m_DropSerial = serial;
    }
    {
        std::lock_guard<std::mutex> lk(m_AudioMutex);
        m_RingFill = 0;
        m_RingRead = 0;
        m_RingSerial = -1;
        m_StartGate = false;
        m_AudioEof = false;
        m_AudioDropBefore = sec;
        m_AudioDropSerial = serial;
    }
    {
        std::lock_guard<std::mutex> lk(m_ClockMutex);
        m_ClockRunning = false;
        m_PausedAt = sec;
    }
    m_DemuxCv.notify_all();
    m_PktCv.notify_all();
    m_FrameCv.notify_all();
    m_FrameSpaceCv.notify_all();
    m_AudioSpaceCv.notify_all();
}

void MediaPipeline::setVolume(float v)
{
    m_Volume.store(std::clamp(v, 0.0f, 2.0f), std::memory_order_relaxed);
}

bool MediaPipeline::finished() const
{
    {
        std::lock_guard<std::mutex> lk(m_PktMutex);
        if (!m_DemuxEof || !m_VideoQ.q.empty() || !m_AudioQ.q.empty()) {
            return false;
        }
    }
    {
        std::lock_guard<std::mutex> lk(m_FrameMutex);
        if (!m_VideoEof || !m_Frames.empty()) {
            return false;
        }
    }
    if (m_Info.hasAudio) {
        std::lock_guard<std::mutex> lk(m_AudioMutex);
        if (!m_AudioEof || m_RingFill > 0) {
            return false;
        }
    }
    return true;
}

std::string MediaPipeline::fatalError() const
{
    std::lock_guard<std::mutex> lk(m_StatsMutex);
    return m_Fatal;
}

MediaPipeline::Stats MediaPipeline::takeStats()
{
    Stats s;
    {
        std::lock_guard<std::mutex> lk(m_StatsMutex);
        s = m_Stats;
        m_Stats = Stats();
    }
    {
        std::lock_guard<std::mutex> lk(m_PktMutex);
        s.bufferedSec = m_Info.hasAudio ? std::min(m_VideoQ.durationSec, m_AudioQ.durationSec) : m_VideoQ.durationSec;
    }
    {
        std::lock_guard<std::mutex> lk(m_FrameMutex);
        s.frameQueue = static_cast<int>(m_Frames.size());
    }
    {
        std::lock_guard<std::mutex> lk(m_ClockMutex);
        s.clockErrMaxMs = m_ClockErrMaxMs;
        m_ClockErrMaxMs = 0.0f;
    }
    return s;
}

std::string MediaPipeline::describe() const
{
    char buf[512];
    std::snprintf(buf, sizeof(buf),
                  "container=%s video=%s%s%s %dbit %dx%d aspect=%.3f fps=%.3f decoder=%s%s audio=%s duration=%.1f s seekable=%d",
                  m_Info.container.c_str(), m_Info.videoCodec.c_str(), m_Info.videoProfile.empty() ? "" : "/",
                  m_Info.videoProfile.c_str(), m_Info.videoBits, m_Info.width, m_Info.height,
                  static_cast<double>(m_Info.frameAspect), m_Info.fps, m_Info.decoder.c_str(), m_VideoHw ? "(hw)" : "(sw)",
                  m_Info.hasAudio ? m_Info.audioCodec.c_str() : "none", m_Info.durationSec, m_Info.seekable ? 1 : 0);
    return buf;
}

}  // namespace player
