#include "qxr_video.h"

#include "qxr_log.h"
#include "viple_vr_bridge.h"

// 建置機先跑 shaders/build_shaders.cmd 產生這兩個標頭。裝置上沒有 glslc。
#include "quad.vert.spv.h"
#include "quad.frag.spv.h"

#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>

#include <time.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace {

struct QxrWait {
    std::mutex mu;
    std::condition_variable cv;
    AImageReader_ImageListener listener{};
};

struct QxrPush {
    float u0;
    float v0;
    float uScale;
    float vScale;
    int32_t linearize;
    int32_t pad0;
    int32_t pad1;
    int32_t pad2;
};

static_assert(sizeof(QxrPush) == 32, "push constant size must match quad.frag");

int64_t monoNs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000ll + ts.tv_nsec;
}

bool isSrgbFormat(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_R8G8B8_SRGB:
    case VK_FORMAT_B8G8R8_SRGB:
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        return true;
    default:
        return false;
    }
}

bool spirvOk(const unsigned char* code, unsigned len, const char* name) {
    if (code == nullptr || len < 4 || (len % 4) != 0) {
        QXR_LOGE("VIDEO", "%s: SPIR-V length %u is not usable. Run shaders/build_shaders.cmd.", name, len);
        return false;
    }
    uint32_t magic = 0;
    std::memcpy(&magic, code, sizeof(magic));
    if (magic != 0x07230203u) {
        QXR_LOGE("VIDEO", "%s: SPIR-V magic 0x%08x. Run shaders/build_shaders.cmd.", name, magic);
        return false;
    }
    return true;
}

VkShaderModule makeShader(VkDevice device, const unsigned char* code, unsigned len, const char* name) {
    if (!spirvOk(code, len, name)) {
        return VK_NULL_HANDLE;
    }
    // xxd 的陣列對齊不足 4，Vulkan 要求 pCode 對齊到 uint32。
    std::vector<uint32_t> aligned((len + 3u) / 4u);
    std::memcpy(aligned.data(), code, len);
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = len;
    ci.pCode = aligned.data();
    VkShaderModule shader = VK_NULL_HANDLE;
    const VkResult result = vkCreateShaderModule(device, &ci, nullptr, &shader);
    if (result != VK_SUCCESS) {
        QXR_LOGE("VIDEO", "vkCreateShaderModule(%s) failed: VkResult=%d", name, static_cast<int>(result));
        return VK_NULL_HANDLE;
    }
    return shader;
}

