#include "pacer.h"
#include "streaming/streamutils.h"

#ifdef Q_OS_WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include "dxvsyncsource.h"
#endif

#ifdef HAS_WAYLAND
#include "waylandvsyncsource.h"
#endif

#include <SDL_syswm.h>

#include <QDeadlineTimer>

#include <algorithm>
#include <atomic>
#include <chrono>

// Limit the number of queued frames to prevent excessive memory consumption
// if the V-Sync source or renderer is blocked for a while. It's important
// that the sum of all queued frames between both pacing and rendering queues
// must not exceed the number buffer pool size to avoid running the decoder
// out of available decoding surfaces.
// [Reduced from 3→2: saves ~1 frame of display-side latency (~8-16ms at 60-120fps)]
#define MAX_QUEUED_FRAMES 2
static_assert(PACER_MAX_OUTSTANDING_FRAMES == 2 * MAX_QUEUED_FRAMES + 2,  // §REVIEW-1104：兩條佇列＋正在畫＋deferred
              "PACER_MAX_OUTSTANDING_FRAMES and MAX_QUEUED_FRAMES must agree");

// We may be woken up slightly late so don't go all the way
// up to the next V-sync since we may accidentally step into
// the next V-sync period. It also takes some amount of time
// to do the render itself, so we can't render right before
// V-sync happens.
// [Reduced from 3→2ms: 1ms closer to vsync deadline]
#define TIMER_SLACK_MS 2

namespace FreezeDiag {
static std::atomic<int> s_RenderPhase {RP_IDLE};
static std::atomic<uint64_t> s_RenderPhaseSinceUs {0};
static std::atomic<int> s_RenderQueueDepth {0};

void setRenderPhase(int phase)
{
    s_RenderPhase.store(phase, std::memory_order_relaxed);
    s_RenderPhaseSinceUs.store(LiGetMicroseconds(), std::memory_order_relaxed);
}

int renderPhase()
{
    return s_RenderPhase.load(std::memory_order_relaxed);
}

uint64_t renderPhaseSinceUs()
{
    return s_RenderPhaseSinceUs.load(std::memory_order_relaxed);
}

int renderQueueDepth()
{
    return s_RenderQueueDepth.load(std::memory_order_relaxed);
}

const char* renderPhaseName(int phase)
{
    switch (phase) {
    case RP_WAIT_TO_RENDER: return "waitToRender";
    case RP_WAIT_QUEUE: return "waitQueue";
    case RP_RENDER: return "renderFrame";
    default: return "idle";
    }
}
}

Pacer::Pacer(IFFmpegRenderer* renderer, PVIDEO_STATS videoStats) :
    m_RenderThread(nullptr),
    m_VsyncThread(nullptr),
    m_DeferredFreeFrame(nullptr),
    m_Stopping(false),
    m_VsyncSource(nullptr),
    m_VsyncRenderer(renderer),
    m_MaxVideoFps(0),
    m_DisplayFps(0),
    m_VideoStats(videoStats)
{

}

Pacer::~Pacer()
{
    m_Stopping = true;

    // Stop the V-sync thread
    if (m_VsyncThread != nullptr) {
        m_PacingQueueNotEmpty.wakeAll();
        m_VsyncSignalled.wakeAll();
        SDL_WaitThread(m_VsyncThread, nullptr);
    }

    // Stop V-sync callbacks
    delete m_VsyncSource;
    m_VsyncSource = nullptr;

    // Stop the render thread
    if (m_RenderThread != nullptr) {
        m_RenderQueueNotEmpty.wakeAll();
        SDL_WaitThread(m_RenderThread, nullptr);
    }
    else {
        // Notify the renderer that it is being destroyed soon
        // NB: This must happen on the same thread that calls renderFrame().
        m_VsyncRenderer->cleanupRenderContext();
    }

    // Delete any remaining unconsumed frames
    while (!m_RenderQueue.isEmpty()) {
        AVFrame* frame = m_RenderQueue.dequeue();
        av_frame_free(&frame);
    }
    while (!m_PacingQueue.isEmpty()) {
        AVFrame* frame = m_PacingQueue.dequeue();
        av_frame_free(&frame);
    }
    av_frame_free(&m_DeferredFreeFrame);
    FreezeDiag::s_RenderQueueDepth.store(0, std::memory_order_relaxed);
}

