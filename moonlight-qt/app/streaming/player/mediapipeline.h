// VipleStream 2.0 §VR-PLAYER — 影片的讀取、解碼、聲音輸出與影音時鐘（不依賴 Qt／OpenXR）。
//
// 一個 MediaPipeline 播一支影片（本機檔案或網址；V4 起可以另給一條音訊網址，影像、音訊分開抓）：
//   - demux 執行緒：libavformat 讀封包，分到影像、音訊兩個佇列（各自有上限；滿了就等）。seek 也在這條做。
//   - 影像解碼執行緒：Linux 上 H.264／HEVC 8-bit／VP9 先用 <codec>_v4l2m2m（DRM PRIME 輸出，頭盔的硬體解碼器），
//     10-bit、AV1、其他編碼、或硬體開不起來就軟解。解出來的幀放進幀佇列（最多 kMaxFrames 張），由呼叫端（呈現端）取走。
//   - 音訊解碼執行緒：解碼後經 libswresample 轉成 48 kHz 立體聲 float，放進環狀緩衝；SDL 的音訊回呼從那裡拿。
//   - 影音時鐘：有聲音時以聲音為準——回呼每次拿資料時記下「這一批的第一個樣本何時會被聽到、它是影片的第幾秒」；
//     沒有聲音時以 steady 時鐘為準。開頭與每次 seek 之後，聲音等到第一張影像解出來才開始（不然開頭的影像全都晚了）。
//
// 序號（serial）：每次 seek 加一。佇列裡的封包與幀都帶序號，舊序號的東西一律丟掉（做法同 ffplay）。
//
// 執行緒：open()／close() 與控制（setPaused／seek／setVolume）在呼叫端任何一條執行緒；peek／take 是呈現端一條執行緒。
// open() 會阻塞到串流資訊讀完（網址要連線），可以在背景執行緒呼叫；close() 會中斷進行中的網路讀取。

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "playerformat.h"

struct AVFormatContext;
struct AVCodecContext;
struct AVPacket;
struct AVFrame;
struct SwrContext;
struct AVBSFContext;

namespace player {

struct MediaOptions {
    std::string url;          // 本機路徑或網址
    std::string audioUrl;     // 另一條音訊（空＝影像那一條裡的音軌）
    double startSec = 0.0;    // 從第幾秒開始
    bool hwDecode = true;     // 允許硬體解碼（v4l2m2m）
    bool enableAudio = true;
    float volume = 1.0f;      // 0..2
    int avOffsetMs = 0;       // 影像相對聲音延後多少（正＝影像晚一點出現）；補裝置延遲用
    std::string userAgent;
    std::string httpHeaders;  // "Key: value\r\n…"
    // V3：host 的媒體端點用配對憑證（client 端 TLS 憑證與私鑰檔）
    std::string tlsCertFile;
    std::string tlsKeyFile;
};

struct MediaInfo {
    double durationSec = 0.0;  // 0＝未知（直播）
    int width = 0;
    int height = 0;
    float frameAspect = 16.0f / 9.0f;  // 含 SAR 的整張顯示比例
    double fps = 0.0;
    bool hasVideo = false;
    bool hasAudio = false;
    bool seekable = true;
    std::string container;
    std::string videoCodec;  // h264、hevc、vp9…
    std::string videoProfile;
    int videoBits = 8;
    std::string audioCodec;
    std::string decoder;     // 實際用的影像解碼器名稱（hevc_v4l2m2m、hevc、libdav1d…）
    std::string title;
    playerfmt::Hints hints;  // 容器裡的 360°／立體中繼資料
};

class MediaPipeline
{
public:
    // 幀佇列上限：v4l2m2m 的 capture buffer 要容得下「參考幀＋這幾張＋呈現端與 XrVideo 手上的」（見 .cpp 的 kCaptureBuffers）
    static constexpr int kMaxFrames = 4;

