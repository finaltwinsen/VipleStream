// §J.3.e.2.i.8 Phase 1.6 — NVIDIA Nsight Aftermath SDK integration.
// See vkfruc-aftermath.h for high-level usage.
//
// SDK reference: 3rdparty/aftermath_sdk/Readme.md + GFSDK_Aftermath_GpuCrashDump.h.
// Sample code:    github.com/NVIDIA/nsight-aftermath-samples (VkHelloNsightAftermath).

#include "vkfruc-aftermath.h"

#include <SDL_log.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#ifdef VIPLESTREAM_HAVE_AFTERMATH
#include "GFSDK_Aftermath.h"
#include "GFSDK_Aftermath_GpuCrashDump.h"
#endif

namespace VipleAftermath {

namespace {

std::atomic<bool> g_active{false};
std::once_flag    g_initOnce;

#ifdef VIPLESTREAM_HAVE_AFTERMATH

// %TEMP% (or fallback to current dir) joined with our prefix + timestamp.
static std::string makeDumpPath(const char* extension)
{
    const char* tempDir = std::getenv("TEMP");
    if (!tempDir || !*tempDir) tempDir = std::getenv("TMP");
    if (!tempDir || !*tempDir) tempDir = ".";

    auto now = std::chrono::system_clock::now();
    auto ts  = std::chrono::duration_cast<std::chrono::seconds>(
                   now.time_since_epoch()).count();

    char buf[1024];
    std::snprintf(buf, sizeof(buf), "%s\\VipleStream-aftermath-%lld.%s",
                  tempDir, (long long)ts, extension);
    return std::string(buf);
}

static void GFSDK_AFTERMATH_CALL gpuCrashDumpCb(const void* pDump,
                                                 const uint32_t dumpSize,
                                                 void* /*pUserData*/)
{
    // Called from Aftermath worker thread after device-lost / TDR.  Write
    // the dump bytes to disk so user can load it into Nsight Graphics.
    auto path = makeDumpPath("nv-gpudmp");
    FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-AFTERMATH] failed to open dump file %s for write",
                     path.c_str());
        return;
    }
    std::fwrite(pDump, 1, dumpSize, fp);
    std::fclose(fp);
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                 "[VIPLE-AFTERMATH] GPU crash dump written: %s (%u bytes) — "
                 "load in Nsight Graphics to inspect last in-flight cmd buffer",
                 path.c_str(), (unsigned)dumpSize);
}

static void GFSDK_AFTERMATH_CALL shaderDebugInfoCb(const void* pInfo,
                                                    const uint32_t infoSize,
                                                    void* /*pUserData*/)
{
    // Driver-side shader debug info (PDB-equivalent for SPIR-V).  Save it
    // alongside the dump so Nsight Graphics can resolve shader sources.
    auto path = makeDumpPath("nv-shaderdbg");
    FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) return;
    std::fwrite(pInfo, 1, infoSize, fp);
    std::fclose(fp);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-AFTERMATH] shader debug info written: %s (%u bytes)",
                path.c_str(), (unsigned)infoSize);
}

static void GFSDK_AFTERMATH_CALL descriptionCb(
    PFN_GFSDK_Aftermath_AddGpuCrashDumpDescription addValue,
    void* /*pUserData*/)
{
    // Stamp the dump with our app identity so Nsight UI shows context.
    addValue(GFSDK_Aftermath_GpuCrashDumpDescriptionKey_ApplicationName,
             "VipleStream-Client");
    addValue(GFSDK_Aftermath_GpuCrashDumpDescriptionKey_ApplicationVersion,
             "Phase 1.6 ONLY-mode TDR diagnosis");
}

static void doInit()
{
    GFSDK_Aftermath_Result rc = GFSDK_Aftermath_EnableGpuCrashDumps(
        GFSDK_Aftermath_Version_API,
        GFSDK_Aftermath_GpuCrashDumpWatchedApiFlags_Vulkan,
        GFSDK_Aftermath_GpuCrashDumpFeatureFlags_Default,
        gpuCrashDumpCb,
        shaderDebugInfoCb,
        descriptionCb,
        /*resolveMarkerCb*/ nullptr,
        /*pUserData*/ nullptr);
    if (rc == GFSDK_Aftermath_Result_Success) {
        g_active.store(true, std::memory_order_release);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-AFTERMATH] Nsight Aftermath SDK enabled — "
                    "GPU crash dumps will land in %%TEMP%%\\VipleStream-"
                    "aftermath-<ts>.nv-gpudmp on next device-lost / TDR");
    } else {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-AFTERMATH] EnableGpuCrashDumps failed rc=%d "
                    "(continuing without crash dump collection)",
                    (int)rc);
    }
}

#else  // !VIPLESTREAM_HAVE_AFTERMATH

static void doInit()
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-AFTERMATH] SDK not compiled in (define "
                "VIPLESTREAM_HAVE_AFTERMATH and link GFSDK_Aftermath_Lib.x64.lib "
                "to enable GPU crash dump collection)");
}

#endif

}  // namespace

bool Ensure()
{
    std::call_once(g_initOnce, doInit);
    return g_active.load(std::memory_order_acquire);
}

bool IsActive()
{
    return g_active.load(std::memory_order_acquire);
}

}  // namespace VipleAftermath
