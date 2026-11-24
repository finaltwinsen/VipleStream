#include "qxr_vk_test.h"

#include "qxr_log.h"
#include "qxr_video.h"

#include <android/native_window.h>
#include <pthread.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr VkDeviceSize kFenceWaitNs = 1000000000ull;

bool vkOk(VkResult result, const char* fn) {
    if (result == VK_SUCCESS) {
        return true;
    }
    QXR_LOGE("VTEST", "%s failed: VkResult=%d", fn, static_cast<int>(result));
    return false;
}

const char* formatName(VkFormat format) {
    if (format == VK_FORMAT_R8G8B8A8_UNORM) {
        return "UNORM";
    }
    if (format == VK_FORMAT_R8G8B8A8_SRGB) {
        return "SRGB";
    }
    return "OTHER";
}

// §QXR-VIDEO-TEST：用一半是為了少讀回，也讓 renderTo 走縮放。再向上取偶數，奇數寬的列長才不會踩到驅動對齊。
uint32_t halfEvenUp(int full) {
    if (full <= 0) {
        return 0;
    }
    int half = (full + 1) / 2;
    if (half < 2) {
        half = 2;
    }
    if (half & 1) {
        ++half;
    }
    return static_cast<uint32_t>(half);
}

uint32_t roundUp(uint32_t value, uint32_t align) {
    if (align <= 1) {
        return value;
    }
    const uint32_t rem = value % align;
    if (rem == 0) {
        return value;
    }
    return value + (align - rem);
}

