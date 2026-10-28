#include "pyrowave_decoder_bridge.h"
#include "SDL_compat.h"

#include <algorithm>
#include <atomic>
#include <cstring>

#include <vulkan/vulkan.h>
#include "pyrowave.h"

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/pixdesc.h>
}

PyroWaveDecoderBridge::PyroWaveDecoderBridge() = default;

PyroWaveDecoderBridge::~PyroWaveDecoderBridge()
{
    cleanup();
}

bool PyroWaveDecoderBridge::isHardwareAvailable()
{
    static std::atomic<int> s_Cached{-1};
    int cached = s_Cached.load(std::memory_order_acquire);
    if (cached >= 0) {
        return cached != 0;
    }

    pyrowave_device dev = nullptr;
    pyrowave_result res = pyrowave_create_default_device(&dev);
    bool ok = (res == PYROWAVE_SUCCESS && dev != nullptr);
    if (dev) {
        pyrowave_device_destroy(dev);
    }

    s_Cached.store(ok ? 1 : 0, std::memory_order_release);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[PyroWave] Vulkan hardware decoder probe: %s",
                ok ? "AVAILABLE" : "UNAVAILABLE");
    return ok;
}

AVPixelFormat PyroWaveDecoderBridge::getOutputPixelFormat(int videoFormat)
{
    const bool is10Bit = (videoFormat & VIDEO_FORMAT_MASK_10BIT) != 0;
    const bool is444 = (videoFormat & VIDEO_FORMAT_MASK_YUV444) != 0;

    if (is444) {
        return is10Bit ? AV_PIX_FMT_YUV444P10LE : AV_PIX_FMT_YUV444P;
    }
    return is10Bit ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_YUV420P;
}

bool PyroWaveDecoderBridge::initialize(int videoFormat, int width, int height)
{
    cleanup();

    if ((videoFormat & VIDEO_FORMAT_MASK_PYROWAVE) == 0 || width <= 0 || height <= 0) {
        return false;
    }

    const bool is444 = (videoFormat & VIDEO_FORMAT_MASK_YUV444) != 0;
    const bool is10Bit = (videoFormat & VIDEO_FORMAT_MASK_10BIT) != 0;

    // 4:2:0 requires even dimensions in PyroWave
    int alignedWidth = is444 ? width : ((width + 1) & ~1);
    int alignedHeight = is444 ? height : ((height + 1) & ~1);

    pyrowave_result res = pyrowave_create_default_device(&m_Device);
    if (res != PYROWAVE_SUCCESS || !m_Device) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[PyroWave] Failed to create Vulkan device (err=%d)",
                     (int)res);
        m_Device = nullptr;
        return false;
    }

    pyrowave_decoder_create_info decInfo = {};
    decInfo.device = m_Device;
    decInfo.width = alignedWidth;
    decInfo.height = alignedHeight;
    decInfo.chroma = is444 ? PYROWAVE_CHROMA_SUBSAMPLING_444 : PYROWAVE_CHROMA_SUBSAMPLING_420;
    // 10-bit CPU buffer scaling factor requires compute path (not fragment path)
    decInfo.fragment_path = is10Bit ? false : pyrowave_decoder_device_prefers_fragment_path(m_Device);

    res = pyrowave_decoder_create(&decInfo, &m_Decoder);
    if (res != PYROWAVE_SUCCESS || !m_Decoder) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[PyroWave] Failed to create decoder %dx%d (chroma=%s, err=%d)",
                     alignedWidth, alignedHeight, is444 ? "444" : "420", (int)res);
        cleanup();
        return false;
    }

    m_VideoFormat = videoFormat;
    m_Width = alignedWidth;
    m_Height = alignedHeight;

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[PyroWave] Initialized decoder: %dx%d format=0x%x chroma=%s bitDepth=%d fragmentPath=%d",
                m_Width, m_Height, m_VideoFormat,
                is444 ? "4:4:4" : "4:2:0",
                is10Bit ? 10 : 8,
                decInfo.fragment_path ? 1 : 0);
    return true;
}

void PyroWaveDecoderBridge::cleanup()
{
    // 零複製：先等 GPU 做完我們送出的工作，再拆 decoder／device（device 是借來的，不會被銷毀）
    cleanupZeroCopy();

    if (m_Decoder) {
        pyrowave_decoder_destroy(m_Decoder);
        m_Decoder = nullptr;
    }
    if (m_Device) {
        pyrowave_device_destroy(m_Device);
        m_Device = nullptr;
    }
    // 共用 device 的 queue 鎖在 pyrowave_device_destroy 期間還會用到，之後才清
    m_Interop = PyroWaveVkInterop();
    m_QueueCis.clear();
    m_VideoFormat = 0;
    m_Width = 0;
    m_Height = 0;
}