void Pacer::renderOnMainThread()
{
    // Ignore this call for renderers that work on a dedicated render thread
    if (m_RenderThread != nullptr) {
        return;
    }

    m_FrameQueueLock.lock();

    if (!m_RenderQueue.isEmpty()) {
        AVFrame* frame = m_RenderQueue.dequeue();
        FreezeDiag::s_RenderQueueDepth.store(m_RenderQueue.count(), std::memory_order_relaxed);
        m_FrameQueueLock.unlock();

        FreezeDiag::setRenderPhase(FreezeDiag::RP_RENDER);
        renderFrame(frame);
        FreezeDiag::setRenderPhase(FreezeDiag::RP_IDLE);
    }
    else {
        m_FrameQueueLock.unlock();
    }
}

int Pacer::vsyncThread(void *context)
{
    Pacer* me = reinterpret_cast<Pacer*>(context);

#if SDL_VERSION_ATLEAST(2, 0, 9)
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_TIME_CRITICAL);
#else
    SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH);
#endif

    bool async = me->m_VsyncSource->isAsync();
    while (!me->m_Stopping) {
        if (async) {
            // Wait for the VSync source to invoke signalVsync() or 100ms to elapse
            me->m_FrameQueueLock.lock();
            me->m_VsyncSignalled.wait(&me->m_FrameQueueLock, 100);
            me->m_FrameQueueLock.unlock();
        }
        else {
            // Let the VSync source wait in the context of our thread
            me->m_VsyncSource->waitForVsync();
        }

        if (me->m_Stopping) {
            break;
        }

        // §PERF-1113：週期改用微秒。整數毫秒在 144 Hz 得到 6 ms（實際 6.94）、240 Hz 得到 4 ms（實際 4.17），
        // 「來不及趕上這個 vblank」的區間因此比設計的 TIMER_SLACK_MS 多出將近 1 ms
        me->handleVsync(1000000 / me->m_DisplayFps);
    }

    return 0;
}

int Pacer::renderThread(void* context)
{
    Pacer* me = reinterpret_cast<Pacer*>(context);

    if (SDL_SetThreadPriority(SDL_THREAD_PRIORITY_HIGH) < 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "Unable to set render thread to high priority: %s",
                    SDL_GetError());
    }

    while (!me->m_Stopping) {
        // Wait for the renderer to be ready for the next frame
        FreezeDiag::setRenderPhase(FreezeDiag::RP_WAIT_TO_RENDER);
        me->m_VsyncRenderer->waitToRender();

        // Acquire the frame queue lock to protect the queue and
        // the not empty condition
        me->m_FrameQueueLock.lock();

        // Wait for a frame to be ready to render
        FreezeDiag::setRenderPhase(FreezeDiag::RP_WAIT_QUEUE);
        while (!me->m_Stopping && me->m_RenderQueue.isEmpty()) {
            me->m_RenderQueueNotEmpty.wait(&me->m_FrameQueueLock);
        }

        if (me->m_Stopping) {
            // Exit this thread
            me->m_FrameQueueLock.unlock();
            break;
        }

        AVFrame* frame = me->m_RenderQueue.dequeue();
        FreezeDiag::s_RenderQueueDepth.store(me->m_RenderQueue.count(), std::memory_order_relaxed);
        me->m_FrameQueueLock.unlock();

        FreezeDiag::setRenderPhase(FreezeDiag::RP_RENDER);
        me->renderFrame(frame);
    }
    FreezeDiag::setRenderPhase(FreezeDiag::RP_IDLE);

    // Notify the renderer that it is being destroyed soon
    // NB: This must happen on the same thread that calls renderFrame().
    me->m_VsyncRenderer->cleanupRenderContext();

    return 0;
}

void Pacer::enqueueFrameForRenderingAndUnlock(AVFrame *frame)
{
    frame->duration = (int64_t)LiGetMicroseconds();  // §PACER-DIAG：交到繪製佇列的時間（這個欄位沒有別人用）
    dropFrameForEnqueue(m_RenderQueue);
    m_RenderQueue.enqueue(frame);
    FreezeDiag::s_RenderQueueDepth.store(m_RenderQueue.count(), std::memory_order_relaxed);

    m_FrameQueueLock.unlock();

    if (m_RenderThread != nullptr) {
        m_RenderQueueNotEmpty.wakeOne();
    }
    else {
        SDL_Event event;

        // For main thread rendering, we'll push an event to trigger a callback
        event.type = SDL_USEREVENT;
        event.user.code = SDL_CODE_FRAME_READY;
        SDL_PushEvent(&event);
    }
}