    struct Stats {
        uint64_t demuxBytes = 0;     // 讀進來的位元組（算位元率）
        uint32_t videoPackets = 0;
        uint32_t audioPackets = 0;
        uint32_t decoded = 0;        // 解出的影像幀
        uint32_t decodeErrors = 0;
        uint32_t seekDropped = 0;    // seek 之後在目標時間之前、丟掉的幀
        uint32_t audioUnderruns = 0; // 播放中回呼拿不到足夠的聲音
        uint32_t decodeUsMax = 0;    // 單張解碼（送封包到取出幀）的最長時間
        double bufferedSec = 0.0;    // 讀取領先播放位置多少秒（取樣時）
        int frameQueue = 0;          // 取樣時的幀佇列長度
        float clockErrMaxMs = 0.0f;  // 聲音回呼量到的時刻和平滑後的時鐘差最多多少（大＝音訊回呼抖動大）
    };

    MediaPipeline();
    ~MediaPipeline();
    MediaPipeline(const MediaPipeline&) = delete;
    MediaPipeline& operator=(const MediaPipeline&) = delete;

    // 開檔／連線、找串流、開解碼器與音訊裝置、起執行緒。失敗回 false、*error 是英文說明
    bool open(const MediaOptions& options, std::string* error);
    void close();
    // close() 之前可以從任何執行緒呼叫：中斷進行中的網路讀取（open() 會盡快失敗返回）
    void abort();
    const MediaInfo& info() const { return m_Info; }

    // ── 呈現端（一條執行緒）──
    // 看佇列最前面那一張：pts（秒）、序號；有下一張就一併給 pts。沒有回 false
    bool peekVideo(double* pts, int* serial, bool* haveNext, double* nextPts);
    // 取走最前面那一張（呼叫端負責 av_frame_free）；沒有回 nullptr
    AVFrame* takeVideo();
    void dropVideo();
    // 等到有幀、或狀態改變（seek／暫停／結束），最多 timeoutMs
    void waitVideo(int timeoutMs);
    // 影片時間 pts 會在什麼時刻（steady ns）被聽到／該被看見；時鐘沒在走（暫停、seek 後還沒開始、還沒開播）回 false
    bool mediaToSteady(double pts, uint64_t* steadyNs) const;
    // 目前播到第幾秒（暫停時是停住的那一點）
    double position() const;
    int serial() const { return m_Serial.load(std::memory_order_acquire); }
    // 沒有聲音的片：呈現端發現自己落後太多（例如網路卡住後）就把時鐘對齊到這一張
    void rebaseVideoClock(double pts, uint64_t steadyNs);

    // ── 控制 ──
    void setPaused(bool paused);
    bool paused() const { return m_Paused.load(std::memory_order_acquire); }
    void seek(double sec);
    void setVolume(float v);
    float volume() const { return m_Volume.load(std::memory_order_relaxed); }

    // 讀到結尾、解碼也做完、幀與聲音都送完
    bool finished() const;
    // 讀取／解碼發生無法繼續的錯誤（空＝沒有）
    std::string fatalError() const;

    Stats takeStats();

    // 給 log：一行說明（容器、編碼、解碼器、尺寸、幀率、聲音）
    std::string describe() const;

private:
    struct Input;
    struct PacketItem {
        AVPacket* pkt = nullptr;  // nullptr＝這一條結束（EOF）
        int serial = 0;
        bool flush = false;       // seek 之後的第一個：解碼器要先 flush
    };
    struct PacketQueue {
        std::deque<PacketItem> q;
        int64_t bytes = 0;
        double durationSec = 0.0;
        void clear();
    };
    struct FrameItem {
        AVFrame* frame = nullptr;
        double pts = 0.0;
        int serial = 0;
    };

