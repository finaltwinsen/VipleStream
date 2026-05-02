// VipleStream §I.D Phase D.2.0 verification probe.
//
// Builds on probe.c by also creating a VkDevice with queueCount=3 and
// running vkGetDeviceQueue for each index, so we know whether Adreno
// 620 (or any device) returns three *distinct* VkQueue handles when
// asked for multiple queues from the same family — the precondition
// for §I.D Phase D.2.0's multi-queue acquisition path actually being
// useful.
//
// We also submit a trivial empty cmd buffer to each queue concurrently
// (no fence waits between submits) to confirm the driver doesn't
// reject same-family concurrent submission on principle.  Real-world
// throughput / parallelism still has to be measured during streaming;
// this just verifies the API plumbing is honoured.
//
// Build: tools/android_vulkan_probe/build.cmd (then rerun with `probe2.c`
//   substituted, or just compile manually:
//     %CLANG% probe2.c -o vk_probe2 -lvulkan
//
// Run: adb push vk_probe2 /data/local/tmp/ && adb shell /data/local/tmp/vk_probe2

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define CHECK(call) do { \
    VkResult _r = (call); \
    if (_r != VK_SUCCESS) { \
        fprintf(stderr, "%s:%d %s failed rc=%d\n", __FILE__, __LINE__, #call, _r); \
        return _r; \
    } \
} while (0)

int main(void)
{
    VkApplicationInfo appInfo = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "VipleStream-D20-Verify",
        .applicationVersion = 1,
        .pEngineName = "VipleStream",
        .engineVersion = 1,
        .apiVersion = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &appInfo,
    };
    VkInstance inst;
    CHECK(vkCreateInstance(&ici, NULL, &inst));

    uint32_t pdCount = 0;
    vkEnumeratePhysicalDevices(inst, &pdCount, NULL);
    if (pdCount == 0) { fprintf(stderr, "no physical devices\n"); return 1; }
    VkPhysicalDevice* pds = calloc(pdCount, sizeof(*pds));
    vkEnumeratePhysicalDevices(inst, &pdCount, pds);
    VkPhysicalDevice phys = pds[0];

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys, &props);
    printf("Device: %s (vendor 0x%04x)\n\n", props.deviceName, props.vendorID);

    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qfCount, NULL);
    VkQueueFamilyProperties* qf = calloc(qfCount, sizeof(*qf));
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qfCount, qf);

    // Pick first family that supports GFX (matches vk_backend.c choice).
    int chosen = -1;
    for (uint32_t i = 0; i < qfCount; i++) {
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            chosen = (int)i;
            break;
        }
    }
    if (chosen < 0) { fprintf(stderr, "no graphics family\n"); return 2; }
    uint32_t available = qf[chosen].queueCount;
    uint32_t want = available >= 3 ? 3 : available >= 2 ? 2 : 1;
    printf("Picked family %d: queueCount=%u (requesting %u)\n\n", chosen, available, want);

    float prios[3] = { 1.0f, 1.0f, 1.0f };
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = (uint32_t)chosen,
        .queueCount = want,
        .pQueuePriorities = prios,
    };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci,
    };
    VkDevice dev;
    CHECK(vkCreateDevice(phys, &dci, NULL, &dev));

    // Acquire each queue and dump handle.
    VkQueue queues[3] = {0};
    for (uint32_t i = 0; i < want; i++) {
        vkGetDeviceQueue(dev, (uint32_t)chosen, i, &queues[i]);
        printf("queue[%u] handle = %p\n", i, (void*)queues[i]);
    }

    // Distinctness check.
    int distinct[3][3] = {0};
    for (uint32_t i = 0; i < want; i++) {
        for (uint32_t j = i + 1; j < want; j++) {
            distinct[i][j] = (queues[i] != queues[j]);
        }
    }
    printf("\nHandle distinctness:\n");
    for (uint32_t i = 0; i < want; i++) {
        for (uint32_t j = i + 1; j < want; j++) {
            printf("  queue[%u] vs queue[%u]: %s\n", i, j,
                   distinct[i][j] ? "DIFFERENT" : "SAME (driver collapsed)");
        }
    }

    // Concurrent-submission smoke: submit empty cmd buffers to each queue
    // back-to-back without intervening fence wait.  Driver MUST accept
    // this even on devices that internally serialize same-family queues.
    VkCommandPool pool;
    VkCommandPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = (uint32_t)chosen,
    };
    CHECK(vkCreateCommandPool(dev, &pci, NULL, &pool));

    VkCommandBuffer cbs[3] = {0};
    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = want,
    };
    CHECK(vkAllocateCommandBuffers(dev, &cbai, cbs));

    for (uint32_t i = 0; i < want; i++) {
        VkCommandBufferBeginInfo bi = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        CHECK(vkBeginCommandBuffer(cbs[i], &bi));
        CHECK(vkEndCommandBuffer(cbs[i]));
    }

    VkFence fences[3] = {0};
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    for (uint32_t i = 0; i < want; i++) {
        CHECK(vkCreateFence(dev, &fci, NULL, &fences[i]));
    }

    printf("\nSubmitting empty cmd buffer to each queue (no waits between)...\n");
    for (uint32_t i = 0; i < want; i++) {
        VkSubmitInfo si = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &cbs[i],
        };
        VkResult r = vkQueueSubmit(queues[i], 1, &si, fences[i]);
        printf("  vkQueueSubmit(queue[%u]) = %s\n",
               i, r == VK_SUCCESS ? "OK" : "FAIL");
        if (r != VK_SUCCESS) {
            return 3;
        }
    }
    printf("\nWaiting for fences...\n");
    for (uint32_t i = 0; i < want; i++) {
        CHECK(vkWaitForFences(dev, 1, &fences[i], VK_TRUE, 1000000000ull));
        printf("  fence[%u] signaled\n", i);
    }

    printf("\n§I.D Phase D.2.0 verdict (Adreno verify):\n");
    int allDistinct = 1;
    for (uint32_t i = 0; i < want; i++)
        for (uint32_t j = i + 1; j < want; j++)
            if (!distinct[i][j]) allDistinct = 0;
    if (allDistinct && want >= 2) {
        printf("  [GO]   driver returned %u distinct VkQueue handles + accepted\n"
               "         concurrent same-family submits → Phase D.2.1+ multi-queue\n"
               "         submit pattern is API-viable. Real GPU parallelism still\n"
               "         needs streaming-session timing measurement.\n", want);
    } else if (want < 2) {
        printf("  [STOP] only %u queue available — Phase D.2.x infeasible.\n", want);
    } else {
        printf("  [DEGRADED] driver returned same handle for some indices —\n"
               "             same-family multi-queue collapses to single queue\n"
               "             on this device. Phase D.2.x cannot benefit.\n");
    }

    // Cleanup
    for (uint32_t i = 0; i < want; i++) vkDestroyFence(dev, fences[i], NULL);
    vkDestroyCommandPool(dev, pool, NULL);
    vkDestroyDevice(dev, NULL);
    free(qf);
    free(pds);
    vkDestroyInstance(inst, NULL);
    return 0;
}
