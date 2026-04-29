// VipleStream §J.3.e.2.i — VkFrucRenderer
// See vkfruc.h header + docs/J.3.e.2.i_vulkan_native_renderer.md.

#include "vkfruc.h"

#ifdef HAVE_LIBPLACEBO_VULKAN

#include <SDL.h>
#include <SDL_vulkan.h>
#include <cstring>

VkFrucRenderer::VkFrucRenderer(int pass)
    : IFFmpegRenderer(RendererType::Vulkan)
    , m_Pass(pass)
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.2 ctor (pass=%d)", pass);
}

VkFrucRenderer::~VkFrucRenderer()
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.2 dtor");
    teardown();
}

void VkFrucRenderer::teardown()
{
    // Order: device-owned objects → device → surface → instance.
    // i.3+ will insert swapchain / pipelines / etc. here.
    if (m_Device != VK_NULL_HANDLE && m_pfnDestroyDevice) {
        m_pfnDestroyDevice(m_Device, nullptr);
        m_Device = VK_NULL_HANDLE;
    }
    if (m_Surface != VK_NULL_HANDLE && m_pfnDestroySurfaceKHR) {
        m_pfnDestroySurfaceKHR(m_Instance, m_Surface, nullptr);
        m_Surface = VK_NULL_HANDLE;
    }
    if (m_Instance != VK_NULL_HANDLE && m_pfnDestroyInstance) {
        m_pfnDestroyInstance(m_Instance, nullptr);
        m_Instance = VK_NULL_HANDLE;
    }
}

bool VkFrucRenderer::createInstanceAndSurface(SDL_Window* window)
{
    m_pfnGetInstanceProcAddr =
        (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    if (!m_pfnGetInstanceProcAddr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.2 SDL_Vulkan_GetVkGetInstanceProcAddr failed: %s",
                     SDL_GetError());
        return false;
    }

    auto pfnCreateInstance =
        (PFN_vkCreateInstance)m_pfnGetInstanceProcAddr(nullptr, "vkCreateInstance");
    if (!pfnCreateInstance) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.2 getProc(vkCreateInstance) NULL");
        return false;
    }

    unsigned int extCount = 0;
    if (!SDL_Vulkan_GetInstanceExtensions(window, &extCount, nullptr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.2 SDL_Vulkan_GetInstanceExtensions #1: %s",
                     SDL_GetError());
        return false;
    }
    std::vector<const char*> exts(extCount);
    if (!SDL_Vulkan_GetInstanceExtensions(window, &extCount, exts.data())) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.2 SDL_Vulkan_GetInstanceExtensions #2: %s",
                     SDL_GetError());
        return false;
    }

    VkApplicationInfo appInfo = {};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "VipleStreamFruc";
    appInfo.applicationVersion = 1;
    appInfo.pEngineName = "VipleStream";
    appInfo.engineVersion = 1;
    appInfo.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ici = {};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &appInfo;
    ici.enabledExtensionCount = (uint32_t)exts.size();
    ici.ppEnabledExtensionNames = exts.data();

    VkResult rc = pfnCreateInstance(&ici, nullptr, &m_Instance);
    if (rc != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.2 vkCreateInstance rc=%d", (int)rc);
        return false;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.2 VkInstance created (exts=%u)",
                (unsigned)exts.size());

    m_pfnDestroyInstance = (PFN_vkDestroyInstance)m_pfnGetInstanceProcAddr(m_Instance, "vkDestroyInstance");
    m_pfnDestroySurfaceKHR = (PFN_vkDestroySurfaceKHR)m_pfnGetInstanceProcAddr(m_Instance, "vkDestroySurfaceKHR");
    if (!m_pfnDestroyInstance || !m_pfnDestroySurfaceKHR) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.2 instance proc table incomplete");
        return false;
    }

    if (!SDL_Vulkan_CreateSurface(window, m_Instance, &m_Surface)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.2 SDL_Vulkan_CreateSurface: %s",
                     SDL_GetError());
        return false;
    }
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.2 VkSurfaceKHR created");
    return true;
}