// Called in an arbitrary thread by the IVsyncSource on V-sync
// or an event synchronized with V-sync
void Pacer::handleVsync(int timeUntilNextVsyncUs)
{
    // Make sure initialize() has been called
    SDL_assert(m_MaxVideoFps != 0);

    m_FrameQueueLock.lock();

    // If the queue length history entries are large, be strict
    // about dropping excess frames.
    int frameDropTarget = 1;

    // If we may get more frames per second than we can display, use
    // frame history to drop frames only if consistently above the
    // one queued frame mark.
    if (m_MaxVideoFps >= m_DisplayFps) {
        for (int queueHistoryEntry : std::as_const(m_PacingQueueHistory)) {
            if (queueHistoryEntry <= 1) {
                // Be lenient as long as the queue length
                // resolves before the end of frame history
                frameDropTarget = 3;
                break;
            }
        }

        // Keep a rolling 500 ms window of pacing queue history
        if (m_PacingQueueHistory.count() == m_DisplayFps / 2) {
            m_PacingQueueHistory.dequeue();
        }

        m_PacingQueueHistory.enqueue(m_PacingQueue.count());
    }

    // Catch up if we're several frames ahead
    while (m_PacingQueue.count() > frameDropTarget) {
        AVFrame* frame = m_PacingQueue.dequeue();

        // Drop the lock while we call av_frame_free()
        m_FrameQueueLock.unlock();
        m_VideoStats->pacerDroppedFrames++;
        av_frame_free(&frame);
        m_FrameQueueLock.lock();
    }

    {
        const int depth = m_PacingQueue.count();  // §PACER-DIAG
        (depth == 0 ? m_DiagVsEmpty : depth == 1 ? m_DiagVsOne : m_DiagVsMore).fetch_add(1, std::memory_order_relaxed);
    }

    if (m_PacingQueue.isEmpty()) {
        // Wait for a frame to arrive or for V-Sync to elapse. If V-Sync
        // elapses without a frame, just render the next image again.
        // §PERF-1113：等到「下一個 vblank 前 TIMER_SLACK_MS」為止，以微秒計算。slack 本身沒有改。
        const int slackUs = TIMER_SLACK_MS * 1000;
        const int waitUs = SDL_max(timeUntilNextVsyncUs, slackUs) - slackUs;
        bool gotFrame;
#ifdef Q_OS_WIN32
        // Windows 的 QWaitCondition 只有毫秒精度，而且 QDeadlineTimer 換算成毫秒時是無條件進位（會吃掉最多 1 ms 的
        // slack）。這裡自己換算成毫秒、無條件捨去——和改成微秒之前在每個更新率都相同（複核 11-13：原本寫成
        // 「讓出 0.1 ms」會讓 144 Hz 多等 1 ms，留給繪製的時間從 2.9 ms 剩 1.9 ms）
        gotFrame = m_PacingQueueNotEmpty.wait(&m_FrameQueueLock, (unsigned long)(waitUs / 1000));
#else
        gotFrame = m_PacingQueueNotEmpty.wait(&m_FrameQueueLock,
                                              QDeadlineTimer(std::chrono::microseconds(waitUs), Qt::PreciseTimer));
#endif
        if (!gotFrame) {
            m_FrameQueueLock.unlock();
            return;
        }

        m_DiagVsLate.fetch_add(1, std::memory_order_relaxed);  // §PACER-DIAG

        if (m_Stopping) {
            m_FrameQueueLock.unlock();
            return;
        }
    }

    // Place the first frame on the render queue
    enqueueFrameForRenderingAndUnlock(m_PacingQueue.dequeue());
}