int pickMemoryType(VkPhysicalDevice physicalDevice, uint32_t bits) {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if (bits & (1u << i)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// externalFormat 規格規定永遠不是 0，連 RGBA 也一樣，不能拿來判斷是不是 YCbCr。
bool ahbUsesYcbcr(const VkAndroidHardwareBufferFormatPropertiesANDROID& props) {
    return props.format == VK_FORMAT_UNDEFINED
        || props.suggestedYcbcrModel != VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY;
}

// 回傳 true 時 *out 是 vkCreateSamplerYcbcrConversion 接受的位置。
// Vulkan 的 format feature：沒有 MIDPOINT_CHROMA_SAMPLES 就不能填 MIDPOINT（否則只能是 COSITED_EVEN）；
// 沒有 COSITED_CHROMA_SAMPLES 就不能填 COSITED_EVEN（否則只能是 MIDPOINT）。
// 兩個 bit 都沒有時，兩種列舉值都會違反 valid usage，這個格式不能用 YCbCr conversion 取樣。
bool legalChromaOffset(VkChromaLocation suggested, VkFormatFeatureFlags features, VkChromaLocation* out) {
    const bool midpoint = (features & VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT) != 0;
    const bool cosited = (features & VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT) != 0;
    if (suggested == VK_CHROMA_LOCATION_MIDPOINT && midpoint) {
        *out = VK_CHROMA_LOCATION_MIDPOINT;
        return true;
    }
    if (suggested == VK_CHROMA_LOCATION_COSITED_EVEN && cosited) {
        *out = VK_CHROMA_LOCATION_COSITED_EVEN;
        return true;
    }
    if (midpoint) {
        *out = VK_CHROMA_LOCATION_MIDPOINT;
        return true;
    }
    if (cosited) {
        *out = VK_CHROMA_LOCATION_COSITED_EVEN;
        return true;
    }
    return false;
}

}  // namespace

QxrVideo::~QxrVideo() {
    destroy();
}

bool QxrVideo::create(const QxrVideoCreateInfo& info, const char** error) {
    auto fail = [&](const char* msg) {
        QXR_LOGE("VIDEO", "%s", msg);
        if (error) {
            *error = msg;
        }
        failReason = msg;
        destroy();
        return false;
    };

    physicalDevice = info.physicalDevice;
    device = info.device;
    queueFamily = info.queueFamily;
    targetFormat = info.targetFormat;
    targetSrgb = isSrgbFormat(info.targetFormat);
    streamWidth = info.streamWidth;
    streamHeight = info.streamHeight;
    stopping.store(0, std::memory_order_release);
    pending.store(0, std::memory_order_release);
    // 同一個物件再 create 時，上一場的到達時刻不能算進這一場。
    imageAvailableNs.store(0, std::memory_order_release);

    if (device == VK_NULL_HANDLE || physicalDevice == VK_NULL_HANDLE) {
        return fail("video create: Vulkan device is null");
    }
    if (streamWidth <= 0 || streamHeight <= 0) {
        return fail("video create: stream size is empty");
    }
    if (targetFormat == VK_FORMAT_UNDEFINED) {
        return fail("video create: target format is undefined");
    }

    auto* wait = new QxrWait();
    waitMu = wait;
    waitCv = &wait->cv;

    getAhbProps = reinterpret_cast<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(
        vkGetDeviceProcAddr(device, "vkGetAndroidHardwareBufferPropertiesANDROID"));
    createYcbcr = reinterpret_cast<PFN_vkCreateSamplerYcbcrConversion>(
        vkGetDeviceProcAddr(device, "vkCreateSamplerYcbcrConversion"));
    if (!createYcbcr) {
        createYcbcr = reinterpret_cast<PFN_vkCreateSamplerYcbcrConversion>(
            vkGetDeviceProcAddr(device, "vkCreateSamplerYcbcrConversionKHR"));
    }
    destroyYcbcr = reinterpret_cast<PFN_vkDestroySamplerYcbcrConversion>(
        vkGetDeviceProcAddr(device, "vkDestroySamplerYcbcrConversion"));
    if (!destroyYcbcr) {
        destroyYcbcr = reinterpret_cast<PFN_vkDestroySamplerYcbcrConversion>(
            vkGetDeviceProcAddr(device, "vkDestroySamplerYcbcrConversionKHR"));
    }
    if (!getAhbProps || !createYcbcr || !destroyYcbcr) {
        return fail("video create: AHB or YCbCr device proc missing");
    }

    VkAttachmentDescription attachment{};
    attachment.format = targetFormat;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    // swapchain 交出來時已是 COLOR_ATTACHMENT_OPTIMAL，交回去也要維持這個 layout。
    attachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference reference{};
    reference.attachment = 0;
    reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;

    VkSubpassDependency dependencies[2]{};
    // 進來的上一手是 color attachment write（swapchain 與測試都把目標留在 COLOR_ATTACHMENT_OPTIMAL）。
    // srcAccess 留 0 時，CLEAR 與單張 in-flight 暫時蓋得住；loadOp 改成 LOAD 或兩張重疊，tiled GPU 會 WAW。
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    // 出去不用 BOTTOM_OF_PIPE：它當 destination 不會擋住後面較早的階段。
    // 同一條 command buffer 裡的 copy 要 TRANSFER read。swapchain 交給 runtime 之後，合成器以 SAMPLED 在 fragment shader 讀。
    // 外部依賴要同時涵蓋這兩種，否則取樣可能讀到還沒寫完的像素。
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT;

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &attachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 2;
    renderPassInfo.pDependencies = dependencies;
    const VkResult passResult = vkCreateRenderPass(device, &renderPassInfo, nullptr, &renderPass);
    if (passResult != VK_SUCCESS) {
        QXR_LOGE("VIDEO", "vkCreateRenderPass failed: VkResult=%d", static_cast<int>(passResult));
        return fail("vkCreateRenderPass failed");
    }

    // PRIVATE 才能拿到 AHardwareBuffer。usage 只標 GPU 取樣，NDK 明文禁止再加 CPU read。
    // 相容表沒有把 PRIVATE+GPU_SAMPLED 列成一列，實機要看這次呼叫的回傳值。
    constexpr int kMaxImages = 4;
    AImageReader* imageReader = nullptr;
    const media_status_t readerStatus = AImageReader_newWithUsage(
        streamWidth, streamHeight, AIMAGE_FORMAT_PRIVATE,
        AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, kMaxImages, &imageReader);
    if (readerStatus != AMEDIA_OK || imageReader == nullptr) {
        QXR_LOGE("VIDEO", "AImageReader_newWithUsage failed: media_status=%d size=%dx%d",
                 static_cast<int>(readerStatus), streamWidth, streamHeight);
        return fail("AImageReader_newWithUsage failed");
    }
    reader = imageReader;

    ANativeWindow* nativeWindow = nullptr;
    const media_status_t windowStatus = AImageReader_getWindow(imageReader, &nativeWindow);
    if (windowStatus != AMEDIA_OK || nativeWindow == nullptr) {
        QXR_LOGE("VIDEO", "AImageReader_getWindow failed: media_status=%d", static_cast<int>(windowStatus));
        return fail("AImageReader_getWindow failed");
    }
    window = nativeWindow;

    wait->listener.context = this;
    wait->listener.onImageAvailable = [](void* ctx, AImageReader*) {
        if (ctx) {
            static_cast<QxrVideo*>(ctx)->onImageAvailable(ctx, nullptr);
        }
    };
    const media_status_t listenStatus = AImageReader_setImageListener(imageReader, &wait->listener);
    if (listenStatus != AMEDIA_OK) {
        QXR_LOGE("VIDEO", "AImageReader_setImageListener failed: media_status=%d", static_cast<int>(listenStatus));
        return fail("AImageReader_setImageListener failed");
    }

    QXR_LOGI("VIDEO", "reader %dx%d maxImages=%d targetFormat=%d srgb=%d",
             streamWidth, streamHeight, kMaxImages, static_cast<int>(targetFormat), targetSrgb ? 1 : 0);
    if (error) {
        *error = nullptr;
    }
    return true;
}

void QxrVideo::destroy() {
    if (waitMu) {
        auto* wait = static_cast<QxrWait*>(waitMu);
        std::lock_guard<std::mutex> lock(wait->mu);
        stopping.store(1, std::memory_order_release);
        wait->cv.notify_all();
    } else {
        stopping.store(1, std::memory_order_release);
    }
    if (reader) {
        AImageReader_setImageListener(static_cast<AImageReader*>(reader), nullptr);
    }
    if (held) {
        AImage_delete(static_cast<AImage*>(held));
        held = nullptr;
    }
    if (latched) {
        AImage_delete(static_cast<AImage*>(latched));
        latched = nullptr;
    }
    currentView = VK_NULL_HANDLE;
    currentImage = VK_NULL_HANDLE;

    if (device) {
        flushCache();
        for (TargetFb& target : targets) {
            if (target.framebuffer) {
                vkDestroyFramebuffer(device, target.framebuffer, nullptr);
            }
            target = {};
        }
        destroyPipeline();
        if (renderPass) {
            vkDestroyRenderPass(device, renderPass, nullptr);
            renderPass = VK_NULL_HANDLE;
        }
        destroySampler();
    }

    // getWindow 拿到的 ANativeWindow 由 AImageReader 持有，沒有多加 reference。
    // 這裡再 release，AImageReader_delete 解構同一顆 Surface 時會放第二次。
    window = nullptr;
    if (reader) {
        AImageReader_delete(static_cast<AImageReader*>(reader));
        reader = nullptr;
    }
    if (waitMu) {
        delete static_cast<QxrWait*>(waitMu);
        waitMu = nullptr;
        waitCv = nullptr;
    }
    formatReady = false;
    // 呼叫端接著會拆 VkDevice。解構子還會再進來一次，留著失效的 handle 會在第二次碰到已拆的裝置。
    device = VK_NULL_HANDLE;
    physicalDevice = VK_NULL_HANDLE;
    // 同一個物件會再 create。旗標與計數留著的話，第一次之後就不再記 first image，10 秒窗也會跨過上一次。
    loggedFirstImage = false;
    loggedMaxImages = false;
    loggedAcquireError = false;
    loggedGetId = false;
    loggedBadCrop = false;
    loggedFbFull = false;
    loggedNoChroma = false;
    statReceived = 0;
    statAdopted = 0;
    statDiscarded = 0;
    statHit = 0;
    statMiss = 0;
    snapReceived = 0;
    snapAdopted = 0;
    snapDiscarded = 0;
    snapHit = 0;
    snapMiss = 0;
    statWindowStartNs = 0;
    imageAvailableNs.store(0, std::memory_order_release);
}

bool QxrVideo::waitForImage(int timeoutMs) {
    if (!waitMu || !waitCv) {
        return false;
    }
    if (timeoutMs < 0) {
        timeoutMs = 0;
    }
    auto* wait = static_cast<QxrWait*>(waitMu);
    std::unique_lock<std::mutex> lock(wait->mu);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    // predicate 在鎖內重讀。signalImage 也在這把鎖裡寫 pending 再 notify，喚醒不會落在檢查與等待之間。
    wait->cv.wait_until(lock, deadline, [this] {
        return pending.load(std::memory_order_acquire) != 0 || stopping.load(std::memory_order_acquire) != 0;
    });
    const bool ready = pending.load(std::memory_order_acquire) != 0;
    pending.store(0, std::memory_order_release);
    return ready;
}

void QxrVideo::onImageAvailable(void* context, void*) {
    auto* self = static_cast<QxrVideo*>(context);
    if (!self || self->stopping.load(std::memory_order_acquire)) {
        return;
    }
    // 不在回呼裡取圖。這條執行緒一旦阻塞就會卡住解碼器，鎖只在 signalImage 裡短暫持有。
    // 到達時刻給投影的速率判斷。影格執行緒可能比這個 store 更早採用這張，下一格再對上。
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const uint64_t ns = static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
    self->imageAvailableNs.store(ns, std::memory_order_release);
    self->statListener.fetch_add(1, std::memory_order_relaxed);
    self->signalImage();
}

void QxrVideo::clearTargetFramebuffers() {
    if (device == VK_NULL_HANDLE) {
        return;
    }
    for (TargetFb& target : targets) {
        if (target.framebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(device, target.framebuffer, nullptr);
        }
        target = {};
    }
}

void QxrVideo::signalImage() {
    if (!waitMu) {
        pending.store(1, std::memory_order_release);
        return;
    }
    auto* wait = static_cast<QxrWait*>(waitMu);
    std::lock_guard<std::mutex> lock(wait->mu);
    if (stopping.load(std::memory_order_acquire)) {
        return;
    }
    pending.store(1, std::memory_order_release);
    wait->cv.notify_one();
}

void* QxrVideo::pullNewest() {
    // acquireLatest 不告訴我們丟掉幾張。把佇列排空，只留最後一張，順便接上 drain 留下的那張。
    AImage* newest = static_cast<AImage*>(held);
    held = nullptr;
    if (!reader) {
        return newest;
    }
    for (;;) {
        AImage* image = nullptr;
        const media_status_t status = AImageReader_acquireNextImage(static_cast<AImageReader*>(reader), &image);
        if (status == AMEDIA_OK && image) {
            if (newest) {
                AImage_delete(newest);
                ++statDiscarded;
            }
            newest = image;
            ++statReceived;
            continue;
        }
        if (status == AMEDIA_IMGREADER_NO_BUFFER_AVAILABLE) {
            break;
        }
        if (status == AMEDIA_IMGREADER_MAX_IMAGES_ACQUIRED) {
            if (!loggedMaxImages) {
                loggedMaxImages = true;
                QXR_LOGW("VIDEO", "AImageReader_acquireNextImage: MAX_IMAGES_ACQUIRED, keeping the newest held image");
            }
            break;
        }
        if (!loggedAcquireError) {
            loggedAcquireError = true;
            QXR_LOGW("VIDEO", "AImageReader_acquireNextImage failed: media_status=%d", static_cast<int>(status));
        }
        break;
    }
    return newest;
}

void QxrVideo::drainReader() {
    if (!reader || stopping.load(std::memory_order_acquire)) {
        return;
    }
    // 不匯入、不畫，也不淘汰快取。session 還沒 begin、或不該畫的時候進來，
    // 上一張的 fence 可能還沒到，快取裡的 VkImage 不能拆。
    held = pullNewest();
}

bool QxrVideo::consumeLatest() {
    if (!reader || stopping.load(std::memory_order_acquire)) {
        return currentView != VK_NULL_HANDLE;
    }

    AImage* newest = static_cast<AImage*>(pullNewest());
    if (!newest) {
        pending.store(0, std::memory_order_release);
        return currentView != VK_NULL_HANDLE;
    }
    if (!importLatched(newest)) {
        AImage_delete(newest);
        ++statDiscarded;
        pending.store(0, std::memory_order_release);
        return currentView != VK_NULL_HANDLE;
    }
    if (latched) {
        // 這張已經採納過。換掉它是正常更新，不是丟掉還沒顯示的幀。
        AImage_delete(static_cast<AImage*>(latched));
    }
    latched = newest;
    ++statAdopted;
    pending.store(0, std::memory_order_release);
    return currentView != VK_NULL_HANDLE;
}

bool QxrVideo::importLatched(void* image) {
    auto* src = static_cast<AImage*>(image);
    AHardwareBuffer* buffer = nullptr;
    const media_status_t bufferStatus = AImage_getHardwareBuffer(src, &buffer);
    if (bufferStatus != AMEDIA_OK || buffer == nullptr) {
        QXR_LOGE("VIDEO", "AImage_getHardwareBuffer failed: media_status=%d", static_cast<int>(bufferStatus));
        return false;
    }

    AHardwareBuffer_Desc desc{};
    AHardwareBuffer_describe(buffer, &desc);
    if (desc.width == 0 || desc.height == 0) {
        QXR_LOGE("VIDEO", "AHardwareBuffer_describe returned empty size");
        return false;
    }

    VkAndroidHardwareBufferFormatPropertiesANDROID formatProps{};
    formatProps.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID;
    VkAndroidHardwareBufferPropertiesANDROID properties{};
    properties.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
    properties.pNext = &formatProps;
    const VkResult propResult = getAhbProps(device, buffer, &properties);
    if (propResult != VK_SUCCESS) {
        QXR_LOGE("VIDEO", "vkGetAndroidHardwareBufferPropertiesANDROID failed: VkResult=%d", static_cast<int>(propResult));
        return false;
    }
    if (!ensureFormat(&formatProps)) {
        return false;
    }

    bool hasId = false;
    uint64_t id = 0;
    if (__builtin_available(android 31, *)) {
        const int idResult = AHardwareBuffer_getId(buffer, &id);
        hasId = idResult == 0;
        if (!loggedGetId) {
            loggedGetId = true;
            QXR_LOGI("VIDEO", "AHardwareBuffer_getId %s (%d)", hasId ? "ok" : "failed", idResult);
        }
    } else if (!loggedGetId) {
        loggedGetId = true;
        QXR_LOGI("VIDEO", "AHardwareBuffer_getId unavailable; cache key is pointer+size");
    }

    if (CacheEntry* hit = findCache(hasId, id, reinterpret_cast<uintptr_t>(buffer), desc.width, desc.height)) {
        ++statHit;
        hit->lastUsed = ++cacheTick;
        currentView = hit->view;
        currentImage = hit->image;
    } else {
        ++statMiss;
        CacheEntry* slot = takeCacheSlot();
        if (!slot) {
            QXR_LOGE("VIDEO", "AHB cache has no slot");
            return false;
        }
        *slot = {};
        slot->occupied = true;
        slot->hasId = hasId;
        slot->id = id;
        slot->ptr = reinterpret_cast<uintptr_t>(buffer);
        slot->width = desc.width;
        slot->height = desc.height;
        slot->externalFormat = formatProps.externalFormat;
        slot->lastUsed = ++cacheTick;
        AHardwareBuffer_acquire(buffer);
        slot->buffer = buffer;

        const bool ycbcr = ahbUsesYcbcr(formatProps);
        VkExternalFormatANDROID externalFormat{};
        externalFormat.sType = VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID;
        externalFormat.externalFormat = formatProps.externalFormat;
        VkExternalMemoryImageCreateInfo externalImage{};
        externalImage.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        externalImage.pNext = ycbcr ? &externalFormat : nullptr;
        externalImage.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;

        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.pNext = &externalImage;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.format = ycbcr ? VK_FORMAT_UNDEFINED : formatProps.format;
        imageInfo.extent = VkExtent3D{desc.width, desc.height, 1};
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkResult result = vkCreateImage(device, &imageInfo, nullptr, &slot->image);
        if (result != VK_SUCCESS) {
            QXR_LOGE("VIDEO", "vkCreateImage(AHB) failed: VkResult=%d", static_cast<int>(result));
            destroyEntry(slot);
            return false;
        }

        const int memoryType = pickMemoryType(physicalDevice, properties.memoryTypeBits);
        if (memoryType < 0) {
            QXR_LOGE("VIDEO", "no memory type for AHB bits=0x%x", properties.memoryTypeBits);
            destroyEntry(slot);
            return false;
        }
        VkImportAndroidHardwareBufferInfoANDROID importInfo{};
        importInfo.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
        importInfo.buffer = buffer;
        VkMemoryDedicatedAllocateInfo dedicated{};
        dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        dedicated.pNext = &importInfo;
        dedicated.image = slot->image;
        VkMemoryAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate.pNext = &dedicated;
        allocate.allocationSize = properties.allocationSize;
        allocate.memoryTypeIndex = static_cast<uint32_t>(memoryType);
        result = vkAllocateMemory(device, &allocate, nullptr, &slot->memory);
        if (result != VK_SUCCESS) {
            QXR_LOGE("VIDEO", "vkAllocateMemory(AHB) failed: VkResult=%d", static_cast<int>(result));
            destroyEntry(slot);
            return false;
        }
        result = vkBindImageMemory(device, slot->image, slot->memory, 0);
        if (result != VK_SUCCESS) {
            QXR_LOGE("VIDEO", "vkBindImageMemory(AHB) failed: VkResult=%d", static_cast<int>(result));
            destroyEntry(slot);
            return false;
        }

        VkSamplerYcbcrConversionInfo conversionInfo{};
        conversionInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
        conversionInfo.conversion = conversion;
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.pNext = ycbcr ? &conversionInfo : nullptr;
        viewInfo.image = slot->image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = ycbcr ? VK_FORMAT_UNDEFINED : formatProps.format;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.layerCount = 1;
        result = vkCreateImageView(device, &viewInfo, nullptr, &slot->view);
        if (result != VK_SUCCESS) {
            QXR_LOGE("VIDEO", "vkCreateImageView(AHB) failed: VkResult=%d", static_cast<int>(result));
            destroyEntry(slot);
            return false;
        }
        currentView = slot->view;
        currentImage = slot->image;
    }

    int32_t imageWidth = 0;
    int32_t imageHeight = 0;
    AImage_getWidth(src, &imageWidth);
    AImage_getHeight(src, &imageHeight);
    const float bufW = static_cast<float>(desc.width);
    const float bufH = static_cast<float>(desc.height);
    AImageCropRect crop{};
    const media_status_t cropStatus = AImage_getCropRect(src, &crop);
    const int32_t cropW = crop.right - crop.left;
    const int32_t cropH = crop.bottom - crop.top;
    float left = 0.f;
    float top = 0.f;
    float right = bufW;
    float bottom = bufH;
    // getWidth 是對齊後的緩衝區大小，有效畫面在 crop。寬或高不是正數就沒有可裁的範圍。
    bool cropOk = cropStatus == AMEDIA_OK && cropW > 0 && cropH > 0;
    if (cropOk) {
        left = static_cast<float>(crop.left);
        top = static_cast<float>(crop.top);
        right = static_cast<float>(crop.right);
        bottom = static_cast<float>(crop.bottom);
        if (left < 0.f) {
            left = 0.f;
        }
        if (top < 0.f) {
            top = 0.f;
        }
        if (right > bufW) {
            right = bufW;
        }
        if (bottom > bufH) {
            bottom = bufH;
        }
        if (right - left <= 0.f || bottom - top <= 0.f) {
            cropOk = false;
        }
    }
    if (!cropOk) {
        if (!loggedBadCrop) {
            loggedBadCrop = true;
            QXR_LOGW("VIDEO", "AImage crop invalid status=%d rect=%d,%d,%d,%d; using full buffer %ux%u",
                     static_cast<int>(cropStatus), crop.left, crop.top, crop.right, crop.bottom, desc.width, desc.height);
        }
        left = 0.f;
        top = 0.f;
        right = bufW;
        bottom = bufH;
    }
    // 雙線性會往樣本點外取半個 texel。收到 crop 內側，才不會混到對齊補邊。
    if (right - left >= 1.f) {
        left += 0.5f;
        right -= 0.5f;
    }
    if (bottom - top >= 1.f) {
        top += 0.5f;
        bottom -= 0.5f;
    }
    uv.u0 = left / bufW;
    uv.v0 = top / bufH;
    uv.uScale = (right - left) / bufW;
    uv.vScale = (bottom - top) / bufH;

    int64_t timestampNs = -1;
    if (AImage_getTimestamp(src, &timestampNs) != AMEDIA_OK) {
        timestampNs = -1;
    }
    if (timestampNs >= 0) {
        latchedFrame = (timestampNs / 1000) / VIPLE_VR_BRIDGE_PTS_STEP_US;
    } else {
        latchedFrame = -1;
    }
    if (!loggedFirstImage) {
        loggedFirstImage = true;
        QXR_LOGI("VIDEO", "first image %dx%d buffer=%ux%u crop=%d,%d,%d,%d uv=(%.4f,%.4f,%.4f,%.4f) vkFormat=%d external=0x%llx frame=%lld",
                 imageWidth, imageHeight, desc.width, desc.height, crop.left, crop.top, crop.right, crop.bottom,
                 static_cast<double>(uv.u0), static_cast<double>(uv.v0), static_cast<double>(uv.uScale),
                 static_cast<double>(uv.vScale), static_cast<int>(formatProps.format),
                 static_cast<unsigned long long>(formatProps.externalFormat),
                 static_cast<long long>(latchedFrame));
    }
    return true;
}

bool QxrVideo::ensureFormat(const void* formatPropsVoid) {
    const auto* formatProps = static_cast<const VkAndroidHardwareBufferFormatPropertiesANDROID*>(formatPropsVoid);
    const bool ycbcr = ahbUsesYcbcr(*formatProps);
    const VkFormat vkFormat = ycbcr ? VK_FORMAT_UNDEFINED : formatProps->format;
    if (formatReady && ycbcr == useYcbcr && formatProps->externalFormat == boundExternalFormat && vkFormat == boundVkFormat
        && sampler != VK_NULL_HANDLE && pipeline != VK_NULL_HANDLE) {
        return true;
    }
    if (!ycbcr && vkFormat == VK_FORMAT_UNDEFINED) {
        QXR_LOGE("VIDEO", "AHB has no external format and no VkFormat");
        return false;
    }
    if (formatReady) {
        QXR_LOGW("VIDEO", "external format changed 0x%llx -> 0x%llx; rebuilding conversion and pipeline",
                 static_cast<unsigned long long>(boundExternalFormat),
                 static_cast<unsigned long long>(formatProps->externalFormat));
    }
    // 呼叫端已保證上一張指令做完，這裡才能拆掉還掛在快取裡的 VkImage。
    currentView = VK_NULL_HANDLE;
    currentImage = VK_NULL_HANDLE;
    flushCache();
    destroyPipeline();
    destroySampler();
    formatReady = false;
    const bool samplerOk = ycbcr ? createYcbcrSampler(formatProps) : createPlainSampler();
    if (!samplerOk || !createPipeline()) {
        destroyPipeline();
        destroySampler();
        return false;
    }
    useYcbcr = ycbcr;
    boundExternalFormat = formatProps->externalFormat;
    boundVkFormat = vkFormat;
    formatReady = true;
    return true;
}

bool QxrVideo::createYcbcrSampler(const void* formatPropsVoid) {
    const auto* formatProps = static_cast<const VkAndroidHardwareBufferFormatPropertiesANDROID*>(formatPropsVoid);
    VkExternalFormatANDROID externalFormat{};
    externalFormat.sType = VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID;
    externalFormat.externalFormat = formatProps->externalFormat;

    VkSamplerYcbcrConversionCreateInfo conversionInfo{};
    conversionInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO;
    conversionInfo.pNext = &externalFormat;
    conversionInfo.format = VK_FORMAT_UNDEFINED;
    conversionInfo.ycbcrModel = formatProps->suggestedYcbcrModel;
    conversionInfo.ycbcrRange = formatProps->suggestedYcbcrRange;
    conversionInfo.components = formatProps->samplerYcbcrConversionComponents;
    const VkFormatFeatureFlags features = formatProps->formatFeatures;
    // 沒有 LINEAR_FILTER bit 還填 LINEAR 是未定義行為，驅動不必回錯誤。
    const bool linearChroma = (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT) != 0;
    const VkFilter chromaFilter = linearChroma ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    // 沒有 SEPARATE_RECONSTRUCTION 時，chroma、min、mag 三者必須相同。
    const bool separate = (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_SEPARATE_RECONSTRUCTION_FILTER_BIT) != 0;
    VkFilter lumaFilter = chromaFilter;
    if (separate) {
        const bool linearSample = (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
        lumaFilter = linearSample ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    }
    VkChromaLocation xOffset = VK_CHROMA_LOCATION_COSITED_EVEN;
    VkChromaLocation yOffset = VK_CHROMA_LOCATION_COSITED_EVEN;
    if (!legalChromaOffset(formatProps->suggestedXChromaOffset, features, &xOffset)
        || !legalChromaOffset(formatProps->suggestedYChromaOffset, features, &yOffset)) {
        // 兩種 chroma 位置都會被拒。送進去只會讓 vkCreateSamplerYcbcrConversion 失敗，這裡直接讓 ensureFormat 失敗。
        if (!loggedNoChroma) {
            loggedNoChroma = true;
            QXR_LOGE("VIDEO",
                     "ensureFormat: formatFeatures=0x%x has neither MIDPOINT_CHROMA_SAMPLES nor COSITED_CHROMA_SAMPLES "
                     "(suggested x=%d y=%d external=0x%llx). Vulkan accepts no chroma location, so this format cannot be "
                     "sampled with a YCbCr conversion",
                     static_cast<unsigned>(features), static_cast<int>(formatProps->suggestedXChromaOffset),
                     static_cast<int>(formatProps->suggestedYChromaOffset),
                     static_cast<unsigned long long>(formatProps->externalFormat));
        }
        return false;
    }
    conversionInfo.xChromaOffset = xOffset;
    conversionInfo.yChromaOffset = yOffset;
    conversionInfo.chromaFilter = chromaFilter;
    conversionInfo.forceExplicitReconstruction = VK_FALSE;

    VkResult result = createYcbcr(device, &conversionInfo, nullptr, &conversion);
    if (result != VK_SUCCESS) {
        QXR_LOGE("VIDEO", "vkCreateSamplerYcbcrConversion failed: VkResult=%d external=0x%llx",
                 static_cast<int>(result), static_cast<unsigned long long>(formatProps->externalFormat));
        conversion = VK_NULL_HANDLE;
        return false;
    }

    VkSamplerYcbcrConversionInfo samplerConversion{};
    samplerConversion.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO;
    samplerConversion.conversion = conversion;
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.pNext = &samplerConversion;
    samplerInfo.magFilter = lumaFilter;
    samplerInfo.minFilter = lumaFilter;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 0.f;
    result = vkCreateSampler(device, &samplerInfo, nullptr, &sampler);
    if (result != VK_SUCCESS) {
        QXR_LOGE("VIDEO", "vkCreateSampler(YCbCr) failed: VkResult=%d", static_cast<int>(result));
        destroyYcbcr(device, conversion, nullptr);
        conversion = VK_NULL_HANDLE;
        return false;
    }
    QXR_LOGI("VIDEO",
             "YCbCr model=%d range=%d xChroma=%d yChroma=%d chromaFilter=%d minFilter=%d magFilter=%d separate=%d features=0x%x external=0x%llx",
             static_cast<int>(conversionInfo.ycbcrModel),
             static_cast<int>(conversionInfo.ycbcrRange),
             static_cast<int>(conversionInfo.xChromaOffset),
             static_cast<int>(conversionInfo.yChromaOffset),
             static_cast<int>(conversionInfo.chromaFilter),
             static_cast<int>(lumaFilter),
             static_cast<int>(lumaFilter),
             separate ? 1 : 0,
             static_cast<unsigned>(features),
             static_cast<unsigned long long>(formatProps->externalFormat));
    return true;
}

bool QxrVideo::createPlainSampler() {
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    const VkResult result = vkCreateSampler(device, &samplerInfo, nullptr, &sampler);
    if (result != VK_SUCCESS) {
        QXR_LOGE("VIDEO", "vkCreateSampler failed: VkResult=%d", static_cast<int>(result));
        return false;
    }
    QXR_LOGI("VIDEO", "plain RGBA sampler");
    return true;
}

bool QxrVideo::createPipeline() {
    if (!sampler || !renderPass) {
        QXR_LOGE("VIDEO", "createPipeline: sampler or render pass missing");
        return false;
    }
    VkShaderModule vert = makeShader(device, quad_vert_spv, quad_vert_spv_len, "quad.vert.spv");
    VkShaderModule frag = makeShader(device, quad_frag_spv, quad_frag_spv_len, "quad.frag.spv");
    if (!vert || !frag) {
        if (vert) vkDestroyShaderModule(device, vert, nullptr);
        if (frag) vkDestroyShaderModule(device, frag, nullptr);
        return false;
    }

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    binding.pImmutableSamplers = &sampler;
    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    VkResult result = vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &setLayout);
    if (result != VK_SUCCESS) {
        QXR_LOGE("VIDEO", "vkCreateDescriptorSetLayout failed: VkResult=%d", static_cast<int>(result));
        vkDestroyShaderModule(device, vert, nullptr);
        vkDestroyShaderModule(device, frag, nullptr);
        return false;
    }

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(QxrPush);
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &setLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;
    result = vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout);
    if (result != VK_SUCCESS) {
        QXR_LOGE("VIDEO", "vkCreatePipelineLayout failed: VkResult=%d", static_cast<int>(result));
        vkDestroyShaderModule(device, vert, nullptr);
        vkDestroyShaderModule(device, frag, nullptr);
        return false;
    }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vertex{};
    vertex.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT
        | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;
    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertex;
    pipelineInfo.pInputAssemblyState = &assembly;
    pipelineInfo.pViewportState = &viewport;
    pipelineInfo.pRasterizationState = &raster;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pColorBlendState = &blend;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = pipelineLayout;
    pipelineInfo.renderPass = renderPass;
    result = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);
    vkDestroyShaderModule(device, vert, nullptr);
    vkDestroyShaderModule(device, frag, nullptr);
    if (result != VK_SUCCESS) {
        QXR_LOGE("VIDEO", "vkCreateGraphicsPipelines failed: VkResult=%d", static_cast<int>(result));
        return false;
    }

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 1;
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    result = vkCreateDescriptorPool(device, &poolInfo, nullptr, &descPool);
    if (result != VK_SUCCESS) {
        QXR_LOGE("VIDEO", "vkCreateDescriptorPool failed: VkResult=%d", static_cast<int>(result));
        return false;
    }
    VkDescriptorSetAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = descPool;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &setLayout;
    result = vkAllocateDescriptorSets(device, &allocate, &descSet);
    if (result != VK_SUCCESS) {
        QXR_LOGE("VIDEO", "vkAllocateDescriptorSets failed: VkResult=%d", static_cast<int>(result));
        return false;
    }
    return true;
}