bool VkFrucRenderer::pickPhysicalDeviceAndQueue()
{
    auto pfnEnumPDs = (PFN_vkEnumeratePhysicalDevices)m_pfnGetInstanceProcAddr(
        m_Instance, "vkEnumeratePhysicalDevices");
    auto pfnGetPDProps = (PFN_vkGetPhysicalDeviceProperties)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetPhysicalDeviceProperties");
    auto pfnGetQFP = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    auto pfnGetSurfSupport = (PFN_vkGetPhysicalDeviceSurfaceSupportKHR)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetPhysicalDeviceSurfaceSupportKHR");
    if (!pfnEnumPDs || !pfnGetPDProps || !pfnGetQFP || !pfnGetSurfSupport) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.2 PD proc PFN load failed");
        return false;
    }

    uint32_t pdCount = 0;
    pfnEnumPDs(m_Instance, &pdCount, nullptr);
    if (pdCount == 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.2 no Vulkan physical devices");
        return false;
    }
    std::vector<VkPhysicalDevice> pds(pdCount);
    pfnEnumPDs(m_Instance, &pdCount, pds.data());

    // Pick first device with a queue family that supports both graphics
    // and present on m_Surface.  Prefer DISCRETE_GPU; fall back to whatever
    // works.  PC NVIDIA / AMD / Intel typically have a "universal" queue
    // family — same pattern as Android's pick_physical_device_and_queue.
    int discreteIdx = -1, anyIdx = -1;
    uint32_t discreteQF = UINT32_MAX, anyQF = UINT32_MAX;
    for (uint32_t i = 0; i < pdCount; i++) {
        VkPhysicalDeviceProperties pdp = {};
        pfnGetPDProps(pds[i], &pdp);

        uint32_t qfCount = 0;
        pfnGetQFP(pds[i], &qfCount, nullptr);
        std::vector<VkQueueFamilyProperties> qfs(qfCount);
        pfnGetQFP(pds[i], &qfCount, qfs.data());

        for (uint32_t qf = 0; qf < qfCount; qf++) {
            VkBool32 surfaceOk = VK_FALSE;
            pfnGetSurfSupport(pds[i], qf, m_Surface, &surfaceOk);
            const bool gfxOk = (qfs[qf].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
            if (!surfaceOk || !gfxOk) continue;

            if (anyIdx < 0) { anyIdx = (int)i; anyQF = qf; }
            if (pdp.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU
                && discreteIdx < 0) {
                discreteIdx = (int)i; discreteQF = qf;
            }
            break;  // one queue family per device is enough for our pick
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VKFRUC] §J.3.e.2.i.2 PD[%u] '%s' type=%d apiVer=%u",
                    i, pdp.deviceName, (int)pdp.deviceType, pdp.apiVersion);
    }

    if (discreteIdx >= 0) {
        m_PhysicalDevice = pds[discreteIdx];
        m_QueueFamily    = discreteQF;
    } else if (anyIdx >= 0) {
        m_PhysicalDevice = pds[anyIdx];
        m_QueueFamily    = anyQF;
    } else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.2 no PD with graphics+present queue");
        return false;
    }

    VkPhysicalDeviceProperties chosen = {};
    pfnGetPDProps(m_PhysicalDevice, &chosen);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.2 picked '%s' QF=%u %s",
                chosen.deviceName, m_QueueFamily,
                discreteIdx >= 0 ? "(DISCRETE_GPU)" : "(integrated/other)");
    return true;
}

bool VkFrucRenderer::createLogicalDevice()
{
    auto pfnCreateDevice = (PFN_vkCreateDevice)m_pfnGetInstanceProcAddr(
        m_Instance, "vkCreateDevice");
    auto pfnGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)m_pfnGetInstanceProcAddr(
        m_Instance, "vkGetDeviceProcAddr");
    if (!pfnCreateDevice || !pfnGetDeviceProcAddr) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.2 device proc PFN load failed");
        return false;
    }

    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo qci = {};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = m_QueueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &queuePriority;

    // i.2 minimal extension set: KHR_swapchain mandatory; SamplerYcbcrConversion
    // for NV12 sampling in i.3 graphics pipeline.
    const char* devExts[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
    };

    VkPhysicalDeviceSamplerYcbcrConversionFeatures ycbcrFeat = {};
    ycbcrFeat.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES;
    ycbcrFeat.samplerYcbcrConversion = VK_TRUE;

    VkDeviceCreateInfo dci = {};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = &ycbcrFeat;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = (uint32_t)(sizeof(devExts) / sizeof(devExts[0]));
    dci.ppEnabledExtensionNames = devExts;

    VkResult rc = pfnCreateDevice(m_PhysicalDevice, &dci, nullptr, &m_Device);
    if (rc != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.2 vkCreateDevice rc=%d", (int)rc);
        return false;
    }

    m_pfnDestroyDevice = (PFN_vkDestroyDevice)pfnGetDeviceProcAddr(m_Device, "vkDestroyDevice");
    auto pfnGetDeviceQueue = (PFN_vkGetDeviceQueue)pfnGetDeviceProcAddr(m_Device, "vkGetDeviceQueue");
    if (!m_pfnDestroyDevice || !pfnGetDeviceQueue) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-VKFRUC] §J.3.e.2.i.2 device proc table incomplete");
        return false;
    }
    pfnGetDeviceQueue(m_Device, m_QueueFamily, 0, &m_GraphicsQueue);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.2 VkDevice created (%s, QF=%u, queue=%p)",
                "KHR_swapchain + KHR_sampler_ycbcr_conversion",
                m_QueueFamily, (void*)m_GraphicsQueue);
    return true;
}

bool VkFrucRenderer::initialize(PDECODER_PARAMETERS params)
{
    m_Window = params->window;

    if (!createInstanceAndSurface(m_Window)) {
        m_InitFailureReason = InitFailureReason::NoSoftwareSupport;
        teardown();
        return false;
    }
    if (!pickPhysicalDeviceAndQueue()) {
        m_InitFailureReason = InitFailureReason::NoSoftwareSupport;
        teardown();
        return false;
    }
    if (!createLogicalDevice()) {
        m_InitFailureReason = InitFailureReason::NoSoftwareSupport;
        teardown();
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-VKFRUC] §J.3.e.2.i.2 initialize: instance/PD/device READY — "
                "i.3 (graphics pipeline) is the next sub-phase; for now still "
                "returning false so PlVkRenderer handles the actual streaming");

    // i.2 is just instance/device bring-up.  Without swapchain / pipelines /
    // renderFrame impl, we can't actually display anything.  Deliberately
    // return false so cascade falls through to PlVkRenderer.
    m_InitFailureReason = InitFailureReason::NoSoftwareSupport;
    return false;
}

bool VkFrucRenderer::prepareDecoderContext(AVCodecContext* context, AVDictionary** options)
{
    (void)context;
    (void)options;
    return false;
}

void VkFrucRenderer::renderFrame(AVFrame* frame)
{
    (void)frame;
}

int VkFrucRenderer::getDecoderCapabilities()  { return 0; }
int VkFrucRenderer::getRendererAttributes()    { return 0; }

#endif // HAVE_LIBPLACEBO_VULKAN
