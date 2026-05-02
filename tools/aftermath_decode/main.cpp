// §J.3.e.2.i.8 Phase 1.6b — Aftermath dump decoder CLI
//
// Standalone utility to decode a .nv-gpudmp captured by VipleStream's
// Aftermath integration into a JSON report.  Use this when you want a
// quick textual look at the GPU crash (page fault address, faulted warps,
// active shaders) without firing up Nsight Graphics.
//
// Build:
//   tools\aftermath_decode\build.cmd
//   -> emits aftermath_decode.exe in this folder
//
// Usage:
//   aftermath_decode.exe <path-to-dump.nv-gpudmp> [output.json]
//   - If output.json is omitted, JSON is written to stdout.
//   - exit code 0 on success, non-zero on failure.
//
// Implementation notes:
//   - We don't implement shader-debug-info / shader-binary / shader-source
//     callbacks — VipleStream doesn't ship per-PSO shader debug archives.
//     The decoder still produces useful PAGE_FAULT_INFO / FAULTED_WARP_INFO /
//     base / device / GPU / driver sections without them.
//   - ALL_INFO = 0x3FFF (every section the SDK supports).

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "GFSDK_Aftermath.h"
#include "GFSDK_Aftermath_GpuCrashDump.h"
#include "GFSDK_Aftermath_GpuCrashDumpDecoding.h"

static const char* resultName(GFSDK_Aftermath_Result rc)
{
    switch (rc) {
    case GFSDK_Aftermath_Result_Success:                   return "Success";
    case GFSDK_Aftermath_Result_NotAvailable:              return "NotAvailable";
    case GFSDK_Aftermath_Result_Fail:                      return "Fail";
    case GFSDK_Aftermath_Result_FAIL_VersionMismatch:      return "FAIL_VersionMismatch";
    case GFSDK_Aftermath_Result_FAIL_NotInitialized:       return "FAIL_NotInitialized";
    case GFSDK_Aftermath_Result_FAIL_InvalidAdapter:       return "FAIL_InvalidAdapter";
    case GFSDK_Aftermath_Result_FAIL_InvalidParameter:     return "FAIL_InvalidParameter";
    case GFSDK_Aftermath_Result_FAIL_Unknown:              return "FAIL_Unknown";
    case GFSDK_Aftermath_Result_FAIL_ApiError:             return "FAIL_ApiError";
    case GFSDK_Aftermath_Result_FAIL_OutOfMemory:          return "FAIL_OutOfMemory";
    case GFSDK_Aftermath_Result_FAIL_DriverVersionNotSupported: return "FAIL_DriverVersionNotSupported";
    default: {
        static char buf[32];
        std::snprintf(buf, sizeof(buf), "0x%08X", (unsigned)rc);
        return buf;
    }
    }
}

#define CHECK_AM(call) do {                                                  \
    GFSDK_Aftermath_Result _rc = (call);                                     \
    if (_rc != GFSDK_Aftermath_Result_Success) {                             \
        std::fprintf(stderr, "[aftermath_decode] %s failed: %s\n",           \
                     #call, resultName(_rc));                                \
        return 2;                                                            \
    }                                                                        \
} while (0)

int main(int argc, char** argv)
{
    if (argc < 2 || argc > 3) {
        std::fprintf(stderr,
            "Usage: %s <dump.nv-gpudmp> [output.json]\n", argv[0]);
        return 1;
    }
    const char* inPath  = argv[1];
    const char* outPath = (argc == 3) ? argv[2] : nullptr;

    // Slurp the dump file into memory.
    FILE* fp = std::fopen(inPath, "rb");
    if (!fp) {
        std::fprintf(stderr, "[aftermath_decode] cannot open %s\n", inPath);
        return 1;
    }
    std::fseek(fp, 0, SEEK_END);
    long sz = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    if (sz <= 0) {
        std::fprintf(stderr, "[aftermath_decode] empty / unreadable dump\n");
        std::fclose(fp);
        return 1;
    }
    std::vector<uint8_t> dump((size_t)sz);
    if (std::fread(dump.data(), 1, (size_t)sz, fp) != (size_t)sz) {
        std::fprintf(stderr, "[aftermath_decode] short read\n");
        std::fclose(fp);
        return 1;
    }
    std::fclose(fp);
    std::fprintf(stderr, "[aftermath_decode] loaded %ld bytes from %s\n",
                 sz, inPath);

    // Create decoder.
    GFSDK_Aftermath_GpuCrashDump_Decoder decoder = {};
    CHECK_AM(GFSDK_Aftermath_GpuCrashDump_CreateDecoder(
        GFSDK_Aftermath_Version_API,
        dump.data(),
        (uint32_t)dump.size(),
        &decoder));

    // Print a one-line summary to stderr so the user sees something even
    // when piping JSON through jq.
    GFSDK_Aftermath_GpuCrashDump_BaseInfo base = {};
    if (GFSDK_Aftermath_GpuCrashDump_GetBaseInfo(decoder, &base)
        == GFSDK_Aftermath_Result_Success) {
        std::fprintf(stderr,
            "[aftermath_decode] app=\"%s\" date=\"%s\" pid=%u api=%d\n",
            base.applicationName, base.creationDate,
            (unsigned)base.pid, (int)base.graphicsApi);
    }

    // Generate JSON with every section the SDK supports.  No shader-debug
    // callbacks — sections that need them are simply omitted.
    uint32_t jsonSize = 0;
    CHECK_AM(GFSDK_Aftermath_GpuCrashDump_GenerateJSON(
        decoder,
        GFSDK_Aftermath_GpuCrashDumpDecoderFlags_ALL_INFO,
        GFSDK_Aftermath_GpuCrashDumpFormatterFlags_UTF8_OUTPUT,
        /*shaderDebugInfoLookupCb*/        nullptr,
        /*shaderLookupCb*/                  nullptr,
        /*shaderSourceDebugInfoLookupCb*/   nullptr,
        /*pUserData*/                       nullptr,
        &jsonSize));

    if (jsonSize == 0) {
        std::fprintf(stderr, "[aftermath_decode] decoder returned 0-byte JSON\n");
        GFSDK_Aftermath_GpuCrashDump_DestroyDecoder(decoder);
        return 3;
    }

    std::vector<char> json((size_t)jsonSize);
    CHECK_AM(GFSDK_Aftermath_GpuCrashDump_GetJSON(
        decoder, jsonSize, json.data()));

    // Strip any trailing NUL the SDK may have put on the end (the SDK
    // includes the NUL in the size).
    size_t writeSize = json.size();
    while (writeSize > 0 && json[writeSize - 1] == '\0') --writeSize;

    if (outPath) {
        FILE* outFp = std::fopen(outPath, "wb");
        if (!outFp) {
            std::fprintf(stderr, "[aftermath_decode] cannot create %s\n", outPath);
            GFSDK_Aftermath_GpuCrashDump_DestroyDecoder(decoder);
            return 1;
        }
        std::fwrite(json.data(), 1, writeSize, outFp);
        std::fclose(outFp);
        std::fprintf(stderr, "[aftermath_decode] wrote %zu bytes JSON -> %s\n",
                     writeSize, outPath);
    } else {
        std::fwrite(json.data(), 1, writeSize, stdout);
        std::fputc('\n', stdout);
    }

    GFSDK_Aftermath_GpuCrashDump_DestroyDecoder(decoder);
    return 0;
}