    bool openInput(Input& in, const std::string& url, std::string* error);
    bool openVideoDecoder(bool allowHw, std::string* error);
    bool openAudioDecoder(std::string* error);
    bool openAudioDevice(std::string* error);
    void demuxThread();
    void videoThread();
    void audioThread();
    static void sdlAudioCallback(void* userdata, uint8_t* stream, int len);
    void audioCallback(float* out, int samples);
    void pushPacket(PacketQueue& pq, AVPacket* pkt, int serial, double durSec);
    bool popPacket(PacketQueue& pq, PacketItem* out, std::condition_variable& cv);
    void doSeek(double sec, int serial);
    void openStartGateIfReady();
    static int interruptCb(void* opaque);

    MediaOptions m_Opt;
    MediaInfo m_Info;
    std::vector<Input*> m_Inputs;  // [0] 影像（和音訊，如果同一條）；[1] 另一條音訊
    int m_VideoInput = -1, m_VideoStream = -1;
    int m_AudioInput = -1, m_AudioStream = -1;
    AVCodecContext* m_VideoCtx = nullptr;
    AVBSFContext* m_VideoBsf = nullptr;  // VP9＋v4l2m2m：拆超級幀（vp9_superframe_split）
    AVCodecContext* m_AudioCtx = nullptr;
    SwrContext* m_Swr = nullptr;
    bool m_VideoHw = false;
    double m_VideoTb = 0.0;   // 影像串流的 time_base（秒）
    double m_AudioTb = 0.0;

    std::atomic<bool> m_Abort{false};
    std::atomic<int> m_Serial{0};
    std::atomic<bool> m_Paused{false};
    std::atomic<float> m_Volume{1.0f};

    // 封包佇列
    mutable std::mutex m_PktMutex;
    std::condition_variable m_PktCv;       // 有封包（解碼端等）
    std::condition_variable m_DemuxCv;     // 有空間或有 seek 要求（demux 端等）
    PacketQueue m_VideoQ, m_AudioQ;
    bool m_SeekReq = false;
    double m_SeekTarget = 0.0;
    int m_SeekSerial = 0;
    bool m_DemuxEof = false;

    // 幀佇列
    mutable std::mutex m_FrameMutex;
    std::condition_variable m_FrameCv;     // 有幀（呈現端等）
    std::condition_variable m_FrameSpaceCv;// 有空間（解碼端等）
    std::deque<FrameItem> m_Frames;
    bool m_VideoEof = false;               // 這個序號的影像已經全部解完
    double m_DropBefore = -1.0;            // seek 之後，比這個早的幀丟掉（精準 seek）
    int m_DropSerial = -1;

    // 聲音：環狀緩衝（float 交錯立體聲 48 kHz）與時鐘
    mutable std::mutex m_AudioMutex;
    std::condition_variable m_AudioSpaceCv;
    std::vector<float> m_Ring;             // 容量 kRingFrames*2
    size_t m_RingRead = 0, m_RingFill = 0; // 以「樣本幀」（左右一組）計
    double m_RingReadPts = 0.0;            // 環狀緩衝裡最舊那一組樣本的影片時間
    int m_RingSerial = -1;                 // 環狀緩衝裡的資料屬於哪個序號
    bool m_AudioEof = false;
    double m_AudioDropBefore = -1.0;
    int m_AudioDropSerial = -1;
    uint32_t m_AudioDev = 0;
    int m_DevBufferFrames = 1024;
    bool m_StartGate = false;              // 開頭／seek 之後：第一張影像解出來之前聲音先不放

    // 時鐘：anchorSteady 時刻＝影片時間 anchorMedia；clockRunning＝false 時不走
    mutable std::mutex m_ClockMutex;
    bool m_ClockRunning = false;
    uint64_t m_AnchorSteadyNs = 0;
    double m_AnchorMedia = 0.0;
    int m_ClockSerial = -1;
    double m_PausedAt = 0.0;
    float m_ClockErrMaxMs = 0.0f;

    mutable std::mutex m_StatsMutex;
    Stats m_Stats;
    std::string m_Fatal;

    std::thread m_DemuxThread, m_VideoThread, m_AudioThread;
    bool m_Opened = false;
};

uint64_t steadyNowNs();

}  // namespace player
