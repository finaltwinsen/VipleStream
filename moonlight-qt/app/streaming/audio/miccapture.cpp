// §VR-MIC（2026-11-05）：頭盔麥克風錄音＋Opus 編碼（介面見 miccapture.h）
#include "miccapture.h"

#include <SDL.h>
#include <opus.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

// 取樣率 48 kHz、單聲道
constexpr int SAMPLE_RATE = 48000;
constexpr int CHANNELS = 1;

// 每幀 20 ms 對應的樣本數（48000 Hz * 0.02 s = 960 樣本）
constexpr int SAMPLES_PER_FRAME = 960;

// 環狀緩衝區容量約 200 ms（48000 Hz * 0.2 s = 9600 個樣本，即 10 幀）
constexpr int RING_BUFFER_CAPACITY = 9600;

// Opus 壓縮後封包的最大位元組數（上限設 240 位元組，加上 2 位元組序號後整則不超過控制通道限制）
constexpr int MAX_OPUS_PAYLOAD = 240;

class MicCaptureImpl : public MicCapture {
public:
    explicit MicCaptureImpl(SendFn send)
        : m_send(std::move(send)),
          m_audioDevice(0),
          m_encoder(nullptr),
          m_ringBuffer(RING_BUFFER_CAPACITY, 0),
          m_bufferHead(0),
          m_bufferCount(0),
          m_muted(false),
          m_packetsSent(0),
          m_overruns(0),
          m_stop(false),
          m_lastStatsTime(std::chrono::steady_clock::now())
    {
    }

    ~MicCaptureImpl() override {
        // 先停止音訊錄製設備並關閉，確保音訊 callback 不會再被觸發
        if (m_audioDevice != 0) {
            SDL_PauseAudioDevice(m_audioDevice, 1);
            SDL_CloseAudioDevice(m_audioDevice);
            m_audioDevice = 0;
        }

        // 通知編碼執行緒退出
        m_stop.store(true, std::memory_order_release);
        m_cv.notify_all();

        // 等待編碼執行緒結束
        if (m_encoderThread.joinable()) {
            m_encoderThread.join();
        }

        // 依需求規定：在關閉 SDL 設備與停止執行緒後，再銷毀 Opus 編碼器
        if (m_encoder != nullptr) {
            opus_encoder_destroy(m_encoder);
            m_encoder = nullptr;
        }

        // init() 加的音訊子系統參照計數要還回去（播放端另外有自己的參照）
        if (m_sdlAudioInit) {
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
        }
    }

    bool init() {
        // 初始化 SDL 音訊子系統（若已初始化過則增加參照計數）
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VR-MIC] SDL_InitSubSystem(SDL_INIT_AUDIO) failed: %s",
                         SDL_GetError());
            return false;
        }
        m_sdlAudioInit = true;

        // 設定期望的錄音格式：48 kHz、單聲道、AUDIO_S16SYS、samples 480
        SDL_AudioSpec want, have;
        SDL_zero(want);
        want.freq = SAMPLE_RATE;
        want.format = AUDIO_S16SYS;
        want.channels = CHANNELS;
        want.samples = 480;
        want.callback = audioCaptureCallback;
        want.userdata = this;

        // allowed_changes 傳入 0，若硬體不支援 48 kHz 或單聲道，允許 SDL 自行在內部重取樣與轉換聲道
        m_audioDevice = SDL_OpenAudioDevice(nullptr, 1 /* iscapture */, &want, &have, 0);
        if (m_audioDevice == 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VR-MIC] Failed to open capture device: %s",
                         SDL_GetError());
            return false;
        }

        // 單次記錄實際取得的設備格式資訊
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VR-MIC] Opened capture device %u: obtained freq=%d, channels=%d, format=0x%x, samples=%u (SDL converting to 48000 Hz, 1 ch, S16SYS)",
                    m_audioDevice, have.freq, have.channels, have.format, have.samples);

        // 建立 Opus 編碼器：48000 Hz, 1 channel, OPUS_APPLICATION_VOIP
        int opusErr = OPUS_OK;
        m_encoder = opus_encoder_create(SAMPLE_RATE, CHANNELS, OPUS_APPLICATION_VOIP, &opusErr);
        if (opusErr != OPUS_OK || m_encoder == nullptr) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-VR-MIC] Failed to create Opus encoder: %s",
                         opus_strerror(opusErr));
            return false;
        }

        // 設定 Opus 編碼參數
        opus_encoder_ctl(m_encoder, OPUS_SET_BITRATE(24000));
        opus_encoder_ctl(m_encoder, OPUS_SET_INBAND_FEC(1));
        opus_encoder_ctl(m_encoder, OPUS_SET_PACKET_LOSS_PERC(10));
        opus_encoder_ctl(m_encoder, OPUS_SET_COMPLEXITY(5));
        opus_encoder_ctl(m_encoder, OPUS_SET_DTX(0));

        // 啟動編碼執行緒
        m_lastStatsTime = std::chrono::steady_clock::now();
        m_encoderThread = std::thread(&MicCaptureImpl::encoderThreadLoop, this);

        // 啟動 SDL 音訊錄製設備
        SDL_PauseAudioDevice(m_audioDevice, 0);

        return true;
    }

    void setMuted(bool muted) override {
        m_muted.store(muted, std::memory_order_relaxed);
    }

    uint32_t packetsSent() const override {
        return m_packetsSent.load(std::memory_order_relaxed);
    }

    uint32_t overruns() const override {
        return m_overruns.load(std::memory_order_relaxed);
    }