bool Pacer::initialize(SDL_Window* window, int maxVideoFps, bool enablePacing)
{
    m_MaxVideoFps = maxVideoFps;
    m_DisplayFps = StreamUtils::getDisplayRefreshRate(window);
    m_RendererAttributes = m_VsyncRenderer->getRendererAttributes();

    if (enablePacing) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Frame pacing: target %d Hz with %d FPS stream",
                    m_DisplayFps, m_MaxVideoFps);

        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        if (!SDL_GetWindowWMInfo(window, &info)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "SDL_GetWindowWMInfo() failed: %s",
                         SDL_GetError());
            return false;
        }

        switch (info.subsystem) {
    #ifdef Q_OS_WIN32
        case SDL_SYSWM_WINDOWS:
            m_VsyncSource = new DxVsyncSource(this);
            break;
    #endif

    #if defined(SDL_VIDEO_DRIVER_WAYLAND) && defined(HAS_WAYLAND)
        case SDL_SYSWM_WAYLAND:
            m_VsyncSource = new WaylandVsyncSource(this);
            break;
    #endif

        default:
            // Platforms without a VsyncSource will just render frames
            // immediately like they used to.
            break;
        }

        SDL_assert(m_VsyncSource != nullptr || !(m_RendererAttributes & RENDERER_ATTRIBUTE_FORCE_PACING));

        if (m_VsyncSource != nullptr && !m_VsyncSource->initialize(window, m_DisplayFps)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Vsync source failed to initialize. Frame pacing will not be available!");
            delete m_VsyncSource;
            m_VsyncSource = nullptr;
        }
    }
    else {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "Frame pacing disabled: target %d Hz with %d FPS stream",
                    m_DisplayFps, m_MaxVideoFps);
    }

    if (m_VsyncSource != nullptr) {
        m_VsyncThread = SDL_CreateThread(Pacer::vsyncThread, "PacerVsync", this);
    }

    if (m_VsyncRenderer->isRenderThreadSupported()) {
        m_RenderThread = SDL_CreateThread(Pacer::renderThread, "PacerRender", this);
    }

    return true;
}

void Pacer::signalVsync()
{
    m_VsyncSignalled.wakeOne();
}