bool PyroWaveDecoderBridge::createTestFrame(AVFrame* outFrame) const
{
    if (!outFrame || m_Width <= 0 || m_Height <= 0) {
        return false;
    }

    av_frame_unref(outFrame);
    outFrame->format = getOutputPixelFormat(m_VideoFormat);
    outFrame->width = m_Width;
    outFrame->height = m_Height;
    outFrame->colorspace = (m_VideoFormat & VIDEO_FORMAT_MASK_10BIT) ? AVCOL_SPC_BT2020_NCL : AVCOL_SPC_BT709;
    outFrame->color_primaries = (m_VideoFormat & VIDEO_FORMAT_MASK_10BIT) ? AVCOL_PRI_BT2020 : AVCOL_PRI_BT709;
    outFrame->color_trc = (m_VideoFormat & VIDEO_FORMAT_MASK_10BIT) ? AVCOL_TRC_SMPTE2084 : AVCOL_TRC_BT709;

    if (av_frame_get_buffer(outFrame, 32) < 0) {
        return false;
    }

    const bool is10Bit = (m_VideoFormat & VIDEO_FORMAT_MASK_10BIT) != 0;
    const bool is444 = (m_VideoFormat & VIDEO_FORMAT_MASK_YUV444) != 0;

    for (int p = 0; p < 3; ++p) {
        int planeH = (p != 0 && !is444) ? (m_Height / 2) : m_Height;
        int planeW = (p != 0 && !is444) ? (m_Width / 2) : m_Width;
        if (!is10Bit) {
            uint8_t fill = (p == 0) ? 16 : 128;
            for (int y = 0; y < planeH; ++y) {
                std::memset(outFrame->data[p] + y * outFrame->linesize[p], fill, planeW);
            }
        }
        else {
            uint16_t fill = (p == 0) ? 64 : 512;
            for (int y = 0; y < planeH; ++y) {
                auto* row = reinterpret_cast<uint16_t*>(outFrame->data[p] + y * outFrame->linesize[p]);
                for (int x = 0; x < planeW; ++x) {
                    row[x] = fill;
                }
            }
        }
    }

    return true;
}

// PyroWave 的 C API 把 decoder 內部的 LOGE 導到 null logger，push_packet 失敗時看不到原因。
// 這裡照 Decoder::Impl::push_packet 的規則（pyrowave_decoder.cpp）重走一遍，印出第一個不成立的條件。
static void logPushPacketFailure(const PDECODE_UNIT du, int entryIdx, int entryCount,
                                 const uint8_t* data, size_t size, int width, int height, bool is444)
{
    const int blocks32 = ((width + 31) / 32) * ((height + 31) / 32) * (is444 ? 3 : 1); // 約略上限，僅供比對
    size_t off = 0;
    while (size - off >= 8) {
        uint16_t w1;
        uint32_t w2;
        std::memcpy(&w1, data + off + 2, 2);
        std::memcpy(&w2, data + off + 4, 4);
        const unsigned payloadWords = w1 & 0xfff;
        const unsigned seq = (w1 >> 12) & 7;
        const unsigned extended = (w1 >> 15) & 1;
        if (extended) {
            uint32_t s0, s1;
            std::memcpy(&s0, data + off, 4);
            std::memcpy(&s1, data + off + 4, 4);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-PYROWAVE] diag frame=%d entry=%d/%d off=%zu SEQ hdr %ux%u seq=%u total_blocks=%u code=%u chroma=%u (decoder %dx%d %s)",
                        du->frameNumber, entryIdx, entryCount, off,
                        (s0 & 0x3fff) + 1, ((s0 >> 14) & 0x3fff) + 1, (s0 >> 28) & 7,
                        s1 & 0xffffff, (s1 >> 24) & 3, (s1 >> 26) & 1,
                        width, height, is444 ? "444" : "420");
            off += 8;
            continue;
        }
        if (payloadWords == 0) {
            break;
        }
        const size_t pktSize = (size_t)payloadWords * 4;
        const unsigned blockIndex = w2 >> 8;
        if (pktSize > size - off) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-PYROWAVE] diag frame=%d entry=%d/%d off=%zu OVERRUN packet=%zu B left=%zu B (seq=%u block=%u)",
                        du->frameNumber, entryIdx, entryCount, off, pktSize, size - off, seq, blockIndex);
            return;
        }
        if ((int)blockIndex >= blocks32 * 2) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-PYROWAVE] diag frame=%d entry=%d/%d off=%zu BLOCK_INDEX %u suspicious (seq=%u words=%u)",
                        du->frameNumber, entryIdx, entryCount, off, blockIndex, seq, payloadWords);
            return;
        }
        off += pktSize;
    }
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-PYROWAVE] diag frame=%d entry=%d/%d len=%zu: headers parse OK (failure is inside decode_packet or chroma/size check)",
                du->frameNumber, entryIdx, entryCount, size);
}

