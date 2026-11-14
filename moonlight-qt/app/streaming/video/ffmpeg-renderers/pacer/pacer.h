#pragma once

#include <functional>
#include <atomic>
#include <cstdint>
#include "../../decoder.h"
#include "../renderer.h"

#include <QQueue>
#include <QMutex>
#include <QWaitCondition>

// The maximum number of frames pacer will ever hold is:
// - 2 frames in the pacing queue  [reduced from 3: saves ~1 frame display latency]
// - 1 frame removed from the render queue in the process of rendering
// - 1 frame for deferred free
// §REVIEW-1104（使用者決策 B）：慢渲染時實際最多 6 張在外面＝pacing queue 2＋render queue 2＋正在畫 1＋deferred 1。
// 原本寫 4，V4L2 m2m 的 capture buffer 依它配置，會少給兩張讓解碼器等不到空 buffer
#define PACER_MAX_OUTSTANDING_FRAMES (2 * 2 + 2)

// §SF-FREEZE：render 執行緒目前在做什麼、從何時開始（ffmpeg.cpp 的凍結 watchdog 讀取）。
// 只是診斷用的 relaxed atomic，不參與任何同步。
namespace FreezeDiag {
enum RenderPhase : int {
    RP_IDLE = 0,
    RP_WAIT_TO_RENDER = 1,   // renderer->waitToRender()（PlVk：swap_buffers／start_frame）
    RP_WAIT_QUEUE = 2,       // 等 render queue 有幀
    RP_RENDER = 3,           // renderer->renderFrame()
};
void setRenderPhase(int phase);
int renderPhase();
uint64_t renderPhaseSinceUs();
int renderQueueDepth();
const char* renderPhaseName(int phase);
}

class IVsyncSource {
public:
    virtual ~IVsyncSource() {}
    virtual bool initialize(SDL_Window* window, int displayFps) = 0;

    // Asynchronous sources produce callbacks on their own, while synchronous
    // sources require calls to waitForVsync().
    virtual bool isAsync() = 0;

    virtual void waitForVsync() {
        // Synchronous sources must implement waitForVsync()!
        SDL_assert(false);
    }
};

class Pacer
{
public:
    Pacer(IFFmpegRenderer* renderer, PVIDEO_STATS videoStats);

    // §VIPLE-2D-PERF：每幀交給 renderer 前後的時間（LiGetMicroseconds）與幀本身；在 render thread 呼叫，不可阻塞
    std::function<void(uint64_t beforeUs, uint64_t afterUs, const AVFrame* frame)> onRendered;

    ~Pacer();

    void submitFrame(AVFrame* frame);

    bool initialize(SDL_Window* window, int maxVideoFps, bool enablePacing);

    void signalVsync();

    void renderOnMainThread();

private:
    static int vsyncThread(void* context);

    static int renderThread(void* context);

    void handleVsync(int timeUntilNextVsyncUs);  // §PERF-1113：單位改成微秒（原本整數毫秒，144／240 Hz 會少算將近 1 ms）

    void enqueueFrameForRenderingAndUnlock(AVFrame* frame);

    void renderFrame(AVFrame* frame);

    void dropFrameForEnqueue(QQueue<AVFrame*>& queue);

    QQueue<AVFrame*> m_RenderQueue;
    QQueue<AVFrame*> m_PacingQueue;
    QQueue<int> m_PacingQueueHistory;
    QQueue<int> m_RenderQueueHistory;
    QMutex m_FrameQueueLock;
    QWaitCondition m_RenderQueueNotEmpty;
    QWaitCondition m_PacingQueueNotEmpty;
    QWaitCondition m_VsyncSignalled;
    SDL_Thread* m_RenderThread;
    SDL_Thread* m_VsyncThread;
    AVFrame* m_DeferredFreeFrame;
    bool m_Stopping;

    IVsyncSource* m_VsyncSource;
    IFFmpegRenderer* m_VsyncRenderer;
    int m_MaxVideoFps;
    int m_DisplayFps;
    PVIDEO_STATS m_VideoStats;
    int m_RendererAttributes;

    // PACER-DIAG: split of the queue time (see pacer.cpp); samples are touched by the render thread only
    static constexpr uint32_t kDiagMax = 4096;
    uint32_t m_DiagPaceUs[kDiagMax];
    uint32_t m_DiagRendUs[kDiagMax];
    uint32_t m_DiagN = 0;
    uint64_t m_DiagStartUs = 0;
    std::atomic<uint32_t> m_DiagVsEmpty{0}, m_DiagVsOne{0}, m_DiagVsMore{0}, m_DiagVsLate{0};
};