void Pacer::renderFrame(AVFrame* frame)
{
    // Count time spent in Pacer's queues
    uint64_t beforeRender = LiGetMicroseconds();
    m_VideoStats->totalPacerTimeUs += (beforeRender - (uint64_t)frame->pkt_dts);

    // §PACER-DIAG（2026-11-11）：把「佇列」拆成兩段——等垂直同步交接（pace）、交接後等繪製執行緒（rend）——
    // 並記交接當下 pacing 佇列裡還剩幾張、這一張是不是「垂直同步到了才來」的（late＝佇列是空的、等到它才交）。
    {
        const uint64_t handoff = (uint64_t)frame->duration;
        if (handoff >= (uint64_t)frame->pkt_dts && beforeRender >= handoff && m_DiagN < kDiagMax) {
            m_DiagPaceUs[m_DiagN] = (uint32_t)(handoff - (uint64_t)frame->pkt_dts);
            m_DiagRendUs[m_DiagN] = (uint32_t)(beforeRender - handoff);
            m_DiagN++;
        }
        if (m_DiagStartUs == 0) {
            m_DiagStartUs = beforeRender;
        }
    }

    // Render it
    m_VsyncRenderer->renderFrame(frame);
    uint64_t afterRender = LiGetMicroseconds();

    m_VideoStats->totalRenderTimeUs += (afterRender - beforeRender);
    if (onRendered) {
        onRendered(beforeRender, afterRender, frame);  // §VIPLE-2D-PERF
    }
    m_VideoStats->renderedFrames++;

    // VipleStream: Track FRUC interpolated frames.
    // §B2 2026-05-06 — TRIPLE 模式 1 server frame 推 2 張 interp，必須 +2
    // 才能讓 ffmpeg.cpp effectiveFps 算出正確「有效輸出幀率」.  接口
    // lastFrameInterpolatedCount() 預設行為兼容 DUAL (回傳 1).
    m_VideoStats->frucInterpolatedFrames += m_VsyncRenderer->lastFrameInterpolatedCount();

    // §PERF-1113：每 10 秒一次的彙總（240 次 snprintf、兩次排序、兩行 log）原本排在 renderFrame 之前，
    // 那一幀因此晚交 0.1～0.3 ms；搬到這一幀交出去之後。上面只留每幀兩筆樣本的記錄
    {
        if (beforeRender - m_DiagStartUs >= 10000000ull && m_DiagN > 0) {
            {
                // 這一窗最後 240 張的逐幀等待（0.1 ms 為單位），看多排的那一格怎麼來、怎麼走
                char seq[240 * 4 + 8];
                size_t o = 0;
                for (uint32_t i = m_DiagN > 240 ? m_DiagN - 240 : 0; i < m_DiagN && o + 5 < sizeof(seq); i++) {
                    o += (size_t)snprintf(seq + o, sizeof(seq) - o, "%u ", (m_DiagPaceUs[i] + 50) / 100);
                }
                seq[o] = 0;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-PACER-SEQ] drops=%u %s", (unsigned)m_VideoStats->pacerDroppedFrames, seq);
            }
            std::sort(m_DiagPaceUs, m_DiagPaceUs + m_DiagN);
            std::sort(m_DiagRendUs, m_DiagRendUs + m_DiagN);
            auto q = [this](const uint32_t* v, double p) { return v[(size_t)(p * (m_DiagN - 1))] / 1000.0; };
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-PACER10] n=%u pace p05/p50/p95=%.2f/%.2f/%.2f ms rend p05/p50/p95=%.2f/%.2f/%.2f ms | at vsync: empty=%u one=%u more=%u late=%u | display=%d Hz stream=%d fps",
                        m_DiagN, q(m_DiagPaceUs, 0.05), q(m_DiagPaceUs, 0.5), q(m_DiagPaceUs, 0.95),
                        q(m_DiagRendUs, 0.05), q(m_DiagRendUs, 0.5), q(m_DiagRendUs, 0.95),
                        m_DiagVsEmpty.exchange(0), m_DiagVsOne.exchange(0), m_DiagVsMore.exchange(0), m_DiagVsLate.exchange(0),
                        m_DisplayFps, m_MaxVideoFps);
            m_DiagN = 0;
            m_DiagStartUs = beforeRender;
        }
    }

    // Wait until after next frame to free this one to ensure the GPU
    // doesn't stall or read garbage if the backing buffer gets returned
    // to the pool and the decoder tries to write a new frame into it
    std::swap(frame, m_DeferredFreeFrame);
    av_frame_free(&frame);

    // Drop frames if we have too many queued up for a while
    m_FrameQueueLock.lock();

    int frameDropTarget;

    if (m_RendererAttributes & RENDERER_ATTRIBUTE_NO_BUFFERING) {
        // Renderers that don't buffer any frames but don't support waitToRender() need us to buffer
        // an extra frame to ensure they don't starve while waiting to present.
        frameDropTarget = 1;
    }
    else {
        frameDropTarget = 0;
        for (int queueHistoryEntry : std::as_const(m_RenderQueueHistory)) {
            if (queueHistoryEntry == 0) {
                // Be lenient as long as the queue length
                // resolves before the end of frame history
                frameDropTarget = 2;
                break;
            }
        }

        // Keep a rolling 500 ms window of render queue history
        if (m_RenderQueueHistory.count() == m_MaxVideoFps / 2) {
            m_RenderQueueHistory.dequeue();
        }

        m_RenderQueueHistory.enqueue(m_RenderQueue.count());
    }

    // Catch up if we're several frames ahead
    while (m_RenderQueue.count() > frameDropTarget) {
        AVFrame* frame = m_RenderQueue.dequeue();

        // Drop the lock while we call av_frame_free()
        m_FrameQueueLock.unlock();
        m_VideoStats->pacerDroppedFrames++;
        av_frame_free(&frame);
        m_FrameQueueLock.lock();
    }
    FreezeDiag::s_RenderQueueDepth.store(m_RenderQueue.count(), std::memory_order_relaxed);

    m_FrameQueueLock.unlock();
}

void Pacer::dropFrameForEnqueue(QQueue<AVFrame*>& queue)
{
    SDL_assert(queue.size() <= MAX_QUEUED_FRAMES);
    // §REVIEW-1104：持鎖呼叫 av_frame_free 會占住 pool mutex；比照同檔另外兩處先解鎖，回來後重查佇列是否仍滿
    while (queue.size() >= MAX_QUEUED_FRAMES) {
        AVFrame* frame = queue.dequeue();
        m_FrameQueueLock.unlock();
        av_frame_free(&frame);
        m_FrameQueueLock.lock();
    }
}

void Pacer::submitFrame(AVFrame* frame)
{
    // Make sure initialize() has been called
    SDL_assert(m_MaxVideoFps != 0);

    // Queue the frame and possibly wake up the render thread
    m_FrameQueueLock.lock();
    if (m_VsyncSource != nullptr) {
        dropFrameForEnqueue(m_PacingQueue);
        m_PacingQueue.enqueue(frame);
        m_FrameQueueLock.unlock();
        m_PacingQueueNotEmpty.wakeOne();
    }
    else {
        enqueueFrameForRenderingAndUnlock(frame);
    }
}