void QxrVideo::destroyPipeline() {
    if (!device) {
        return;
    }
    if (descPool) {
        vkDestroyDescriptorPool(device, descPool, nullptr);
        descPool = VK_NULL_HANDLE;
        descSet = VK_NULL_HANDLE;
    }
    if (pipeline) {
        vkDestroyPipeline(device, pipeline, nullptr);
        pipeline = VK_NULL_HANDLE;
    }
    if (pipelineLayout) {
        vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        pipelineLayout = VK_NULL_HANDLE;
    }
    if (setLayout) {
        vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
        setLayout = VK_NULL_HANDLE;
    }
}

void QxrVideo::destroySampler() {
    if (!device) {
        return;
    }
    if (sampler) {
        vkDestroySampler(device, sampler, nullptr);
        sampler = VK_NULL_HANDLE;
    }
    if (conversion && destroyYcbcr) {
        destroyYcbcr(device, conversion, nullptr);
    }
    conversion = VK_NULL_HANDLE;
}

void QxrVideo::flushCache() {
    for (CacheEntry& entry : cache) {
        if (entry.occupied) {
            destroyEntry(&entry);
        }
    }
}

void QxrVideo::destroyEntry(CacheEntry* entry) {
    if (!entry) {
        return;
    }
    if (device) {
        if (entry->view) {
            vkDestroyImageView(device, entry->view, nullptr);
        }
        if (entry->image) {
            vkDestroyImage(device, entry->image, nullptr);
        }
        if (entry->memory) {
            vkFreeMemory(device, entry->memory, nullptr);
        }
    }
    if (entry->buffer) {
        AHardwareBuffer_release(static_cast<AHardwareBuffer*>(entry->buffer));
    }
    *entry = {};
}

