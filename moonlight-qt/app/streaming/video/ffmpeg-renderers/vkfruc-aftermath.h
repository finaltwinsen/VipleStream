// §J.3.e.2.i.8 Phase 1.6 — NVIDIA Nsight Aftermath SDK integration helper.
//
// Standalone GPU crash dump collector.  When the Vulkan device goes into
// VK_ERROR_DEVICE_LOST (typically NV TDR), Aftermath's signal handler kicks
// in and dumps a `.nv-gpudmp` file containing the last in-flight GPU work
// (which cmd buffer, which dispatch / draw, what the driver was trying to
// execute).  The dump file is loadable by Nsight Graphics for visualization.
//
// Compiles to a no-op when SDK is absent (`!VIPLESTREAM_HAVE_AFTERMATH`),
// so the same source builds in CI / dev-tool-less environments.
//
// API:
//   AftermathDumpCollector::Ensure()  — idempotent, call once before vkCreateInstance.
//                                       Returns true if Aftermath is active.
//   AftermathDumpCollector::IsActive() — query.
//
// Dump location: %TEMP%\VipleStream-aftermath-<unix-ts>.nv-gpudmp

#pragma once

namespace VipleAftermath {

// Idempotent — first call enables Aftermath GPU crash dump collection
// (Vulkan API watch).  Must be called BEFORE any vkCreateDevice.  Subsequent
// calls are no-ops and just return the prior state.  Returns true if SDK is
// active and ready to dump on next device-lost.
bool Ensure();

// Whether Aftermath is currently active (i.e. SDK loaded + Ensure succeeded).
bool IsActive();

}  // namespace VipleAftermath