int PyroWaveDecoderBridge::submitDecodeUnit(PDECODE_UNIT du, AVFrame* outFrame)
{
    if (!m_Decoder || !du || !outFrame) {
        return DR_NEED_IDR;
    }

    // server 送的是連續位元流，單一 PyroWave 封包可能跨好幾個 RTP 片段，所以先把整幀
    // 接成一段再 push。不完整的幀（DU_FLAG_PARTIAL_FRAME）由 common-c 保證是「從頭開始、
    // 沒有缺口的前綴」（RtpVideoQueue 的 stagePyroWaveContiguousPrefix），最後一個封包可能
    // 被截斷：push 會在那裡失敗，但前面已解析的區塊會留著，缺的區塊解碼時補 0。
    const bool isPartial = (du->flags & DU_FLAG_PARTIAL_FRAME) != 0;

    m_FrameBuffer.clear();
    m_FrameBuffer.reserve((size_t)du->fullLength);
    for (PLENTRY entry = du->bufferList; entry != nullptr; entry = entry->next) {
        if (entry->data && entry->length > 0) {
            m_FrameBuffer.insert(m_FrameBuffer.end(),
                                 reinterpret_cast<const uint8_t*>(entry->data),
                                 reinterpret_cast<const uint8_t*>(entry->data) + entry->length);
        }
    }

    pyrowave_decoder_clear(m_Decoder);

    pyrowave_result pushRes = pyrowave_decoder_push_packet(m_Decoder, m_FrameBuffer.data(), m_FrameBuffer.size());
    if (pushRes != PYROWAVE_SUCCESS && !isPartial) {
        static std::atomic<int> s_DiagCount{0};
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[PyroWave] push_packet failed on frame %d (len=%zu, err=%d)",
                    du->frameNumber, m_FrameBuffer.size(), (int)pushRes);
        if (s_DiagCount.fetch_add(1) < 8) {
            logPushPacketFailure(du, 0, 1, m_FrameBuffer.data(), m_FrameBuffer.size(),
                                 m_Width, m_Height, (m_VideoFormat & VIDEO_FORMAT_MASK_YUV444) != 0);
        }
        // 每幀都是獨立的 intra 幀：丟掉這一幀就好，不要求 IDR
        return DR_OK;
    }

    // 不完整的幀：區塊編號從最粗的小波層排到最細的，前綴缺的一定是最細層（畫面下方的高頻細節）。
    // PyroWave 預設 90% 門檻是給「隨機丟」用的，這裡最低兩個頻帶照樣要求完整、區塊比例放寬到 50%：
    // 一幀局部稍糊勝過整幀丟掉（畫面停一格）。不夠就丟，重複上一幀。
    const bool ready = isPartial
        ? pyrowave_decoder_decode_is_ready_with_sideband(m_Decoder, true, 2, 0.5f, nullptr, 0)
        : pyrowave_decoder_decode_is_ready(m_Decoder, false);
    if (!ready) {
        static std::atomic<int> s_NotReadyCount{0};
        const int n = s_NotReadyCount.fetch_add(1);
        if (n < 5 || (n % 300) == 0) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-PYROWAVE] frame %d not decodable (partial=%d, %zu B received; total %d)",
                        du->frameNumber, isPartial ? 1 : 0, m_FrameBuffer.size(), n + 1);
        }
        return DR_OK;
    }
    if (isPartial) {
        static std::atomic<int> s_PartialDecoded{0};
        const int n = s_PartialDecoded.fetch_add(1);
        if (n < 5 || (n % 300) == 0) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-PYROWAVE] decoding partial frame %d (%zu B received; total %d)",
                        du->frameNumber, m_FrameBuffer.size(), n + 1);
        }
    }

    if (m_ZeroCopy) {
        return decodeToVulkan(du, outFrame);
    }

    av_frame_unref(outFrame);
    outFrame->format = getOutputPixelFormat(m_VideoFormat);
    outFrame->width = m_Width;
    outFrame->height = m_Height;

    const bool is10Bit = (m_VideoFormat & VIDEO_FORMAT_MASK_10BIT) != 0;
    const bool is444 = (m_VideoFormat & VIDEO_FORMAT_MASK_YUV444) != 0;
    const bool isHdr = is10Bit && LiGetCurrentHostDisplayHdrMode();

    outFrame->colorspace = isHdr ? AVCOL_SPC_BT2020_NCL : AVCOL_SPC_BT709;
    outFrame->color_primaries = isHdr ? AVCOL_PRI_BT2020 : AVCOL_PRI_BT709;
    outFrame->color_trc = isHdr ? AVCOL_TRC_SMPTE2084 : AVCOL_TRC_BT709;

    if (av_frame_get_buffer(outFrame, 32) < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[PyroWave] av_frame_get_buffer failed for %dx%d",
                     m_Width, m_Height);
        return DR_OK;
    }

    pyrowave_cpu_buffer cpuBuf = {};
    cpuBuf.width = m_Width;
    cpuBuf.height = m_Height;
    if (is444) {
        cpuBuf.format = is10Bit ? PYROWAVE_CPU_BUFFER_FORMAT_YUV444P10 : PYROWAVE_CPU_BUFFER_FORMAT_YUV444P;
    }
    else {
        cpuBuf.format = is10Bit ? PYROWAVE_CPU_BUFFER_FORMAT_YUV420P10 : PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
    }

    for (int p = 0; p < 3; ++p) {
        int planeHeight = (p != 0 && !is444) ? (m_Height / 2) : m_Height;
        cpuBuf.data[p] = outFrame->data[p];
        cpuBuf.row_stride_in_bytes[p] = (size_t)outFrame->linesize[p];
        cpuBuf.plane_size_in_bytes[p] = (size_t)outFrame->linesize[p] * (size_t)planeHeight;
    }

    pyrowave_result decRes = pyrowave_decoder_decode_cpu_buffer_synchronous(m_Decoder, &cpuBuf);
    if (decRes != PYROWAVE_SUCCESS) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[PyroWave] decode_cpu_buffer_synchronous failed on frame %d (err=%d)",
                    du->frameNumber, (int)decRes);
        av_frame_unref(outFrame);
        // 每幀都是 intra：丟這一幀就好，不要求 IDR
        return DR_OK;
    }

    return DR_OK;
}