QxrVideo::CacheEntry* QxrVideo::findCache(bool hasId, uint64_t id, uintptr_t ptr, uint32_t width, uint32_t height) {
    for (CacheEntry& entry : cache) {
        if (!entry.occupied) {
            continue;
        }
        if (hasId && entry.hasId && entry.id == id) {
            return &entry;
        }
        if (!hasId && !entry.hasId && entry.ptr == ptr && entry.width == width && entry.height == height) {
            return &entry;
        }
    }
    return nullptr;
}

QxrVideo::CacheEntry* QxrVideo::takeCacheSlot() {
    for (CacheEntry& entry : cache) {
        if (!entry.occupied) {
            return &entry;
        }
    }
    CacheEntry* oldest = nullptr;
    for (CacheEntry& entry : cache) {
        if (entry.view == currentView) {
            continue;
        }
        if (!oldest || entry.lastUsed < oldest->lastUsed) {
            oldest = &entry;
        }
    }
    if (!oldest) {
        return nullptr;
    }
    destroyEntry(oldest);
    return oldest;
}

VkFramebuffer QxrVideo::framebufferFor(VkImageView view, uint32_t width, uint32_t height) {
    // 跟 VkImage 快取同一條前提：呼叫端同一時間只讓一張 command buffer 在飛，等過 fence 才再呼叫 renderTo。
    // 淘汰時沒有還沒做完的指令在用這些 framebuffer。
    // 容量 kTargetCap 必須大於呼叫端同時在用的目標數（2D quad 與 projection 的 swapchain 張數、測試的那一張離屏）。
    // 達不到時新目標會拆最久沒用的那張，那張可能還在用；這種情況只記一次。
    if (!view || !renderPass || width == 0 || height == 0) {
        return VK_NULL_HANDLE;
    }
    TargetFb* empty = nullptr;
    TargetFb* oldest = &targets[0];
    for (TargetFb& target : targets) {
        if (target.view == view && target.width == width && target.height == height && target.framebuffer) {
            target.lastUsed = ++cacheTick;
            return target.framebuffer;
        }
        if (!target.framebuffer && !empty) {
            empty = &target;
        }
        if (target.lastUsed < oldest->lastUsed) {
            oldest = &target;
        }
    }
    TargetFb* slot = empty ? empty : oldest;
    if (!empty && !loggedFbFull) {
        loggedFbFull = true;
        QXR_LOGW("VIDEO",
                 "framebuffer cache full (cap=%d); evicting the least recently used target. simultaneous in-use targets "
                 "must stay below this cap, and the previous fence must already have been waited",
                 kTargetCap);
    }
    if (slot->framebuffer) {
        vkDestroyFramebuffer(device, slot->framebuffer, nullptr);
        slot->framebuffer = VK_NULL_HANDLE;
    }
    VkFramebufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    info.renderPass = renderPass;
    info.attachmentCount = 1;
    info.pAttachments = &view;
    info.width = width;
    info.height = height;
    info.layers = 1;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    const VkResult result = vkCreateFramebuffer(device, &info, nullptr, &framebuffer);
    if (result != VK_SUCCESS) {
        QXR_LOGE("VIDEO", "vkCreateFramebuffer failed: VkResult=%d", static_cast<int>(result));
        *slot = {};
        return VK_NULL_HANDLE;
    }
    slot->view = view;
    slot->framebuffer = framebuffer;
    slot->width = width;
    slot->height = height;
    slot->lastUsed = ++cacheTick;
    return framebuffer;
}

