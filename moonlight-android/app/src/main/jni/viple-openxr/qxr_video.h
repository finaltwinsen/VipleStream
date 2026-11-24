#pragma once

// 影像層不碰 OpenXR。裝置由呼叫端建好傳進來，這一層只負責
// AImageReader、AHardwareBuffer 匯入與畫進呼叫端給的目標。
// 手機上沒有 runtime 時可以單獨餵 Surface、呼叫 consumeLatest／renderTo。

#include <atomic>
#include <cstdint>

// AHardwareBuffer_getId 是 API 31 才有。平台設在 28，不先把較新的符號標成 weak，
// 連結會去找 stub 裡不存在的強符號，arm64 以外的載入路徑也會被這個 .so 拖垮。
#ifndef __ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__
#define __ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__ 1
#endif
#include <android/versioning.h>

#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR 1
#endif
#include <vulkan/vulkan.h>

struct ANativeWindow;

struct QxrUvRect {
    float u0;
    float v0;
    float uScale;
    float vScale;
};

struct QxrVideoCreateInfo {
    VkPhysicalDevice physicalDevice;
    VkDevice device;
    uint32_t queueFamily;
    VkFormat targetFormat;
    int32_t streamWidth;
    int32_t streamHeight;
};

class QxrVideo {
public:
    QxrVideo() = default;
    ~QxrVideo();

    QxrVideo(const QxrVideo&) = delete;
    QxrVideo& operator=(const QxrVideo&) = delete;

    bool create(const QxrVideoCreateInfo& info, const char** error);
    void destroy();

    ANativeWindow* decoderWindow() const { return window; }

    // listener 只把旗標設起來。沒有 OpenXR 的測試可以停在這裡等下一張。
    bool waitForImage(int timeoutMs);

    // onImageAvailable 寫入的 CLOCK_MONOTONIC。還沒進過圖是 0。
    uint64_t lastImageAvailableNs() const {
        return imageAvailableNs.load(std::memory_order_acquire);
    }

    // 呼叫端必須已經等完 GPU。projection 的 VkImageView 即將銷毀，綁在上面的 framebuffer 不能留著。
    void clearTargetFramebuffers();

    // 上一張用到這些 VkImage 的指令必須已經做完。快取淘汰會拆掉它們。
    // 回傳值：目前有沒有一張可以取樣的畫面（含上一張還沒被換掉的）。
    bool consumeLatest();
    // 只把 AImageReader 排到剩最新一張，不匯入、不畫。
    // 呼叫端未必已等到上一張 fence，這裡不能拆快取裡的 VkImage。
    void drainReader();
    bool hasFrame() const { return currentView != VK_NULL_HANDLE; }
    QxrUvRect sourceUv() const { return uv; }
    int64_t frameNumber() const { return latchedFrame; }

    // 沒有畫面時只清成純色，仍然回 true（呼叫端還是要送出這張）。
    bool renderTo(VkCommandBuffer cmd, VkImageView target, uint32_t width, uint32_t height, QxrUvRect src);

    void pumpStats();

private:
    struct CacheEntry {
        bool occupied = false;
        bool hasId = false;
        uint64_t id = 0;
        uintptr_t ptr = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint64_t lastUsed = 0;
        uint64_t externalFormat = 0;
        void* buffer = nullptr;
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
    };

    struct TargetFb {
        VkImageView view = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        uint32_t width = 0;
        uint32_t height = 0;
        uint64_t lastUsed = 0;
    };

    void* pullNewest();
    bool importLatched(void* image);
    void recordQueueTransfer(VkCommandBuffer cmd, VkImage image, bool toDevice, VkImageLayout sampleLayout);
    bool ensureFormat(const void* formatProps);
    bool createYcbcrSampler(const void* formatProps);
    bool createPlainSampler();
    bool createPipeline();
    void destroyPipeline();
    void destroySampler();
    void flushCache();
    void destroyEntry(CacheEntry* entry);
    CacheEntry* findCache(bool hasId, uint64_t id, uintptr_t ptr, uint32_t width, uint32_t height);
    CacheEntry* takeCacheSlot();
    VkFramebuffer framebufferFor(VkImageView view, uint32_t width, uint32_t height);
    void signalImage();
    void logStats();

    static void onImageAvailable(void* context, void* reader);

    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkFormat targetFormat = VK_FORMAT_UNDEFINED;
    bool targetSrgb = false;
    int32_t streamWidth = 0;
    int32_t streamHeight = 0;

    void* reader = nullptr;
    ANativeWindow* window = nullptr;
    void* latched = nullptr;
    // drainReader 留下、尚未匯入的最新一張。跟 latched 分開，才不會在 GPU 還在讀時把已匯入的那張放掉。
    void* held = nullptr;
    VkImage currentImage = VK_NULL_HANDLE;
    VkImageView currentView = VK_NULL_HANDLE;
    QxrUvRect uv = {0.f, 0.f, 1.f, 1.f};
    int64_t latchedFrame = -1;
    bool loggedFirstImage = false;
    bool loggedMaxImages = false;
    bool loggedAcquireError = false;
    bool loggedGetId = false;
    bool loggedBadCrop = false;
    bool loggedFbFull = false;
    bool loggedNoChroma = false;

    bool formatReady = false;
    bool useYcbcr = false;
    uint64_t boundExternalFormat = 0;
    VkFormat boundVkFormat = VK_FORMAT_UNDEFINED;

    PFN_vkGetAndroidHardwareBufferPropertiesANDROID getAhbProps = nullptr;
    PFN_vkCreateSamplerYcbcrConversion createYcbcr = nullptr;
    PFN_vkDestroySamplerYcbcrConversion destroyYcbcr = nullptr;

    VkSamplerYcbcrConversion conversion = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    VkDescriptorSet descSet = VK_NULL_HANDLE;
    VkRenderPass renderPass = VK_NULL_HANDLE;

    // 匯入快取要涵蓋解碼器實際輪替的緩衝區數。Pixel 5（Codec2）實測容量 8 時命中率只有約 50%，
    // 也就是一半的畫面每次都重新匯入；Codec2 的 max dequeue 是 25，再加 AImageReader 的 4 張。
    static constexpr int kCacheCap = 32;
    CacheEntry cache[kCacheCap];
    uint64_t cacheTick = 0;
    // framebuffer 快取只需涵蓋同時在用的 swapchain 張數（2D quad 加上 projection），與匯入快取分開。
    static constexpr int kTargetCap = 16;
    TargetFb targets[kTargetCap];

    uint64_t statReceived = 0;
    uint64_t statAdopted = 0;
    uint64_t statDiscarded = 0;
    uint64_t statHit = 0;
    uint64_t statMiss = 0;
    uint64_t snapReceived = 0;
    uint64_t snapAdopted = 0;
    uint64_t snapDiscarded = 0;
    uint64_t snapHit = 0;
    uint64_t snapMiss = 0;
    int64_t statWindowStartNs = 0;

    // waitForImage 與 signalImage 共用。pending 要在這把鎖裡寫入再 notify，喚醒才不會落在「已檢查、尚未等待」之間。
    // listener 只在 signalImage 裡短暫持有，不能拿去取圖或匯入。
    void* waitMu = nullptr;
    void* waitCv = nullptr;
    std::atomic<int> pending{0};
    // 診斷：listener 被呼叫的累計次數。取圖少於這個數字代表佇列被別的東西吃掉；兩者都停代表生產端沒送。
    std::atomic<unsigned long long> statListener{0};
    std::atomic<int> stopping{0};
    std::atomic<uint64_t> imageAvailableNs{0};

    const char* failReason = nullptr;
};