// ============================================================================
// §PYROWAVE-ZC 零複製：共用 PlVkRenderer（libplacebo）的 VkDevice，PyroWave 直接解碼進
// FFmpeg 的 AVVkFrame，renderer 用 pl_map_avframe_ex 取樣，不經過 CPU。
//
// 每幀流程（全部在解碼執行緒）：
//   1. CPU 等上一幀的 m_DoneSem：PyroWave 借用我們的 command buffer 時，它內部的 staging
//      與常駐 payload buffer 每幀重用，但它不知道 GPU 何時做完。
//   2. 從 frames ctx 取一張 AVVkFrame（每個 plane 一張 R8／R16 影像），lock_frame。
//   3. 錄：UNDEFINED→GENERAL barrier（內容全覆寫，不用保留）→ PyroWave decode（compute，寫
//      storage image）→ compute 寫入對後續所有讀取可見。
//   4. submit：等 AVVkFrame.sem[p]＝sem_value[p]，signal sem_value[p]+1 與 m_DoneSem。
//   5. 更新 AVVkFrame 的 layout／access／sem_value／queue_family，unlock_frame。
// ============================================================================

void PyroWaveDecoderBridge::lockAllQueues(void* userdata)
{
    auto* me = static_cast<PyroWaveDecoderBridge*>(userdata);
    for (const auto& qci : me->m_QueueCis) {
        for (uint32_t i = 0; i < qci.queueCount; i++) {
            me->m_Interop.lockQueue(me->m_Interop.lockOpaque, qci.queueFamilyIndex, i);
        }
    }
}

void PyroWaveDecoderBridge::unlockAllQueues(void* userdata)
{
    auto* me = static_cast<PyroWaveDecoderBridge*>(userdata);
    for (auto it = me->m_QueueCis.rbegin(); it != me->m_QueueCis.rend(); ++it) {
        for (uint32_t i = it->queueCount; i > 0; i--) {
            me->m_Interop.unlockQueue(me->m_Interop.lockOpaque, it->queueFamilyIndex, i - 1);
        }
    }
}

bool PyroWaveDecoderBridge::initializeZeroCopy(const PyroWaveVkInterop& interop, int videoFormat, int width, int height)
{
    cleanup();

    if ((videoFormat & VIDEO_FORMAT_MASK_PYROWAVE) == 0 || width <= 0 || height <= 0 ||
        !interop.getInstanceProcAddr || !interop.device || !interop.features || !interop.hwDeviceCtx ||
        !interop.lockQueue || !interop.unlockQueue) {
        return false;
    }
    if (interop.apiVersion < VK_API_VERSION_1_3) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-PYROWAVE-ZC] Vulkan API 0x%x < 1.3, zero-copy unavailable", interop.apiVersion);
        return false;
    }

    m_Interop = interop;
    const bool is444 = (videoFormat & VIDEO_FORMAT_MASK_YUV444) != 0;
    const bool is10Bit = (videoFormat & VIDEO_FORMAT_MASK_10BIT) != 0;
    const int alignedWidth = is444 ? width : ((width + 1) & ~1);
    const int alignedHeight = is444 ? height : ((height + 1) & ~1);

    // ---- 函式指標（不連結 vulkan-1，一律從 libplacebo 的 loader 取）----
    auto gipa = interop.getInstanceProcAddr;
    m_vkGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)gipa(interop.instance, "vkGetDeviceProcAddr");
    if (!m_vkGetDeviceProcAddr) {
        cleanup();
        return false;
    }

    // 錄製與送出用 synchronization2、完成通知用 timeline semaphore：libplacebo 只在支援時才開，
    // 這裡確認真的有開，沒有就退回拷貝版
    {
        bool sync2 = false, timeline = false;
        for (auto* s = (const VkBaseInStructure*)interop.features; s != nullptr; s = s->pNext) {
            if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES) {
                sync2 |= ((const VkPhysicalDeviceVulkan13Features*)s)->synchronization2 == VK_TRUE;
            }
            else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES) {
                sync2 |= ((const VkPhysicalDeviceSynchronization2Features*)s)->synchronization2 == VK_TRUE;
            }
            else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
                timeline |= ((const VkPhysicalDeviceVulkan12Features*)s)->timelineSemaphore == VK_TRUE;
            }
            else if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES) {
                timeline |= ((const VkPhysicalDeviceTimelineSemaphoreFeatures*)s)->timelineSemaphore == VK_TRUE;
            }
        }
        if (!sync2 || !timeline) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-PYROWAVE-ZC] device lacks synchronization2=%d / timelineSemaphore=%d; using copy path",
                        sync2 ? 1 : 0, timeline ? 1 : 0);
            cleanup();
            return false;
        }

        // decode 是 compute dispatch，錄在 graphics family 的 command buffer 上，所以它要有 compute
        auto getQfProps = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)
            gipa(interop.instance, "vkGetPhysicalDeviceQueueFamilyProperties");
        uint32_t qfCount = 0;
        if (getQfProps) {
            getQfProps(interop.physicalDevice, &qfCount, nullptr);
        }
        std::vector<VkQueueFamilyProperties> qfProps(qfCount);
        if (getQfProps && qfCount > 0) {
            getQfProps(interop.physicalDevice, &qfCount, qfProps.data());
        }
        if (interop.graphics.family >= qfCount ||
            !(qfProps[interop.graphics.family].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-PYROWAVE-ZC] graphics queue family %u has no compute; using copy path",
                        interop.graphics.family);
            cleanup();
            return false;
        }
    }