void QxrVideo::recordQueueTransfer(VkCommandBuffer cmd, VkImage image, bool toDevice, VkImageLayout sampleLayout) {
    // 外部格式（YCbCr 這條路把 format 設成 UNDEFINED）的真實排列不是任何一個 VkImageLayout。
    // 規格要求這種影像做 queue family ownership transfer 時 oldLayout 與 newLayout 都填 GENERAL。
    // UNDEFINED 代表內容可以丟掉，已寫好的畫面（含 UBWC 標頭）不保證還在。
    // 有具體 VkFormat 的圖才轉進取樣 layout；release 回到 GENERAL，下一次 acquire 的 oldLayout 才接得上。
    // acquire 與 release 寫在同一次 submit：fence 等到之後才會換下一張或淘汰快取，那時所有權已經還給 foreign。
    const bool externalFormat = useYcbcr;
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    if (toDevice) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
        barrier.dstQueueFamilyIndex = queueFamily;
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout = externalFormat ? VK_IMAGE_LAYOUT_GENERAL : sampleLayout;
        srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else {
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = 0;
        barrier.srcQueueFamilyIndex = queueFamily;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
        barrier.oldLayout = externalFormat ? VK_IMAGE_LAYOUT_GENERAL : sampleLayout;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        srcStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dstStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    }
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

bool QxrVideo::renderTo(VkCommandBuffer cmd, VkImageView target, uint32_t width, uint32_t height, QxrUvRect src) {
    // 呼叫端同一時間只讓一張 command buffer 在飛，等過 fence 才錄下一條。
    // 所以更新這個 descriptor set 時，沒有還沒做完的 command buffer 在讀同一組。
    // 有的 GPU 在 vkCmdBeginRenderPass 就抓住 descriptor，寫在 begin 之後會畫到上一張或全黑。
    if (!cmd || !target || !renderPass || width == 0 || height == 0) {
        return false;
    }
    const VkFramebuffer framebuffer = framebufferFor(target, width, height);
    if (!framebuffer) {
        return false;
    }

    const bool canDraw = currentView && currentImage && pipeline && descSet && sampler;
    // 外部格式取樣時 layout 只能是 GENERAL，descriptor 要跟 barrier 的 newLayout 一致。
    const VkImageLayout sampleLayout = useYcbcr ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (canDraw) {
        recordQueueTransfer(cmd, currentImage, true, sampleLayout);
        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageView = currentView;
        imageInfo.imageLayout = sampleLayout;
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = descSet;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }

    VkClearValue clear{};
    clear.color.float32[0] = 0.07f;
    clear.color.float32[1] = 0.07f;
    clear.color.float32[2] = 0.08f;
    clear.color.float32[3] = 1.f;
    VkRenderPassBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass = renderPass;
    begin.framebuffer = framebuffer;
    begin.renderArea.extent = VkExtent2D{width, height};
    begin.clearValueCount = 1;
    begin.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);

    if (canDraw) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &descSet, 0, nullptr);
        VkViewport viewport{};
        viewport.width = static_cast<float>(width);
        viewport.height = static_cast<float>(height);
        viewport.minDepth = 0.f;
        viewport.maxDepth = 1.f;
        VkRect2D scissor{};
        scissor.extent = VkExtent2D{width, height};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        QxrPush push{};
        push.u0 = src.u0;
        push.v0 = src.v0;
        push.uScale = src.uScale;
        push.vScale = src.vScale;
        push.linearize = targetSrgb ? 1 : 0;
        vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    vkCmdEndRenderPass(cmd);
    if (canDraw) {
        recordQueueTransfer(cmd, currentImage, false, sampleLayout);
    }
    return true;
}