int findMemory(VkPhysicalDevice physical, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties props{};
    vkGetPhysicalDeviceMemoryProperties(physical, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) == 0) {
            continue;
        }
        if ((props.memoryTypes[i].propertyFlags & want) == want) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool extListed(const std::vector<VkExtensionProperties>& exts, const char* name) {
    for (const VkExtensionProperties& ext : exts) {
        if (std::strcmp(ext.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

double percentile(const std::vector<double>& src, int pct) {
    if (src.empty()) {
        return 0;
    }
    std::vector<double> values = src;
    std::sort(values.begin(), values.end());
    size_t idx = (values.size() * static_cast<size_t>(pct) + 99u) / 100u;
    if (idx > 0) {
        --idx;
    }
    if (idx >= values.size()) {
        idx = values.size() - 1;
    }
    return values[idx];
}

struct RgbSum {
    double r = 0;
    double g = 0;
    double b = 0;
    uint64_t n = 0;

    void add(uint8_t pr, uint8_t pg, uint8_t pb) {
        r += pr;
        g += pg;
        b += pb;
        ++n;
    }

    double ar() const { return n == 0 ? 0 : r / static_cast<double>(n); }
    double ag() const { return n == 0 ? 0 : g / static_cast<double>(n); }
    double ab() const { return n == 0 ? 0 : b / static_cast<double>(n); }
};

struct FrameStats {
    double avgR = 0;
    double avgG = 0;
    double avgB = 0;
    double leftR = 0;
    double leftG = 0;
    double leftB = 0;
    double rightR = 0;
    double rightG = 0;
    double rightB = 0;
    double topLuma = 0;
    double bottomLuma = 0;
    double centerR = 0;
    double centerG = 0;
    double centerB = 0;
    double blackRatio = 0;
    double greenRatio = 0;
};

}  // namespace

struct QxrVideoTest::Impl {
    bool create(int streamW, int streamH, const char* dir, bool srgb);
    void start();
    void stop();
    void teardown();
    ~Impl();

    ANativeWindow* decoderWindowLocked();

private:
    bool createDevice();
    bool formatOk(VkFormat format) const;
    bool createCommands();
    bool createTarget(VkFormat format);
    void destroyTarget();
    bool beginCommands();
    bool submitAndWait();
    void cmdImageBarrier(VkImageLayout oldLayout, VkImageLayout newLayout, VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                         VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage);
    bool transitionToColor();
    bool createVideo(VkFormat format);
    bool renderOnce(double* gpuMs);
    FrameStats analyze() const;
    void writePpm(const std::string& fileName);
    void logStats(const FrameStats& stats, const std::vector<double>& gpuMs) const;
    void threadMain();
    void loop();

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    uint32_t apiVersion = VK_API_VERSION_1_1;
    char deviceName[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE] = {};
    std::string enabledExtList;
    VkDeviceSize pitchAlign = 4;

    int streamW = 0;
    int streamH = 0;
    uint32_t targetW = 0;
    uint32_t targetH = 0;
    uint32_t rowPitch = 0;
    VkFormat targetFormat = VK_FORMAT_UNDEFINED;

    VkImage targetImage = VK_NULL_HANDLE;
    VkDeviceMemory targetMemory = VK_NULL_HANDLE;
    VkImageView targetView = VK_NULL_HANDLE;
    VkBuffer readBuffer = VK_NULL_HANDLE;
    VkDeviceMemory readMemory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize mappedBytes = 0;
    bool readCoherent = false;

    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;

    QxrVideo video;
    ANativeWindow* window = nullptr;
    std::mutex windowMu;

    std::string outDir;
    std::atomic<bool> stopFlag{false};
    std::thread thread;

    uint64_t renders = 0;
    uint64_t newFrames = 0;
    uint64_t consumeFalse = 0;
    std::vector<uint8_t> ppmRow;
};

QxrVideoTest::Impl::~Impl() {
    teardown();
}

ANativeWindow* QxrVideoTest::Impl::decoderWindowLocked() {
    std::lock_guard<std::mutex> lock(windowMu);
    if (window != nullptr) {
        ANativeWindow_acquire(window);
    }
    return window;
}

bool QxrVideoTest::Impl::create(int inStreamW, int inStreamH, const char* dir, bool srgb) {
    if (inStreamW <= 0 || inStreamH <= 0) {
        QXR_LOGE("VTEST", "create: stream size is empty (%dx%d)", inStreamW, inStreamH);
        return false;
    }
    if (dir == nullptr || dir[0] == '\0') {
        QXR_LOGE("VTEST", "create: outDir is empty");
        return false;
    }
    streamW = inStreamW;
    streamH = inStreamH;
    outDir = dir;
    while (!outDir.empty() && (outDir.back() == '/' || outDir.back() == '\\')) {
        outDir.pop_back();
    }
    if (outDir.empty()) {
        QXR_LOGE("VTEST", "create: outDir is empty");
        return false;
    }
    targetW = halfEvenUp(streamW);
    targetH = halfEvenUp(streamH);
    if (targetW == 0 || targetH == 0) {
        QXR_LOGE("VTEST", "create: target size is empty");
        return false;
    }
    // §QXR-VIDEO-TEST：讀回緩衝是 width*4 再對齊。這裡擋掉會讓 uint32 列長溢位的尺寸。
    if (targetW > 16384u || targetH > 16384u) {
        QXR_LOGE("VTEST", "target size is too large: %ux%u", targetW, targetH);
        return false;
    }

    if (!createDevice()) {
        return false;
    }
    // 格式在建立時就定死。只查這一個，不再為了中途改 sRGB 預先要求兩種都能用。
    const VkFormat chosen = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    if (!formatOk(chosen)) {
        return false;
    }
    if (!createCommands() || !createTarget(chosen) || !transitionToColor() || !createVideo(chosen)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(windowMu);
        window = video.decoderWindow();
    }
    if (mkdir(outDir.c_str(), 0700) != 0 && errno != EEXIST) {
        QXR_LOGW("VTEST", "mkdir failed: %s errno=%d", outDir.c_str(), errno);
    }
    QXR_LOGI("VTEST", "ready stream=%dx%d target=%ux%u format=%s out=%s", streamW, streamH, targetW, targetH,
             formatName(targetFormat), outDir.c_str());
    return true;
}

bool QxrVideoTest::Impl::createDevice() {
    uint32_t loaderApi = 0;
    VkResult result = vkEnumerateInstanceVersion(&loaderApi);
    if (result != VK_SUCCESS || loaderApi == 0) {
        if (result != VK_SUCCESS) {
            QXR_LOGE("VTEST", "vkEnumerateInstanceVersion failed: VkResult=%d", static_cast<int>(result));
        }
        loaderApi = VK_API_VERSION_1_0;
    }
    if (loaderApi < VK_API_VERSION_1_1) {
        QXR_LOGE("VTEST", "Vulkan loader API is below 1.1: 0x%x", loaderApi);
        return false;
    }

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "VipleStream";
    app.pEngineName = "QxrVideoTest";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    result = vkCreateInstance(&ici, nullptr, &instance);
    if (!vkOk(result, "vkCreateInstance") || instance == VK_NULL_HANDLE) {
        instance = VK_NULL_HANDLE;
        return false;
    }

    uint32_t deviceCount = 0;
    result = vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
    if (!vkOk(result, "vkEnumeratePhysicalDevices")) {
        return false;
    }
    if (deviceCount == 0) {
        QXR_LOGE("VTEST", "no physical device");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(deviceCount);
    result = vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());
    if (!vkOk(result, "vkEnumeratePhysicalDevices")) {
        return false;
    }
    devices.resize(deviceCount);

    bool foundDevice = false;
    for (VkPhysicalDevice candidate : devices) {
        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        if (familyCount > 0) {
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &familyCount, families.data());
        }
        for (uint32_t i = 0; i < familyCount; ++i) {
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && families[i].queueCount > 0) {
                physical = candidate;
                queueFamily = i;
                foundDevice = true;
                break;
            }
        }
        if (foundDevice) {
            break;
        }
    }
    if (!foundDevice) {
        QXR_LOGE("VTEST", "no graphics queue family");
        return false;
    }

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physical, &props);
    std::snprintf(deviceName, sizeof(deviceName), "%s", props.deviceName);
    if (props.apiVersion < VK_API_VERSION_1_1) {
        QXR_LOGE("VTEST", "GPU Vulkan API is below 1.1: \"%s\" api=0x%x", deviceName, props.apiVersion);
        return false;
    }
    apiVersion = VK_API_VERSION_1_1;
    pitchAlign = props.limits.optimalBufferCopyRowPitchAlignment;
    if (pitchAlign < 4) {
        pitchAlign = 4;
    }

    uint32_t extCount = 0;
    result = vkEnumerateDeviceExtensionProperties(physical, nullptr, &extCount, nullptr);
    if (!vkOk(result, "vkEnumerateDeviceExtensionProperties")) {
        return false;
    }
    std::vector<VkExtensionProperties> deviceExts(extCount);
    if (extCount > 0) {
        result = vkEnumerateDeviceExtensionProperties(physical, nullptr, &extCount, deviceExts.data());
        if (!vkOk(result, "vkEnumerateDeviceExtensionProperties")) {
            return false;
        }
        deviceExts.resize(extCount);
    }
    if (!extListed(deviceExts, VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME)) {
        QXR_LOGE("VTEST", "missing device extension %s", VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME);
        return false;
    }
    if (!extListed(deviceExts, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME)) {
        QXR_LOGE("VTEST", "missing device extension %s", VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
        return false;
    }

    // §QXR-VIDEO-TEST：1.1 已升成核心的擴充不放進 ppEnabledExtensionNames，部分驅動會拒。跟 qxr_session.cpp 同一條規則。
    const char* kCore11[] = {
        VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_MAINTENANCE1_EXTENSION_NAME,
        VK_KHR_BIND_MEMORY_2_EXTENSION_NAME,
        VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
        VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
    };
    std::vector<const char*> enableExts;
    enableExts.push_back(VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME);
    enableExts.push_back(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);
    if (apiVersion < VK_API_VERSION_1_1) {
        for (const char* name : kCore11) {
            if (!extListed(deviceExts, name)) {
                QXR_LOGE("VTEST", "missing device extension %s", name);
                return false;
            }
            enableExts.push_back(name);
        }
    } else {
        for (const char* name : kCore11) {
            if (!extListed(deviceExts, name)) {
                QXR_LOGI("VTEST", "%s not advertised; using Vulkan 1.1 core", name);
            }
        }
    }
    enabledExtList.clear();
    for (size_t i = 0; i < enableExts.size(); ++i) {
        if (i != 0) {
            enabledExtList += ' ';
        }
        enabledExtList += enableExts[i];
    }

    VkPhysicalDeviceSamplerYcbcrConversionFeatures supportedYcbcr{};
    supportedYcbcr.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES;
    VkPhysicalDeviceFeatures2 queryFeatures{};
    queryFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    queryFeatures.pNext = &supportedYcbcr;
    vkGetPhysicalDeviceFeatures2(physical, &queryFeatures);
    if (supportedYcbcr.samplerYcbcrConversion != VK_TRUE) {
        QXR_LOGE("VTEST", "samplerYcbcrConversion is not supported");
        return false;
    }

    VkPhysicalDeviceSamplerYcbcrConversionFeatures enableYcbcr{};
    enableYcbcr.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES;
    enableYcbcr.samplerYcbcrConversion = VK_TRUE;
    VkPhysicalDeviceFeatures2 enableFeatures{};
    enableFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    enableFeatures.pNext = &enableYcbcr;

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = &enableFeatures;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &queueInfo;
    dci.enabledExtensionCount = static_cast<uint32_t>(enableExts.size());
    dci.ppEnabledExtensionNames = enableExts.data();
    // §QXR-VIDEO-TEST：pEnabledFeatures 與 pNext 的 Features2 不能同時給，否則建立會被拒。
    dci.pEnabledFeatures = nullptr;

    result = vkCreateDevice(physical, &dci, nullptr, &device);
    if (!vkOk(result, "vkCreateDevice") || device == VK_NULL_HANDLE) {
        device = VK_NULL_HANDLE;
        return false;
    }
    vkGetDeviceQueue(device, queueFamily, 0, &queue);
    if (queue == VK_NULL_HANDLE) {
        QXR_LOGE("VTEST", "vkGetDeviceQueue returned null");
        return false;
    }
    QXR_LOGI("VTEST", "device=\"%s\" api=%u.%u queueFamily=%u exts=%s feature=samplerYcbcrConversion", deviceName,
             VK_API_VERSION_MAJOR(apiVersion), VK_API_VERSION_MINOR(apiVersion), queueFamily, enabledExtList.c_str());
    return true;
}

bool QxrVideoTest::Impl::formatOk(VkFormat format) const {
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(physical, format, &props);
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    if ((props.optimalTilingFeatures & need) != need) {
        QXR_LOGE("VTEST", "format %s (%d) missing features have=0x%x need=0x%x", formatName(format),
                 static_cast<int>(format), static_cast<unsigned>(props.optimalTilingFeatures), static_cast<unsigned>(need));
        return false;
    }
    return true;
}

bool QxrVideoTest::Impl::createCommands() {
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamily;
    VkResult result = vkCreateCommandPool(device, &poolInfo, nullptr, &pool);
    if (!vkOk(result, "vkCreateCommandPool")) {
        return false;
    }
    VkCommandBufferAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate.commandPool = pool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    result = vkAllocateCommandBuffers(device, &allocate, &cmd);
    if (!vkOk(result, "vkAllocateCommandBuffers")) {
        return false;
    }
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    result = vkCreateFence(device, &fenceInfo, nullptr, &fence);
    return vkOk(result, "vkCreateFence");
}

bool QxrVideoTest::Impl::createTarget(VkFormat format) {
    destroyTarget();
    targetFormat = format;
    const uint32_t align = pitchAlign > 0xffffffffull ? 0xffffffffu : static_cast<uint32_t>(pitchAlign);
    rowPitch = roundUp(targetW * 4u, align < 4u ? 4u : align);
    if (rowPitch < targetW * 4u || (rowPitch % 4u) != 0) {
        QXR_LOGE("VTEST", "readback pitch is unusable pitch=%u width=%u", rowPitch, targetW);
        return false;
    }
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(rowPitch) * static_cast<VkDeviceSize>(targetH);
    if (rowPitch != 0 && bytes / rowPitch != targetH) {
        QXR_LOGE("VTEST", "readback size overflow %ux%u pitch=%u", targetW, targetH, rowPitch);
        return false;
    }

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = VkExtent3D{targetW, targetH, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult result = vkCreateImage(device, &imageInfo, nullptr, &targetImage);
    if (!vkOk(result, "vkCreateImage")) {
        return false;
    }
    VkMemoryRequirements imageReq{};
    vkGetImageMemoryRequirements(device, targetImage, &imageReq);
    int imageType = findMemory(physical, imageReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (imageType < 0) {
        imageType = findMemory(physical, imageReq.memoryTypeBits, 0);
    }
    if (imageType < 0) {
        QXR_LOGE("VTEST", "no memory type for target image");
        return false;
    }
    VkMemoryAllocateInfo imageAlloc{};
    imageAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    imageAlloc.allocationSize = imageReq.size;
    imageAlloc.memoryTypeIndex = static_cast<uint32_t>(imageType);
    result = vkAllocateMemory(device, &imageAlloc, nullptr, &targetMemory);
    if (!vkOk(result, "vkAllocateMemory")) {
        return false;
    }
    result = vkBindImageMemory(device, targetImage, targetMemory, 0);
    if (!vkOk(result, "vkBindImageMemory")) {
        return false;
    }
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = targetImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    result = vkCreateImageView(device, &viewInfo, nullptr, &targetView);
    if (!vkOk(result, "vkCreateImageView")) {
        return false;
    }

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = bytes;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    result = vkCreateBuffer(device, &bufferInfo, nullptr, &readBuffer);
    if (!vkOk(result, "vkCreateBuffer")) {
        return false;
    }
    VkMemoryRequirements bufferReq{};
    vkGetBufferMemoryRequirements(device, readBuffer, &bufferReq);
    int bufferType = -1;
    readCoherent = false;
    const VkMemoryPropertyFlags kHost[] = {
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
    };
    const bool kCoherent[] = {true, true, false, false};
    for (int i = 0; i < 4; ++i) {
        bufferType = findMemory(physical, bufferReq.memoryTypeBits, kHost[i]);
        if (bufferType >= 0) {
            readCoherent = kCoherent[i];
            break;
        }
    }
    if (bufferType < 0) {
        QXR_LOGE("VTEST", "no host-visible memory type for readback");
        return false;
    }
    VkMemoryAllocateInfo bufferAlloc{};
    bufferAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    bufferAlloc.allocationSize = bufferReq.size;
    bufferAlloc.memoryTypeIndex = static_cast<uint32_t>(bufferType);
    result = vkAllocateMemory(device, &bufferAlloc, nullptr, &readMemory);
    if (!vkOk(result, "vkAllocateMemory")) {
        return false;
    }
    result = vkBindBufferMemory(device, readBuffer, readMemory, 0);
    if (!vkOk(result, "vkBindBufferMemory")) {
        return false;
    }
    result = vkMapMemory(device, readMemory, 0, bufferReq.size, 0, &mapped);
    if (!vkOk(result, "vkMapMemory") || mapped == nullptr) {
        mapped = nullptr;
        return false;
    }
    mappedBytes = bufferReq.size;
    if (mappedBytes < bytes) {
        QXR_LOGE("VTEST", "readback mapping is shorter than the copy (%llu < %llu)",
                 static_cast<unsigned long long>(mappedBytes), static_cast<unsigned long long>(bytes));
        return false;
    }
    return true;
}

void QxrVideoTest::Impl::destroyTarget() {
    if (device != VK_NULL_HANDLE && mapped != nullptr && readMemory != VK_NULL_HANDLE) {
        vkUnmapMemory(device, readMemory);
    }
    mapped = nullptr;
    mappedBytes = 0;
    if (device != VK_NULL_HANDLE && readBuffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device, readBuffer, nullptr);
    }
    readBuffer = VK_NULL_HANDLE;
    if (device != VK_NULL_HANDLE && readMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device, readMemory, nullptr);
    }
    readMemory = VK_NULL_HANDLE;
    if (device != VK_NULL_HANDLE && targetView != VK_NULL_HANDLE) {
        vkDestroyImageView(device, targetView, nullptr);
    }
    targetView = VK_NULL_HANDLE;
    if (device != VK_NULL_HANDLE && targetImage != VK_NULL_HANDLE) {
        vkDestroyImage(device, targetImage, nullptr);
    }
    targetImage = VK_NULL_HANDLE;
    if (device != VK_NULL_HANDLE && targetMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device, targetMemory, nullptr);
    }
    targetMemory = VK_NULL_HANDLE;
}