#define PW_LOAD(name) m_##name = (PFN_##name)m_vkGetDeviceProcAddr(interop.device, #name); \
    if (!m_##name) { SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-PYROWAVE-ZC] missing " #name); cleanupZeroCopy(); return false; }
    PW_LOAD(vkGetDeviceQueue)
    PW_LOAD(vkCreateCommandPool)
    PW_LOAD(vkDestroyCommandPool)
    PW_LOAD(vkAllocateCommandBuffers)
    PW_LOAD(vkResetCommandBuffer)
    PW_LOAD(vkBeginCommandBuffer)
    PW_LOAD(vkEndCommandBuffer)
    PW_LOAD(vkCmdPipelineBarrier2)
    PW_LOAD(vkQueueSubmit2)
    PW_LOAD(vkCreateSemaphore)
    PW_LOAD(vkDestroySemaphore)
    PW_LOAD(vkWaitSemaphores)
#undef PW_LOAD

    // ---- 重建 libplacebo 建 instance／device 時的參數給 Granite 解析 ----
    m_AppInfo = {};
    m_AppInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    m_AppInfo.apiVersion = interop.apiVersion;
    m_InstanceCi = {};
    m_InstanceCi.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    m_InstanceCi.pApplicationInfo = &m_AppInfo;
    m_InstanceCi.enabledExtensionCount = (uint32_t)interop.numInstanceExtensions;
    m_InstanceCi.ppEnabledExtensionNames = interop.instanceExtensions;

    m_QueueCis.clear();
    uint32_t maxCount = 1;
    for (const auto& q : { interop.graphics, interop.compute, interop.transfer }) {
        if (q.count == 0) {
            continue;
        }
        bool dup = false;
        for (const auto& existing : m_QueueCis) {
            dup |= existing.queueFamilyIndex == q.family;
        }
        if (dup) {
            continue;
        }
        VkDeviceQueueCreateInfo qci = {};
        qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = q.family;
        qci.queueCount = q.count;
        m_QueueCis.push_back(qci);
        maxCount = std::max(maxCount, q.count);
    }
    m_QueuePriorities.assign(maxCount, 1.0f);
    for (auto& qci : m_QueueCis) {
        qci.pQueuePriorities = m_QueuePriorities.data();
    }
    if (interop.graphics.count == 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-PYROWAVE-ZC] no graphics queue");
        cleanupZeroCopy();
        return false;
    }

    m_DeviceCi = {};
    m_DeviceCi.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    m_DeviceCi.pNext = interop.features; // Granite 要求開頭是 VkPhysicalDeviceFeatures2
    m_DeviceCi.queueCreateInfoCount = (uint32_t)m_QueueCis.size();
    m_DeviceCi.pQueueCreateInfos = m_QueueCis.data();
    m_DeviceCi.enabledExtensionCount = (uint32_t)interop.numDeviceExtensions;
    m_DeviceCi.ppEnabledExtensionNames = interop.deviceExtensions;

    std::vector<pyrowave_device_create_queue_info> queueInfos;
    for (const auto& qci : m_QueueCis) {
        for (uint32_t i = 0; i < qci.queueCount; i++) {
            pyrowave_device_create_queue_info qi = {};
            m_vkGetDeviceQueue(interop.device, qci.queueFamilyIndex, i, &qi.queue);
            qi.familyIndex = qci.queueFamilyIndex;
            qi.index = i;
            queueInfos.push_back(qi);
        }
    }

    pyrowave_device_create_info devInfo = {};
    devInfo.GetInstanceProcAddr = interop.getInstanceProcAddr;
    devInfo.instance = interop.instance;
    devInfo.physical_device = interop.physicalDevice;
    devInfo.device = interop.device;
    devInfo.instance_create_info = &m_InstanceCi;
    devInfo.device_create_info = &m_DeviceCi;
    devInfo.queue_info = queueInfos.data();
    devInfo.queue_info_count = (uint32_t)queueInfos.size();
    devInfo.queue_lock_callback = lockAllQueues;
    devInfo.queue_unlock_callback = unlockAllQueues;
    devInfo.userdata = this;

    pyrowave_result res = pyrowave_create_device(&devInfo, &m_Device);
    if (res != PYROWAVE_SUCCESS || !m_Device) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-PYROWAVE-ZC] pyrowave_create_device on shared VkDevice failed (err=%d)", (int)res);
        m_Device = nullptr;
        cleanupZeroCopy();
        return false;
    }

    // 我們的 command buffer 來自 graphics family（同時支援 compute），跟 libplacebo 算繪同一個
    // family，影像不用做 queue family ownership 轉移。
    pyrowave_device_set_queue_type(m_Device, VK_QUEUE_GRAPHICS_BIT);

    if (pyrowave_decoder_device_prefers_fragment_path(m_Device)) {
        // fragment path 要 ATTACHMENT layout／COLOR_ATTACHMENT usage，零複製先只做 compute path
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-PYROWAVE-ZC] device prefers fragment path; using copy path instead");
        cleanup();
        return false;
    }

    pyrowave_decoder_create_info decInfo = {};
    decInfo.device = m_Device;
    decInfo.width = alignedWidth;
    decInfo.height = alignedHeight;
    decInfo.chroma = is444 ? PYROWAVE_CHROMA_SUBSAMPLING_444 : PYROWAVE_CHROMA_SUBSAMPLING_420;
    decInfo.fragment_path = false;
    res = pyrowave_decoder_create(&decInfo, &m_Decoder);
    if (res != PYROWAVE_SUCCESS || !m_Decoder) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-PYROWAVE-ZC] pyrowave_decoder_create on shared VkDevice failed (err=%d)", (int)res);
        m_Decoder = nullptr;
        cleanup();
        return false;
    }
    // 10-bit 存在 R16_UNORM 的低 10 bit（FFmpeg yuv4xxp10 的慣例），libplacebo 依 sw_format 解讀
    pyrowave_decoder_set_ycbcr_scaling_factor(m_Decoder, is10Bit ? float(0x3ff) / float(0xffff) : 1.0f);

    // ---- AVVkFrame 池：每個 plane 一張單通道影像（停用 multiplane，PyroWave 要 storage image）----
    m_FramesCtx = av_hwframe_ctx_alloc(interop.hwDeviceCtx);
    if (!m_FramesCtx) {
        cleanup();
        return false;
    }
    auto* fc = (AVHWFramesContext*)m_FramesCtx->data;
    fc->format = AV_PIX_FMT_VULKAN;
    fc->sw_format = getOutputPixelFormat(videoFormat);
    fc->width = alignedWidth;
    fc->height = alignedHeight;
    fc->initial_pool_size = 0; // 動態配置：Pacer／renderer 手上可能同時握著好幾張
    auto* vkfc = (AVVulkanFramesContext*)fc->hwctx;
    vkfc->tiling = VK_IMAGE_TILING_OPTIMAL;
    vkfc->usage = (VkImageUsageFlagBits)(VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    vkfc->flags = (AVVkFrameFlags)(vkfc->flags | AV_VK_FRAME_FLAG_DISABLE_MULTIPLANE);
    int err = av_hwframe_ctx_init(m_FramesCtx);
    if (err < 0) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-PYROWAVE-ZC] av_hwframe_ctx_init(%s) failed: %d",
                    av_get_pix_fmt_name(fc->sw_format), err);
        cleanup();
        return false;
    }
    m_PlaneCount = av_pix_fmt_count_planes(fc->sw_format);
    m_PlaneFormat = is10Bit ? VK_FORMAT_R16_UNORM : VK_FORMAT_R8_UNORM;
    if (m_PlaneCount != 3 || vkfc->format[0] != m_PlaneFormat ||
        vkfc->format[1] != m_PlaneFormat || vkfc->format[2] != m_PlaneFormat) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-PYROWAVE-ZC] unexpected frame layout: planes=%d formats=%d/%d/%d (want %d)",
                    m_PlaneCount, (int)vkfc->format[0], (int)vkfc->format[1], (int)vkfc->format[2],
                    (int)m_PlaneFormat);
        cleanup();
        return false;
    }

    // ---- command buffer＋完成用的 timeline semaphore ----
    m_vkGetDeviceQueue(interop.device, interop.graphics.family, 0, &m_Queue);

    VkCommandPoolCreateInfo poolCi = {};
    poolCi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolCi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolCi.queueFamilyIndex = interop.graphics.family;
    if (m_vkCreateCommandPool(interop.device, &poolCi, nullptr, &m_CmdPool) != VK_SUCCESS) {
        cleanup();
        return false;
    }
    VkCommandBufferAllocateInfo cbAi = {};
    cbAi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbAi.commandPool = m_CmdPool;
    cbAi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbAi.commandBufferCount = 1;
    if (m_vkAllocateCommandBuffers(interop.device, &cbAi, &m_Cmd) != VK_SUCCESS) {
        cleanup();
        return false;
    }

    VkSemaphoreTypeCreateInfo semType = {};
    semType.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    semType.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    semType.initialValue = 0;
    VkSemaphoreCreateInfo semCi = {};
    semCi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    semCi.pNext = &semType;
    if (m_vkCreateSemaphore(interop.device, &semCi, nullptr, &m_DoneSem) != VK_SUCCESS) {
        cleanup();
        return false;
    }
    m_DoneValue = 0;

    m_ZeroCopy = true;
    m_VideoFormat = videoFormat;
    m_Width = alignedWidth;
    m_Height = alignedHeight;

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-PYROWAVE-ZC] zero-copy decoder on shared VkDevice: %dx%d %s %d-bit, sw_format=%s, queue family %u",
                m_Width, m_Height, is444 ? "4:4:4" : "4:2:0", is10Bit ? 10 : 8,
                av_get_pix_fmt_name(fc->sw_format), interop.graphics.family);
    return true;
}