void QxrVideo::pumpStats() {
    const int64_t now = monoNs();
    if (statWindowStartNs == 0) {
        statWindowStartNs = now;
        snapReceived = statReceived;
        snapAdopted = statAdopted;
        snapDiscarded = statDiscarded;
        snapHit = statHit;
        snapMiss = statMiss;
        return;
    }
    if (now - statWindowStartNs < 10000000000ll) {
        return;
    }
    logStats();
    snapReceived = statReceived;
    snapAdopted = statAdopted;
    snapDiscarded = statDiscarded;
    snapHit = statHit;
    snapMiss = statMiss;
    statWindowStartNs = now;
}

void QxrVideo::logStats() {
    QXR_LOGI("VIDEO", "listenerTotal=%llu", static_cast<unsigned long long>(statListener.load(std::memory_order_relaxed)));
    QXR_LOGI("VIDEO", "received=%llu adopted=%llu discarded=%llu cacheHit=%llu cacheMiss=%llu format=%s (10s)",
             static_cast<unsigned long long>(statReceived - snapReceived),
             static_cast<unsigned long long>(statAdopted - snapAdopted),
             static_cast<unsigned long long>(statDiscarded - snapDiscarded),
             static_cast<unsigned long long>(statHit - snapHit),
             static_cast<unsigned long long>(statMiss - snapMiss),
             targetSrgb ? "SRGB" : "UNORM");
}