private:
    static void SDLCALL audioCaptureCallback(void* userdata, Uint8* stream, int len) {
        auto* self = static_cast<MicCaptureImpl*>(userdata);
        if (self != nullptr) {
            self->onAudioCapture(stream, len);
        }
    }

    void onAudioCapture(const Uint8* stream, int len) {
        // 因 format 設定為 AUDIO_S16SYS，每個樣本大小為 2 位元組
        int samplesReceived = len / static_cast<int>(sizeof(int16_t));
        if (samplesReceived <= 0) {
            return;
        }

        const auto* inSamples = reinterpret_cast<const int16_t*>(stream);

        {
            std::lock_guard<std::mutex> lock(m_mutex);

            // 若單次接收的樣本數直接超出或等於緩衝區容量
            if (samplesReceived >= RING_BUFFER_CAPACITY) {
                // 丟棄全部舊樣本，只保留最新的 RING_BUFFER_CAPACITY 個樣本
                const int16_t* latestSamples = inSamples + (samplesReceived - RING_BUFFER_CAPACITY);
                std::memcpy(m_ringBuffer.data(), latestSamples, RING_BUFFER_CAPACITY * sizeof(int16_t));
                m_bufferHead = 0;
                m_bufferCount = RING_BUFFER_CAPACITY;
                m_overruns.fetch_add(1, std::memory_order_relaxed);
            } else {
                // 檢查加入新樣本後是否溢位；若溢位則丟棄最舊的樣本
                if (m_bufferCount + samplesReceived > RING_BUFFER_CAPACITY) {
                    int dropCount = (m_bufferCount + samplesReceived) - RING_BUFFER_CAPACITY;
                    m_bufferHead = (m_bufferHead + dropCount) % RING_BUFFER_CAPACITY;
                    m_bufferCount -= dropCount;
                    m_overruns.fetch_add(1, std::memory_order_relaxed);
                }

                // 將新樣本寫入環狀緩衝區（處理跨越尾端環繞的情形）
                int writePos = (m_bufferHead + m_bufferCount) % RING_BUFFER_CAPACITY;
                int firstChunk = std::min(samplesReceived, RING_BUFFER_CAPACITY - writePos);
                std::memcpy(&m_ringBuffer[writePos], inSamples, firstChunk * sizeof(int16_t));

                int secondChunk = samplesReceived - firstChunk;
                if (secondChunk > 0) {
                    std::memcpy(&m_ringBuffer[0], inSamples + firstChunk, secondChunk * sizeof(int16_t));
                }

                m_bufferCount += samplesReceived;
            }
        }

        // 喚醒編碼執行緒（不可在 callback 內部執行編碼或呼叫 send）
        m_cv.notify_one();
    }

    void encoderThreadLoop() {
        uint16_t seq = 0;
        std::vector<int16_t> frame(SAMPLES_PER_FRAME);
        uint8_t opusOut[MAX_OPUS_PAYLOAD];
        uint8_t payload[2 + MAX_OPUS_PAYLOAD];

        while (true) {
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                while (!m_stop.load(std::memory_order_acquire) && m_bufferCount < SAMPLES_PER_FRAME) {
                    m_cv.wait_for(lock, std::chrono::milliseconds(100));
                    logStatsIfNeeded();
                }

                if (m_stop.load(std::memory_order_acquire)) {
                    break;
                }

                // 自環狀緩衝區取出 960 個樣本（處理環繞情形）
                int firstChunk = std::min(SAMPLES_PER_FRAME, RING_BUFFER_CAPACITY - m_bufferHead);
                std::memcpy(frame.data(), &m_ringBuffer[m_bufferHead], firstChunk * sizeof(int16_t));

                int secondChunk = SAMPLES_PER_FRAME - firstChunk;
                if (secondChunk > 0) {
                    std::memcpy(frame.data() + firstChunk, &m_ringBuffer[0], secondChunk * sizeof(int16_t));
                }

                m_bufferHead = (m_bufferHead + SAMPLES_PER_FRAME) % RING_BUFFER_CAPACITY;
                m_bufferCount -= SAMPLES_PER_FRAME;
            }

            // 若處於靜音狀態，將樣本填零，由 Opus 壓縮成微小封包以維持接收端時序
            if (m_muted.load(std::memory_order_relaxed)) {
                std::memset(frame.data(), 0, SAMPLES_PER_FRAME * sizeof(int16_t));
            }

            // 進行 Opus 音訊編碼
            opus_int32 encodedBytes = opus_encode(
                m_encoder,
                frame.data(),
                SAMPLES_PER_FRAME,
                opusOut,
                MAX_OPUS_PAYLOAD
            );

            if (encodedBytes > 0) {
                // 組裝 payload: [seq low byte, seq high byte] + opus bytes
                payload[0] = static_cast<uint8_t>(seq & 0xFF);
                payload[1] = static_cast<uint8_t>((seq >> 8) & 0xFF);
                std::memcpy(&payload[2], opusOut, static_cast<size_t>(encodedBytes));

                if (m_send) {
                    m_send(payload, 2 + encodedBytes);
                }

                seq++; // uint16 自然溢位繞回
                m_packetsSent.fetch_add(1, std::memory_order_relaxed);
            } else if (encodedBytes < 0) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "[VIPLE-VR-MIC] opus_encode failed: %s",
                            opus_strerror(encodedBytes));
            }

            logStatsIfNeeded();
        }
    }

    void logStatsIfNeeded() {
        auto now = std::chrono::steady_clock::now();
        if (now - m_lastStatsTime >= std::chrono::seconds(10)) {
            m_lastStatsTime = now;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-VR-MIC] 10s: sent=%u overruns=%u muted=%d",
                        m_packetsSent.load(std::memory_order_relaxed),
                        m_overruns.load(std::memory_order_relaxed),
                        m_muted.load(std::memory_order_relaxed) ? 1 : 0);
        }
    }

    SendFn m_send;
    bool m_sdlAudioInit = false;
    SDL_AudioDeviceID m_audioDevice;
    OpusEncoder* m_encoder;

    // 環狀緩衝區與執行緒同步保護
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::vector<int16_t> m_ringBuffer;
    int m_bufferHead;
    int m_bufferCount;

    // 狀態與統計變數（原子操作）
    std::atomic<bool> m_muted;
    std::atomic<uint32_t> m_packetsSent;
    std::atomic<uint32_t> m_overruns;
    std::atomic<bool> m_stop;

    std::thread m_encoderThread;
    std::chrono::steady_clock::time_point m_lastStatsTime;
};

} // namespace

std::unique_ptr<MicCapture> MicCapture::start(SendFn send) {
    if (!send) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VR-MIC] Failed to start mic capture: invalid send callback");
        return nullptr;
    }

    auto capture = std::make_unique<MicCaptureImpl>(std::move(send));
    if (!capture->init()) {
        return nullptr;
    }

    return capture;
}
