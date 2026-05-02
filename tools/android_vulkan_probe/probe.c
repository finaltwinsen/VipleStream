// VipleStream §I.D pre-implementation probe.
//
// Standalone Vulkan 1.0 device-and-queue-family enumerator for Android,
// run from /data/local/tmp/ via adb shell.  Intent: figure out whether
// Pixel 5 / Adreno 620 (and any future device) advertises a separate
// queue family that supports VK_QUEUE_COMPUTE_BIT without
// VK_QUEUE_GRAPHICS_BIT — the precondition for §I.D's "ME / warp on
// dedicated compute queue" optimization.
//
// vk_backend.c on Android already prints this info via [VKBE-QF] log
// during streaming session bring-up, but spinning up a streaming
// session requires a reachable Sunshine host.  This standalone binary
// lets us skip that and answer the queue-family question with a 30-
// second adb push + run.
//
// Build (from scripts/build_android_probe.cmd):
//   aarch64-linux-android30-clang probe.c -o probe -lvulkan
//
// Usage:
//   adb push probe /data/local/tmp/vk_probe
//   adb shell chmod +x /data/local/tmp/vk_probe
//   adb shell /data/local/tmp/vk_probe

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

static const char* qflag_str(VkQueueFlags f) {
    static char buf[128];
    buf[0] = '\0';
    if (f & VK_QUEUE_GRAPHICS_BIT)       strcat(buf, "GFX ");
    if (f & VK_QUEUE_COMPUTE_BIT)        strcat(buf, "CMP ");
    if (f & VK_QUEUE_TRANSFER_BIT)       strcat(buf, "XFER ");
    if (f & VK_QUEUE_SPARSE_BINDING_BIT) strcat(buf, "SPARSE ");
    if (f & VK_QUEUE_PROTECTED_BIT)      strcat(buf, "PROT ");
    return buf;
}

int main(void)
{
    VkApplicationInfo appInfo = {
        .sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "VipleStream-VK-Probe",
        .applicationVersion = 1,
        .pEngineName      = "VipleStream",
        .engineVersion    = 1,
        .apiVersion       = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo ici = {
        .sType                = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo     = &appInfo,
        .enabledExtensionCount = 0,  // no surface needed for queue-family probe
        .enabledLayerCount     = 0,
    };
    VkInstance inst = VK_NULL_HANDLE;
    VkResult r = vkCreateInstance(&ici, NULL, &inst);
    if (r != VK_SUCCESS) {
        fprintf(stderr, "vkCreateInstance failed rc=%d\n", r);
        return 1;
    }

    uint32_t pdCount = 0;
    vkEnumeratePhysicalDevices(inst, &pdCount, NULL);
    if (pdCount == 0) {
        fprintf(stderr, "no physical devices\n");
        return 2;
    }
    VkPhysicalDevice* pds = (VkPhysicalDevice*)calloc(pdCount, sizeof(*pds));
    vkEnumeratePhysicalDevices(inst, &pdCount, pds);

    printf("Vulkan probe: %u physical device(s)\n\n", pdCount);

    for (uint32_t pi = 0; pi < pdCount; pi++) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(pds[pi], &props);
        printf("PhysicalDevice[%u]: '%s'\n", pi, props.deviceName);
        printf("  vendorID = 0x%04x  deviceID = 0x%04x\n",
               props.vendorID, props.deviceID);
        printf("  apiVersion = %u.%u.%u  driverVersion = 0x%08x\n",
               (props.apiVersion >> 22) & 0x7F,
               (props.apiVersion >> 12) & 0x3FF,
               props.apiVersion & 0xFFF, props.driverVersion);
        printf("  deviceType = %s\n",
               props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? "INTEGRATED_GPU" :
               props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU   ? "DISCRETE_GPU"   :
               props.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU    ? "VIRTUAL_GPU"    :
               props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU            ? "CPU"            :
                                                                            "OTHER");

        uint32_t qfCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pds[pi], &qfCount, NULL);
        VkQueueFamilyProperties* qf =
            (VkQueueFamilyProperties*)calloc(qfCount, sizeof(*qf));
        vkGetPhysicalDeviceQueueFamilyProperties(pds[pi], &qfCount, qf);

        printf("  queue families: %u\n", qfCount);
        int hasDedicatedCompute = 0;
        int hasMultipleGfxQueues = 0;
        for (uint32_t i = 0; i < qfCount; i++) {
            const VkQueueFamilyProperties* p = &qf[i];
            printf("    family[%u]: queueCount=%u flags=0x%x [%s]\n",
                   i, p->queueCount, p->queueFlags, qflag_str(p->queueFlags));
            if ((p->queueFlags & VK_QUEUE_COMPUTE_BIT) &&
                !(p->queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
                hasDedicatedCompute = 1;
            }
            if ((p->queueFlags & VK_QUEUE_GRAPHICS_BIT) && p->queueCount > 1) {
                hasMultipleGfxQueues = 1;
            }
        }
        free(qf);

        printf("\n  §I.D async-compute verdict for this device:\n");
        if (hasDedicatedCompute) {
            printf("    [GO]   has dedicated compute family — async compute viable\n");
        } else if (hasMultipleGfxQueues) {
            printf("    [PARTIAL] no dedicated CMP family but graphics family has\n"
                   "             multiple queues — could submit ME/warp to a 2nd queue\n"
                   "             but contends for the same GPU scheduler\n");
        } else {
            printf("    [STOP] only single universal queue — async compute not\n"
                   "           possible on this device; §I.D should defer indefinitely\n");
        }
        printf("\n");
    }
    free(pds);
    vkDestroyInstance(inst, NULL);
    return 0;
}