bool QxrVideoTest::Impl::beginCommands() {
    VkResult result = vkResetFences(device, 1, &fence);
    if (!vkOk(result, "vkResetFences")) {
        return false;
    }
    result = vkResetCommandBuffer(cmd, 0);
    if (!vkOk(result, "vkResetCommandBuffer")) {
        return false;
    }
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vkBeginCommandBuffer(cmd, &begin);
    return vkOk(result, "vkBeginCommandBuffer");
}

bool QxrVideoTest::Impl::submitAndWait() {
    VkResult result = vkEndCommandBuffer(cmd);
    if (!vkOk(result, "vkEndCommandBuffer")) {
        return false;
    }
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    result = vkQueueSubmit(queue, 1, &submit, fence);
    if (!vkOk(result, "vkQueueSubmit")) {
        return false;
    }
    result = vkWaitForFences(device, 1, &fence, VK_TRUE, kFenceWaitNs);
    return vkOk(result, "vkWaitForFences");
}

void QxrVideoTest::Impl::cmdImageBarrier(VkImageLayout oldLayout, VkImageLayout newLayout, VkAccessFlags srcAccess,
                                        VkAccessFlags dstAccess, VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = srcAccess;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = targetImage;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

bool QxrVideoTest::Impl::transitionToColor() {
    if (!beginCommands()) {
        return false;
    }
    // §QXR-VIDEO-TEST：render pass 的 initialLayout 是 COLOR_ATTACHMENT_OPTIMAL。先轉好，模擬 swapchain acquire 之後。
    cmdImageBarrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    return submitAndWait();
}

bool QxrVideoTest::Impl::createVideo(VkFormat format) {
    QxrVideoCreateInfo info{};
    info.physicalDevice = physical;
    info.device = device;
    info.queueFamily = queueFamily;
    info.targetFormat = format;
    info.streamWidth = streamW;
    info.streamHeight = streamH;
    const char* error = nullptr;
    if (!video.create(info, &error)) {
        QXR_LOGE("VTEST", "QxrVideo::create failed: %s", error != nullptr ? error : "unknown");
        return false;
    }
    targetFormat = format;
    return true;
}

bool QxrVideoTest::Impl::renderOnce(double* gpuMs) {
    if (!beginCommands()) {
        return false;
    }
    const bool drew = video.renderTo(cmd, targetView, targetW, targetH, video.sourceUv());
    if (!drew) {
        QXR_LOGE("VTEST", "QxrVideo::renderTo failed");
        vkEndCommandBuffer(cmd);
        return false;
    }
    // render pass 出去的 dependency 把 color write 交到 TRANSFER。src 要接到同一階段，layout transition 才看得到剛寫的像素。
    cmdImageBarrier(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = (rowPitch == targetW * 4u) ? 0u : (rowPitch / 4u);
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = VkExtent3D{targetW, targetH, 1};
    vkCmdCopyImageToBuffer(cmd, targetImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readBuffer, 1, &region);
    // §QXR-VIDEO-TEST：讀回把 layout 轉成 TRANSFER_SRC。下一圈 renderTo 仍假設 COLOR_ATTACHMENT_OPTIMAL，同一條 command buffer 裡轉回去。
    cmdImageBarrier(VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    VkResult result = vkEndCommandBuffer(cmd);
    if (!vkOk(result, "vkEndCommandBuffer")) {
        return false;
    }
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    const auto t0 = std::chrono::steady_clock::now();
    result = vkQueueSubmit(queue, 1, &submit, fence);
    if (!vkOk(result, "vkQueueSubmit")) {
        return false;
    }
    result = vkWaitForFences(device, 1, &fence, VK_TRUE, kFenceWaitNs);
    const auto t1 = std::chrono::steady_clock::now();
    if (!vkOk(result, "vkWaitForFences")) {
        return false;
    }
    *gpuMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
    if (!readCoherent) {
        VkMappedMemoryRange range{};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = readMemory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        result = vkInvalidateMappedMemoryRanges(device, 1, &range);
        if (!vkOk(result, "vkInvalidateMappedMemoryRanges")) {
            return false;
        }
    }
    return true;
}

FrameStats QxrVideoTest::Impl::analyze() const {
    FrameStats stats;
    if (mapped == nullptr || targetW == 0 || targetH == 0 || rowPitch < targetW * 4u) {
        return stats;
    }
    const auto* base = static_cast<const uint8_t*>(mapped);
    const uint32_t mid = targetW / 2u;
    uint32_t band = targetH / 10u;
    if (band < 1u) {
        band = 1u;
    }
    if (band > targetH) {
        band = targetH;
    }
    const uint32_t boxW = targetW >= 8u ? 8u : targetW;
    const uint32_t boxH = targetH >= 8u ? 8u : targetH;
    const uint32_t boxX = (targetW - boxW) / 2u;
    const uint32_t boxY = (targetH - boxH) / 2u;

    RgbSum all;
    RgbSum left;
    RgbSum right;
    RgbSum center;
    double topLuma = 0;
    double bottomLuma = 0;
    uint64_t topN = 0;
    uint64_t bottomN = 0;
    uint64_t blackN = 0;
    uint64_t greenN = 0;

    for (uint32_t y = 0; y < targetH; ++y) {
        const uint8_t* row = base + static_cast<size_t>(y) * rowPitch;
        const bool inTop = y < band;
        const bool inBottom = y >= targetH - band;
        const bool inBoxY = y >= boxY && y < boxY + boxH;
        for (uint32_t x = 0; x < targetW; ++x) {
            const uint8_t* px = row + static_cast<size_t>(x) * 4u;
            const uint8_t r = px[0];
            const uint8_t g = px[1];
            const uint8_t b = px[2];
            all.add(r, g, b);
            if (x < mid) {
                left.add(r, g, b);
            } else {
                right.add(r, g, b);
            }
            if (r == 0 && g == 0 && b == 0) {
                ++blackN;
            }
            if (g >= 200 && r <= 40 && b <= 40) {
                ++greenN;
            }
            if (inTop || inBottom) {
                const double luma = static_cast<double>((static_cast<unsigned>(r) * 54u + static_cast<unsigned>(g) * 183u
                                                          + static_cast<unsigned>(b) * 19u)
                                                         >> 8);
                if (inTop) {
                    topLuma += luma;
                    ++topN;
                }
                if (inBottom) {
                    bottomLuma += luma;
                    ++bottomN;
                }
            }
            if (inBoxY && x >= boxX && x < boxX + boxW) {
                center.add(r, g, b);
            }
        }
    }

    stats.avgR = all.ar();
    stats.avgG = all.ag();
    stats.avgB = all.ab();
    stats.leftR = left.ar();
    stats.leftG = left.ag();
    stats.leftB = left.ab();
    stats.rightR = right.ar();
    stats.rightG = right.ag();
    stats.rightB = right.ab();
    stats.topLuma = topN == 0 ? 0 : topLuma / static_cast<double>(topN);
    stats.bottomLuma = bottomN == 0 ? 0 : bottomLuma / static_cast<double>(bottomN);
    stats.centerR = center.ar();
    stats.centerG = center.ag();
    stats.centerB = center.ab();
    const double denom = all.n == 0 ? 1.0 : static_cast<double>(all.n);
    stats.blackRatio = static_cast<double>(blackN) / denom;
    stats.greenRatio = static_cast<double>(greenN) / denom;
    return stats;
}

void QxrVideoTest::Impl::writePpm(const std::string& fileName) {
    if (mapped == nullptr || targetW == 0 || targetH == 0 || rowPitch < targetW * 4u) {
        QXR_LOGE("VTEST", "writePpm skipped: no readback (%s)", fileName.c_str());
        return;
    }
    const std::string path = outDir + "/" + fileName;
    FILE* file = std::fopen(path.c_str(), "wb");
    if (file == nullptr) {
        QXR_LOGE("VTEST", "fopen failed: %s errno=%d", path.c_str(), errno);
        return;
    }
    char header[64];
    const int headerLen = std::snprintf(header, sizeof(header), "P6\n%u %u\n255\n", targetW, targetH);
    if (headerLen <= 0
        || std::fwrite(header, 1, static_cast<size_t>(headerLen), file) != static_cast<size_t>(headerLen)) {
        QXR_LOGE("VTEST", "fwrite failed: %s errno=%d", path.c_str(), errno);
        std::fclose(file);
        std::remove(path.c_str());
        return;
    }
    ppmRow.resize(static_cast<size_t>(targetW) * 3u);
    const auto* base = static_cast<const uint8_t*>(mapped);
    for (uint32_t y = 0; y < targetH; ++y) {
        const uint8_t* row = base + static_cast<size_t>(y) * rowPitch;
        for (uint32_t x = 0; x < targetW; ++x) {
            ppmRow[static_cast<size_t>(x) * 3u] = row[static_cast<size_t>(x) * 4u];
            ppmRow[static_cast<size_t>(x) * 3u + 1u] = row[static_cast<size_t>(x) * 4u + 1u];
            ppmRow[static_cast<size_t>(x) * 3u + 2u] = row[static_cast<size_t>(x) * 4u + 2u];
        }
        if (std::fwrite(ppmRow.data(), 1, ppmRow.size(), file) != ppmRow.size()) {
            QXR_LOGE("VTEST", "fwrite failed: %s errno=%d", path.c_str(), errno);
            std::fclose(file);
            std::remove(path.c_str());
            return;
        }
    }
    if (std::fclose(file) != 0) {
        QXR_LOGE("VTEST", "fclose failed: %s errno=%d", path.c_str(), errno);
        return;
    }
    QXR_LOGI("VTEST", "wrote %s %ux%u", path.c_str(), targetW, targetH);
}

void QxrVideoTest::Impl::logStats(const FrameStats& stats, const std::vector<double>& gpuMs) const {
    QXR_LOGI("VTEST",
             "renders=%llu newFrames=%llu consumeFalse=%llu avg=%.1f,%.1f,%.1f left=%.1f,%.1f,%.1f right=%.1f,%.1f,%.1f "
             "top10=%.1f bottom10=%.1f center=%.1f,%.1f,%.1f black=%.3f green=%.3f gpuMs p50=%.2f p95=%.2f n=%u "
             "format=%s target=%ux%u",
             static_cast<unsigned long long>(renders), static_cast<unsigned long long>(newFrames),
             static_cast<unsigned long long>(consumeFalse), stats.avgR, stats.avgG, stats.avgB, stats.leftR, stats.leftG,
             stats.leftB, stats.rightR, stats.rightG, stats.rightB, stats.topLuma, stats.bottomLuma, stats.centerR,
             stats.centerG, stats.centerB, stats.blackRatio, stats.greenRatio, percentile(gpuMs, 50), percentile(gpuMs, 95),
             static_cast<unsigned>(gpuMs.size()), formatName(targetFormat), targetW, targetH);
}

void QxrVideoTest::Impl::threadMain() {
    // §QXR-VIDEO-TEST：logcat 裡這條執行緒要跟解碼器分開，崩潰時才對得到。
    pthread_setname_np(pthread_self(), "qxr-vtest");
    try {
        loop();
    } catch (const std::exception& ex) {
        QXR_LOGE("VTEST", "render thread exception: %s", ex.what());
    }
    QXR_LOGI("VTEST", "render thread exit renders=%llu newFrames=%llu consumeFalse=%llu",
             static_cast<unsigned long long>(renders), static_cast<unsigned long long>(newFrames),
             static_cast<unsigned long long>(consumeFalse));
}

void QxrVideoTest::Impl::loop() {
    const auto t0 = std::chrono::steady_clock::now();
    auto nextLog = t0 + std::chrono::seconds(2);
    bool shot3 = false;
    bool shot10 = false;
    bool shot30 = false;
    bool shot60 = false;
    FrameStats lastStats;
    std::vector<double> gpuMs;

    auto elapsedSec = [&]() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    };
    auto writeTimedShot = [&](int sec, bool* flag) {
        if (*flag || elapsedSec() < static_cast<double>(sec) || renders == 0) {
            return;
        }
        writePpm(std::string("qxr_vtest_") + formatName(targetFormat) + "_" + std::to_string(sec) + "s.ppm");
        *flag = true;
    };

    while (!stopFlag.load(std::memory_order_acquire)) {
        const bool woke = video.waitForImage(50);
        if (stopFlag.load(std::memory_order_acquire)) {
            break;
        }
        if (woke) {
            ++newFrames;
        }
        // §QXR-VIDEO-TEST：每圈都主動取圖，不只靠 listener 叫醒；這樣才分得出「通知沒來」與「生產端沒送」。
        // 上一圈 renderOnce 已等過 fence。consumeLatest 淘汰快取時會拆 VkImage。
        if (!video.consumeLatest()) {
            ++consumeFalse;
        }
        if (stopFlag.load(std::memory_order_acquire)) {
            break;
        }

        double gpu = 0;
        if (!renderOnce(&gpu)) {
            QXR_LOGE("VTEST", "render thread stopped after Vulkan error");
            break;
        }
        ++renders;
        gpuMs.push_back(gpu);
        lastStats = analyze();
        video.pumpStats();

        writeTimedShot(3, &shot3);
        writeTimedShot(10, &shot10);
        writeTimedShot(30, &shot30);
        writeTimedShot(60, &shot60);

        if (std::chrono::steady_clock::now() >= nextLog) {
            logStats(lastStats, gpuMs);
            gpuMs.clear();
            nextLog = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        }
    }
}

void QxrVideoTest::Impl::start() {
    if (thread.joinable() || device == VK_NULL_HANDLE) {
        return;
    }
    stopFlag.store(false, std::memory_order_release);
    thread = std::thread([this] { threadMain(); });
}

void QxrVideoTest::Impl::stop() {
    stopFlag.store(true, std::memory_order_release);
    if (thread.joinable()) {
        thread.join();
    }
}

void QxrVideoTest::Impl::teardown() {
    if (thread.joinable()) {
        // §QXR-VIDEO-TEST：std::thread 在還 joinable 時解構會 terminate。這裡補一次，避免拆到一半直接殺程序。
        stopFlag.store(true, std::memory_order_release);
        thread.join();
    }
    if (device != VK_NULL_HANDLE) {
        const VkResult idle = vkDeviceWaitIdle(device);
        if (idle != VK_SUCCESS) {
            QXR_LOGE("VTEST", "vkDeviceWaitIdle failed: VkResult=%d", static_cast<int>(idle));
        }
    }
    {
        std::lock_guard<std::mutex> lock(windowMu);
        window = nullptr;
    }
    video.destroy();
    destroyTarget();
    if (device != VK_NULL_HANDLE && pool != VK_NULL_HANDLE && cmd != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(device, pool, 1, &cmd);
    }
    cmd = VK_NULL_HANDLE;
    if (device != VK_NULL_HANDLE && pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device, pool, nullptr);
    }
    pool = VK_NULL_HANDLE;
    if (device != VK_NULL_HANDLE && fence != VK_NULL_HANDLE) {
        vkDestroyFence(device, fence, nullptr);
    }
    fence = VK_NULL_HANDLE;
    if (device != VK_NULL_HANDLE) {
        vkDestroyDevice(device, nullptr);
    }
    device = VK_NULL_HANDLE;
    queue = VK_NULL_HANDLE;
    physical = VK_NULL_HANDLE;
    if (instance != VK_NULL_HANDLE) {
        vkDestroyInstance(instance, nullptr);
    }
    instance = VK_NULL_HANDLE;
}

QxrVideoTest::~QxrVideoTest() {
    destroy();
}

bool QxrVideoTest::create(int streamW, int streamH, const char* outDir, bool srgb) {
    std::lock_guard<std::mutex> lock(life);
    if (impl != nullptr) {
        QXR_LOGE("VTEST", "create called twice");
        return false;
    }
    impl = new Impl();
    try {
        if (!impl->create(streamW, streamH, outDir, srgb)) {
            delete impl;
            impl = nullptr;
            return false;
        }
    } catch (...) {
        delete impl;
        impl = nullptr;
        throw;
    }
    return true;
}

ANativeWindow* QxrVideoTest::decoderWindow() const {
    std::lock_guard<std::mutex> lock(life);
    if (impl == nullptr) {
        return nullptr;
    }
    return impl->decoderWindowLocked();
}

void QxrVideoTest::start() {
    std::lock_guard<std::mutex> lock(life);
    if (impl != nullptr) {
        impl->start();
    }
}

void QxrVideoTest::stop() {
    std::lock_guard<std::mutex> lock(life);
    if (impl != nullptr) {
        impl->stop();
    }
}

void QxrVideoTest::destroy() {
    std::lock_guard<std::mutex> lock(life);
    if (impl == nullptr) {
        return;
    }
    impl->stop();
    delete impl;
    impl = nullptr;
}