bool PyroWaveDecoderBridge::waitPreviousGpuWork()
{
    if (m_DoneValue == 0) {
        return true;
    }
    VkSemaphoreWaitInfo wi = {};
    wi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    wi.semaphoreCount = 1;
    wi.pSemaphores = &m_DoneSem;
    wi.pValues = &m_DoneValue;
    // 1 秒逾時：正常 1 ms 內完成，等不到代表 device 出事，不要永遠卡住解碼執行緒
    VkResult vr = m_vkWaitSemaphores(m_Interop.device, &wi, 1000ULL * 1000 * 1000);
    if (vr != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-PYROWAVE-ZC] wait for previous decode failed: %d", (int)vr);
        return false;
    }
    return true;
}

int PyroWaveDecoderBridge::decodeToVulkan(PDECODE_UNIT du, AVFrame* outFrame)
{
    if (!waitPreviousGpuWork()) {
        return DR_OK;
    }

    av_frame_unref(outFrame);
    int err = av_hwframe_get_buffer(m_FramesCtx, outFrame, 0);
    if (err < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-PYROWAVE-ZC] av_hwframe_get_buffer failed: %d", err);
        return DR_OK;
    }

    auto* fc = (AVHWFramesContext*)m_FramesCtx->data;
    auto* vkfc = (AVVulkanFramesContext*)fc->hwctx;
    auto* vkf = (AVVkFrame*)outFrame->data[0];
    const bool is444 = (m_VideoFormat & VIDEO_FORMAT_MASK_YUV444) != 0;

    vkfc->lock_frame(fc, vkf);

    VkCommandBufferBeginInfo bi = {};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VkResult cbRes = m_vkResetCommandBuffer(m_Cmd, 0);
    if (cbRes == VK_SUCCESS) {
        cbRes = m_vkBeginCommandBuffer(m_Cmd, &bi);
    }
    if (cbRes != VK_SUCCESS) {
        vkfc->unlock_frame(fc, vkf);
        av_frame_unref(outFrame);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-PYROWAVE-ZC] command buffer reset/begin failed on frame %d: %d",
                     du->frameNumber, (int)cbRes);
        return DR_OK;
    }

    VkImageMemoryBarrier2 toGeneral[3] = {};
    for (int p = 0; p < 3; p++) {
        auto& b = toGeneral[p];
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        // 先前的讀取（libplacebo 取樣）由 semaphore 等待涵蓋；內容全覆寫，舊 layout 直接丟
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.srcAccessMask = 0;
        b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = vkf->img[p];
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.levelCount = 1;
        b.subresourceRange.layerCount = 1;
    }
    VkDependencyInfo dep = {};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 3;
    dep.pImageMemoryBarriers = toGeneral;
    m_vkCmdPipelineBarrier2(m_Cmd, &dep);

    pyrowave_gpu_buffers buffers = {};
    for (int p = 0; p < 3; p++) {
        auto& v = buffers.planes[p];
        v.image = vkf->img[p];
        v.width = (uint32_t)((p != 0 && !is444) ? m_Width / 2 : m_Width);
        v.height = (uint32_t)((p != 0 && !is444) ? m_Height / 2 : m_Height);
        v.image_format = m_PlaneFormat;
        v.view_format = m_PlaneFormat;
        v.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        v.swizzle = VK_COMPONENT_SWIZZLE_IDENTITY;
        v.layout = VK_IMAGE_LAYOUT_GENERAL;
    }

    pyrowave_device_set_command_buffer(m_Device, m_Cmd);
    pyrowave_result res = pyrowave_decoder_decode_gpu_buffer(m_Decoder, nullptr, nullptr, &buffers);
    pyrowave_device_set_command_buffer(m_Device, VK_NULL_HANDLE);

    VkMemoryBarrier2 afterDecode = {};
    afterDecode.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    afterDecode.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    afterDecode.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    afterDecode.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    afterDecode.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
    VkDependencyInfo dep2 = {};
    dep2.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep2.memoryBarrierCount = 1;
    dep2.pMemoryBarriers = &afterDecode;
    m_vkCmdPipelineBarrier2(m_Cmd, &dep2);
    cbRes = m_vkEndCommandBuffer(m_Cmd);

    if (res != PYROWAVE_SUCCESS || cbRes != VK_SUCCESS) {
        // 錄好的 barrier 不送出：影像維持原狀態，這張幀直接丟回池
        vkfc->unlock_frame(fc, vkf);
        av_frame_unref(outFrame);
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-PYROWAVE-ZC] decode_gpu_buffer failed on frame %d (err=%d, end=%d)",
                    du->frameNumber, (int)res, (int)cbRes);
        return DR_OK;
    }

    VkSemaphoreSubmitInfo waits[3] = {};
    VkSemaphoreSubmitInfo signals[4] = {};
    for (int p = 0; p < 3; p++) {
        waits[p].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        waits[p].semaphore = vkf->sem[p];
        waits[p].value = vkf->sem_value[p];
        waits[p].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        signals[p].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        signals[p].semaphore = vkf->sem[p];
        signals[p].value = vkf->sem_value[p] + 1;
        signals[p].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    }
    signals[3].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signals[3].semaphore = m_DoneSem;
    signals[3].value = m_DoneValue + 1;
    signals[3].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkCommandBufferSubmitInfo cbsi = {};
    cbsi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cbsi.commandBuffer = m_Cmd;
    VkSubmitInfo2 si = {};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    si.waitSemaphoreInfoCount = 3;
    si.pWaitSemaphoreInfos = waits;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbsi;
    si.signalSemaphoreInfoCount = 4;
    si.pSignalSemaphoreInfos = signals;

    m_Interop.lockQueue(m_Interop.lockOpaque, m_Interop.graphics.family, 0);
    VkResult vr = m_vkQueueSubmit2(m_Queue, 1, &si, VK_NULL_HANDLE);
    m_Interop.unlockQueue(m_Interop.lockOpaque, m_Interop.graphics.family, 0);

    if (vr != VK_SUCCESS) {
        vkfc->unlock_frame(fc, vkf);
        av_frame_unref(outFrame);
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-PYROWAVE-ZC] vkQueueSubmit2 failed on frame %d: %d", du->frameNumber, (int)vr);
        return DR_OK;
    }

    m_DoneValue++;
    for (int p = 0; p < 3; p++) {
        vkf->sem_value[p]++;
        vkf->layout[p] = VK_IMAGE_LAYOUT_GENERAL;
        vkf->access[p] = (VkAccessFlagBits)VK_ACCESS_SHADER_WRITE_BIT;
        vkf->queue_family[p] = m_Interop.graphics.family;
    }
    vkfc->unlock_frame(fc, vkf);

    const bool is10Bit = (m_VideoFormat & VIDEO_FORMAT_MASK_10BIT) != 0;
    const bool isHdr = is10Bit && LiGetCurrentHostDisplayHdrMode();
    outFrame->colorspace = isHdr ? AVCOL_SPC_BT2020_NCL : AVCOL_SPC_BT709;
    outFrame->color_primaries = isHdr ? AVCOL_PRI_BT2020 : AVCOL_PRI_BT709;
    outFrame->color_trc = isHdr ? AVCOL_TRC_SMPTE2084 : AVCOL_TRC_BT709;

    static std::atomic<bool> s_Logged{false};
    if (!s_Logged.exchange(true)) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-PYROWAVE-ZC] first zero-copy frame %d submitted (no CPU readback)",
                    du->frameNumber);
    }
    return DR_OK;
}

