#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include <Limelight.h>

#include <vulkan/vulkan.h>

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

struct pyrowave_device_opaque;
struct pyrowave_decoder_opaque;

// §PYROWAVE-ZC：renderer（PlVkRenderer）交給 bridge 的 Vulkan 共用資訊。
// 所有指標的生命週期都跟 renderer 一樣長；bridge 一定比 renderer 先銷毀（FFmpegVideoDecoder::reset）。
struct PyroWaveVkInterop
{
    PFN_vkGetInstanceProcAddr getInstanceProcAddr = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    uint32_t apiVersion = 0;

    const char* const* instanceExtensions = nullptr;
    int numInstanceExtensions = 0;
    const char* const* deviceExtensions = nullptr;
    int numDeviceExtensions = 0;
    // 建 device 時實際開啟的功能鏈（開頭是 VkPhysicalDeviceFeatures2）
    const VkPhysicalDeviceFeatures2* features = nullptr;

    // libplacebo 開的 queue（family index、數量）
    struct Queue { uint32_t family; uint32_t count; };
    Queue graphics = {};
    Queue compute = {};
    Queue transfer = {};

    // libplacebo 的 queue 鎖：所有對共用 VkQueue 的 submit 都要包在裡面
    void (*lockQueue)(void* opaque, uint32_t family, uint32_t index) = nullptr;
    void (*unlockQueue)(void* opaque, uint32_t family, uint32_t index) = nullptr;
    void* lockOpaque = nullptr;

    // 同一個 VkDevice 的 FFmpeg Vulkan device context（AV_HWDEVICE_TYPE_VULKAN），用來配 AVVkFrame
    AVBufferRef* hwDeviceCtx = nullptr;
};

class PyroWaveDecoderBridge
{
public:
    PyroWaveDecoderBridge();
    ~PyroWaveDecoderBridge();

    static bool isHardwareAvailable();
    static AVPixelFormat getOutputPixelFormat(int videoFormat);

    // 拷貝版：自建 Vulkan device，解碼後讀回 CPU（任何 renderer 都能用）
    bool initialize(int videoFormat, int width, int height);
    // §PYROWAVE-ZC 零複製：共用 renderer 的 VkDevice，直接解碼進 AVVkFrame
    bool initializeZeroCopy(const PyroWaveVkInterop& interop, int videoFormat, int width, int height);
    void cleanup();

    int submitDecodeUnit(PDECODE_UNIT du, AVFrame* outFrame);
    bool createTestFrame(AVFrame* outFrame) const;

    int width() const { return m_Width; }
    int height() const { return m_Height; }
    int videoFormat() const { return m_VideoFormat; }
    bool isZeroCopy() const { return m_ZeroCopy; }

private:
    int decodeToVulkan(PDECODE_UNIT du, AVFrame* outFrame);
    bool waitPreviousGpuWork();
    void cleanupZeroCopy();

    static void lockAllQueues(void* userdata);
    static void unlockAllQueues(void* userdata);

    pyrowave_device_opaque* m_Device = nullptr;
    pyrowave_decoder_opaque* m_Decoder = nullptr;
    int m_VideoFormat = 0;
    int m_Width = 0;
    int m_Height = 0;
    // 一幀的 RTP 片段接成連續位元流（單一 PyroWave 封包可能跨片段）
    std::vector<uint8_t> m_FrameBuffer;

    // ---- §PYROWAVE-ZC ----
    bool m_ZeroCopy = false;
    PyroWaveVkInterop m_Interop;
    // pyrowave_create_device 要求建立參數在 device 存活期間都有效
    VkApplicationInfo m_AppInfo = {};
    VkInstanceCreateInfo m_InstanceCi = {};
    VkDeviceCreateInfo m_DeviceCi = {};
    std::vector<VkDeviceQueueCreateInfo> m_QueueCis;
    std::vector<float> m_QueuePriorities;
    AVBufferRef* m_FramesCtx = nullptr;
    VkQueue m_Queue = VK_NULL_HANDLE;
    VkCommandPool m_CmdPool = VK_NULL_HANDLE;
    VkCommandBuffer m_Cmd = VK_NULL_HANDLE;
    // 每次 submit +1；下一幀解碼前 CPU 先等它（PyroWave 借用 command buffer 時不知道 GPU 何時做完）
    VkSemaphore m_DoneSem = VK_NULL_HANDLE;
    uint64_t m_DoneValue = 0;
    int m_PlaneCount = 0;
    VkFormat m_PlaneFormat = VK_FORMAT_UNDEFINED;

    PFN_vkGetDeviceProcAddr m_vkGetDeviceProcAddr = nullptr;
    PFN_vkGetDeviceQueue m_vkGetDeviceQueue = nullptr;
    PFN_vkCreateCommandPool m_vkCreateCommandPool = nullptr;
    PFN_vkDestroyCommandPool m_vkDestroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers m_vkAllocateCommandBuffers = nullptr;
    PFN_vkResetCommandBuffer m_vkResetCommandBuffer = nullptr;
    PFN_vkBeginCommandBuffer m_vkBeginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer m_vkEndCommandBuffer = nullptr;
    PFN_vkCmdPipelineBarrier2 m_vkCmdPipelineBarrier2 = nullptr;
    PFN_vkQueueSubmit2 m_vkQueueSubmit2 = nullptr;
    PFN_vkCreateSemaphore m_vkCreateSemaphore = nullptr;
    PFN_vkDestroySemaphore m_vkDestroySemaphore = nullptr;
    PFN_vkWaitSemaphores m_vkWaitSemaphores = nullptr;
};