void PyroWaveDecoderBridge::cleanupZeroCopy()
{
    if (m_Interop.device != VK_NULL_HANDLE) {
        // GPU 還沒做完就拆 semaphore／command pool 是 Vulkan 未定義行為；等不到時寧可洩漏到行程結束
        const bool gpuIdle = (m_DoneSem == VK_NULL_HANDLE) || waitPreviousGpuWork();
        if (!gpuIdle) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-PYROWAVE-ZC] GPU work still pending at cleanup; leaking command pool/semaphore");
        }
        else {
            if (m_DoneSem != VK_NULL_HANDLE) {
                m_vkDestroySemaphore(m_Interop.device, m_DoneSem, nullptr);
            }
            if (m_CmdPool != VK_NULL_HANDLE) {
                m_vkDestroyCommandPool(m_Interop.device, m_CmdPool, nullptr);
            }
        }
        m_DoneSem = VK_NULL_HANDLE;
        m_CmdPool = VK_NULL_HANDLE;
        m_Cmd = VK_NULL_HANDLE;
    }
    // AVFrame 各自持有 frames ctx 的參考，這裡只放掉 bridge 的那一份
    av_buffer_unref(&m_FramesCtx);
    m_Queue = VK_NULL_HANDLE;
    m_DoneValue = 0;
    m_PlaneCount = 0;
    m_PlaneFormat = VK_FORMAT_UNDEFINED;
    m_ZeroCopy = false;
}
