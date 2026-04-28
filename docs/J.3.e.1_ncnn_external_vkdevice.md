# §J.3.e.1 — ncnn fork patch：support external VkInstance/VkDevice

**Workstream:** §J 桌面 Vulkan-native client → §J.3.e NCNN-Vulkan 整合進 PlVkRenderer
**Status:** v1.3.64 §J.3.e.1.a header sync ship → 後續 b/c/d sub-commit
**Predecessors:** v1.3.63 (36e046f) §J.3.e.0 AVVkFrame access probe ship
**Successors:** §J.3.e.2 NV12→RGB compute shader + AVVkFrame → ncnn::VkMat input

---

## Context

§J.3.c.1 (v1.3.60) + §J.3.f (v1.3.62) 已經把整個 desktop client decoder + renderer
推到 Vulkan-native (libplacebo)：HEVC / H264 / AV1 三 codec 都實機跑通，no D3D11 in chain。

但 NCNN-Vulkan FRUC backend **還跑在 d3d11va.cpp 的 cascade**，只在 D3D11VARenderer
被選中時才 fire。VIPLE_USE_VK_DECODER=1 時整個 renderer 路徑換 PlVkRenderer，這條
path 沒 FRUC integration → user 拿不到 ML 補幀。

要把 NCNN 整合進 PlVkRenderer 的 renderFrame，**核心 architectural 問題是 VkDevice 共用：**

- ffmpeg's hwcontext_vulkan 把 AVVkFrame.img[0] (VkImage) 建在 libplacebo 的 VkDevice 上
- NCNN 自己 create_gpu_instance() 建另一個獨立 VkDevice
- 跨 VkDevice 共享 VkImage 要 OPAQUE_WIN32 export/import + ffmpeg AVHWFramesContext
  自定義 + 多一輪 copy → ~400 LOC 工程 + uncertainty

最乾淨：**讓 ncnn 直接用 libplacebo 的 VkDevice**。同一個 device，AVVkFrame.img[0]
直接給 ncnn::VkMat 包，無 copy 無 cross-device 同步。

但 ncnn 沒有「外部給的 VkDevice」這個 API — `VulkanDevice` ctor 永遠自己呼 `vkCreateDevice`。
本 design 加上這個 API。

---

## Goal

加 ncnn 公開 API：

```cpp
NCNN_EXPORT int ncnn::create_gpu_instance_external(VkInstance instance,
                                                    VkPhysicalDevice physicalDevice,
                                                    VkDevice device,
                                                    uint32_t computeQueueFamilyIndex,
                                                    uint32_t computeQueueCount,
                                                    uint32_t graphicsQueueFamilyIndex,
                                                    uint32_t graphicsQueueCount,
                                                    uint32_t transferQueueFamilyIndex,
                                                    uint32_t transferQueueCount);
```

呼叫後：

- `ncnn::Net::set_vulkan_device(0)` 拿到的 `VulkanDevice` 內部 d->device 等於 caller 提供的
  external VkDevice
- 所有 ncnn 後續操作 (extract / VkCompute / VkTransfer / Pipeline create) 都跑在 external
  VkDevice
- `ncnn::destroy_gpu_instance()` 釋放 ncnn-allocated 資源（pipeline cache / 取得的 queue
  refs / texelfetch sampler / dummy buffer）但**不**呼 `vkDestroyDevice` 跟 `vkDestroyInstance`
  — caller 控制這兩個 top-level handle 的 lifetime。

成功條件：

1. ncnn.dll 重 build 過、不破壞既有 NcnnFRUC backend（regression test：720p NCNN
   cascade probe 在沒設 VIPLE_USE_VK_DECODER 時行為跟 v1.3.63 等價）
2. 新 API 有 minimal 範例使用：NcnnFRUC 加 alt ctor / init path 可以收 external 三件組
   (VkInstance, VkPhysicalDevice, VkDevice) 並把 ncnn instance 接上去
3. PlVkRenderer 啟用時若 NCNN backend 想接，可以直接拿 m_PlVkInstance->instance / 
   m_Vulkan->phys_device / m_Vulkan->device / queue family indices 餵給 ncnn

---

## Patch surface

### File 1: `src/gpu.h` (header — 公開新 API)

加在 `create_gpu_instance / destroy_gpu_instance` 後面：

```cpp
// VipleStream §J.3.e.1 — external VkDevice support
NCNN_EXPORT int create_gpu_instance_external(
    VkInstance instance,
    VkPhysicalDevice physicalDevice,
    VkDevice device,
    uint32_t computeQueueFamilyIndex,
    uint32_t computeQueueCount,
    uint32_t graphicsQueueFamilyIndex,
    uint32_t graphicsQueueCount,
    uint32_t transferQueueFamilyIndex,
    uint32_t transferQueueCount);
```

### File 2: `src/gpu.cpp` (主 patch ~200 LOC)

#### 2.1 新增全域 flag

```cpp
// 86 行 g_default_vkdev[] 後面加
static bool g_external_mode = false;
```

#### 2.2 拆 `VulkanDevice::VulkanDevice(int device_index)` 成兩段

目前 ctor 一鼓作氣做完 vkCreateDevice + 所有 post-init。拆成：

```cpp
// 私有 helper — 跑 vkCreateDevice 之前的 extension/feature 列舉
// (gpu.cpp:1898-2127 那段，包到 build_create_info 函式)
class VulkanDevice {
private:
    void build_device_create_info(VkDeviceCreateInfo& dci,
                                   std::vector<const char*>& enabledExtensions,
                                   /* ... feature struct refs ... */);
    void init_post_device_create();  // 2128-2196 那段
public:
    VulkanDevice(int device_index);  // 既有 — 內部呼 build_device_create_info → vkCreateDevice → init_post_device_create
    VulkanDevice(int device_index, VkDevice externalDevice);  // 新加 — 跳過 vkCreateDevice，直接 init_post_device_create
};
```

關鍵：`init_post_device_create()` 內部 do **不**重新跑 vkCreateDevice，但仍要：
- `init_device_extension()` 載 function pointer
- `vkGetDeviceQueue()` 取得 compute/graphics/transfer queues
- 創 texelfetch_sampler
- `create_dummy_buffer_image()`
- `new PipelineCache(this)`
- `memset(d->uop_packing, 0, ...)`

新 ctor body：

```cpp
VulkanDevice::VulkanDevice(int device_index, VkDevice externalDevice)
    : info(get_gpu_info(device_index)), d(new VulkanDevicePrivate(this))
{
    d->device = externalDevice;
    d->is_external = true;  // 新加 private bool
    init_post_device_create();
}
```

#### 2.3 dtor 處理 external

```cpp
VulkanDevice::~VulkanDevice() {
    // 既有 cleanup ...
    if (d->texelfetch_sampler) vkDestroySampler(...);
    d->destroy_dummy_buffer_image();
    delete d->pipeline_cache;
    // ...
    
    // 修改：external 不呼 vkDestroyDevice
    if (!d->is_external) {
        vkDestroyDevice(d->device, 0);
    }
    delete d;
}
```

#### 2.4 `destroy_gpu_instance()` 處理 external

```cpp
void destroy_gpu_instance() {
    MutexLockGuard lock(g_instance_lock);
    if ((VkInstance)g_instance == 0) return;
    
    glslang::FinalizeProcess();
    
    for (int i = 0; i < NCNN_MAX_GPU_COUNT; i++) {
        delete g_default_vkdev[i];  // ← 這個 delete 觸發 VulkanDevice dtor，has external check
        g_default_vkdev[i] = 0;
        delete g_gpu_infos[i];
        g_gpu_infos[i] = 0;
    }
    
    // 修改：external 不呼 vkDestroyInstance
    if (!g_external_mode) {
        vkDestroyInstance(g_instance, 0);
    }
    g_instance.instance = 0;
    g_external_mode = false;  // reset for next session
}
```

#### 2.5 GpuInfo populate from external — 新 helper

`get_gpu_info(0)` 期望 `g_gpu_infos[0]` 已經填好。標準 `create_gpu_instance` 透過
`vkEnumeratePhysicalDevices` + `vkGetPhysicalDeviceProperties` 等填。external 模式下要
從 caller 給的 VkPhysicalDevice 重新 enumerate 一次。

新加 static helper：

```cpp
static int populate_gpu_info_from_external(int idx,
                                            VkPhysicalDevice physDev,
                                            uint32_t computeQF, uint32_t computeQC,
                                            uint32_t graphicsQF, uint32_t graphicsQC,
                                            uint32_t transferQF, uint32_t transferQC)
{
    GpuInfo* gi = new GpuInfo;
    gi->d->physical_device = physDev;
    
    // 抄 create_gpu_instance 從 vkGetPhysicalDeviceProperties 等填的部分
    // (gpu.cpp 1100-1500 區段，去掉 device 列舉部分，直接對 physDev 跑)
    vkGetPhysicalDeviceProperties(physDev, &gi->d->physicalDeviceProperties);
    vkGetPhysicalDeviceMemoryProperties(physDev, &gi->d->memoryProperties);
    // ... feature 列舉 ...
    // ... extension 列舉 ...
    // ... subgroup_size / driver_version 等 ...
    
    // queue family 直接用 caller 給的，不靠 vkGetPhysicalDeviceQueueFamilyProperties
    gi->d->compute_queue_family_index = computeQF;
    gi->d->compute_queue_count = computeQC;
    gi->d->graphics_queue_family_index = graphicsQF;
    gi->d->graphics_queue_count = graphicsQC;
    gi->d->transfer_queue_family_index = transferQF;
    gi->d->transfer_queue_count = transferQC;
    
    g_gpu_infos[idx] = gi;
    return 0;
}
```

> ⚠️ **GpuInfoPrivate 是 ncnn private class — 此 helper 必須跟 GpuInfo 同 namespace 寫進
> gpu.cpp，不能放 header**。可能要把部分 vkGet*Properties 邏輯從現有 create_gpu_instance
> 抽出共用。

#### 2.6 `create_gpu_instance_external` 主函式

```cpp
int create_gpu_instance_external(
    VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device,
    uint32_t cQF, uint32_t cQC, uint32_t gQF, uint32_t gQC, uint32_t tQF, uint32_t tQC)
{
    MutexLockGuard lock(g_instance_lock);
    
    if ((VkInstance)g_instance != 0) {
        NCNN_LOGE("create_gpu_instance_external: ncnn already initialized");
        return -1;
    }
    
    g_instance.instance = instance;
    g_external_mode = true;
    
    // 載入 instance-level function pointers (vkGetPhysicalDeviceProperties2 等)
    // 既有 create_gpu_instance() 在 vkCreateInstance 後面那段抽出來
    init_instance_extensions(instance);
    
    // 填 g_gpu_infos[0]
    if (populate_gpu_info_from_external(0, physicalDevice, cQF, cQC, gQF, gQC, tQF, tQC) != 0) {
        g_instance.instance = 0;
        g_external_mode = false;
        return -1;
    }
    g_gpu_count = 1;
    g_default_gpu_index = 0;
    
    // 創 VulkanDevice
    g_default_vkdev[0] = new VulkanDevice(0, device);
    
    glslang::InitializeProcess();
    return 0;
}
```

---

## File 3: `src/gpu.h` 同步加 `VulkanDevice` 第二個 ctor 公開（給 cast 用）

```cpp
class NCNN_EXPORT VulkanDevice {
public:
    VulkanDevice(int device_index = get_default_gpu_index());
    VulkanDevice(int device_index, VkDevice externalDevice);  // 新加
    // ...
};
```

---

## Risk + mitigation

### R1: `init_device_extension()` 假設 ncnn 自己 enable 的 extension list

ncnn 透過 `enabledExtensions` vector 內部追蹤啟用了什麼 ext，後續 `init_device_extension()`
基於這個 list 載 function pointer。external 模式下這個 list 沒填過 → 有些 PFN 載不到 → ncnn
某些路徑 NULL deref。

**Mitigation**: external ctor 仍跑 `gpu.cpp:1903-1972` 那段「列舉所有可選 extension 並 push 到 enabledExtensions」（**但不**真的呼 vkCreateDevice）。其實這段邏輯只是讀 GpuInfo 字段然後 push，不會嘗試實際 enable — 純資料 prep。再呼 `init_device_extension` 就會載到正確的 PFN。

### R2: queue 取得 — `vkGetDeviceQueue` 對 external device

`init_post_device_create()` 內部 `vkGetDeviceQueue(d->device, ...)` 是 device-level call，跟誰
創建的無關。caller 必須保證該 family 真的有那麼多 queue（caller 創 device 時 要列在
VkDeviceQueueCreateInfo）。

**Mitigation**: design doc 明寫此假設。caller (PlVkRenderer wrapper) 必須先確認 libplacebo
創 device 時 enable 了夠多 compute/graphics/transfer queue。實際上 libplacebo 預設只 enable
一個 queue per family — 可能要改 libplacebo 的 pl_vulkan_create params。

### R3: g_external_mode 跨 destroy/recreate cycle

v1.3.44 修了 ncnn destroy/recreate 不穩 → 改成 process-lifetime singleton。external 模式下
g_external_mode 需要 reset 才能下次再用。已在 destroy_gpu_instance 結尾 reset。

### R4: glslang::InitializeProcess 也是 external 嗎？

glslang 是 ncnn 內部用，跟 external VkDevice 無關。external 模式仍要 InitializeProcess
（編譯 SPIR-V）/ FinalizeProcess（清乾淨）。這部分照舊。

### R5: ncnn-allocated VkPipeline/VkBuffer 都跑 external device

ncnn 的 pipeline / blob_allocator / staging_allocator 用 external VkDevice 來 vkCreate*。
ncnn dtor 用 same external device 來 vkDestroy*。這個對稱關係 in vkDevice lifetime 內成立
（caller 必須保證 ncnn shutdown 在 caller destroy device 之前）。design doc 明寫此 ordering 要求。

### R6: g_instance_lock 在 Windows 是 SRWLock — 不可重入（實作時發現）

`create_gpu_instance_external` 一開始持 `g_instance_lock` 設置 g_instance.instance / 全域
`support_VK_KHR_*` flags / 呼叫 init_instance_extension / populate_gpu_info_from_external，
結尾要 `new VulkanDevice(0, externalDevice)` 把第二個 ctor 觸發。

該 ctor 的 initializer list 走 `info(get_gpu_info(0))` → `try_create_gpu_instance()`
→ `is_gpu_instance_ready()`，後者 `MutexLockGuard lock(g_instance_lock)`。在 Windows 上
ncnn 的 `Mutex` 包 `SRWLOCK`（`AcquireSRWLockExclusive`），非 recursive，**同 thread 第二次
acquire 即 deadlock**。

**Mitigation**: `create_gpu_instance_external` 把 g_instance_lock 限定在「設置全域狀態 +
populate g_gpu_infos」的 scope 內，scope 結束 lock 釋放，**之後**才 `new VulkanDevice`。
此時 `try_create_gpu_instance` 看到 g_instance 已非 0 直接 short-circuit，不會去 vkCreateInstance。

### R7: libplacebo 的 VkInstance 不 enable VK_KHR_get_physical_device_properties2（§J.3.e.1.d 實測）

ncnn 的 `init_instance_extension` 用 `vkGetInstanceProcAddr(g_instance, "vkGetPhysicalDeviceProperties2KHR")`
（KHR-suffixed 名）載 PFN。Vulkan spec 規定該 API 對 KHR alias 只在「instance 創建時 enable
了該 KHR extension」才會回非 NULL。在 Vulkan 1.1+ 該 functionality 已是 core，libplacebo
（也包括 ffmpeg 的 hwcontext_vulkan）建立 instance 時**不**勞煩 enable 此 KHR ext，於是
`vkGetPhysicalDeviceProperties2KHR` PFN 回 NULL → populate_gpu_info_from_external 在
subgroup 探測時 NULL deref。實測 §J.3.e.1.d 第一次 wire 時 vkQueueSubmit 之前就 0xC0000005。

**Mitigation**: `create_gpu_instance_external` 在 `init_instance_extension()` 之後，補一段
fallback：如果 KHR-suffixed PFN 沒載到，改用 unsuffixed core name (`vkGetPhysicalDeviceProperties2` /
`vkGetPhysicalDeviceFeatures2` / `vkGetPhysicalDeviceMemoryProperties2` /
`vkGetPhysicalDeviceQueueFamilyProperties2`) 重試一次。如果這次成功，把
`support_VK_KHR_get_physical_device_properties2 = 1` 標起來讓後續 populate 跑該 path。
v1.3.71 ship 此修正後 §J.3.e.1.d handoff log 印 `ncnn d->device=000001245A7C46F0
== m_Vulkan->device=000001245A7C46F0` 確認 RTX 3060 上 wire 通了。

---

## Testing strategy

### Phase 1: build smoke

`scripts/build_ncnn.cmd` (新加) 跑 nmake build_v 重 build ncnn.dll。確認沒 compile error /
linker error。Copy 新 dll 到 `moonlight-qt/libs/windows/ncnn/runtimes/win-x64/native/`.

### Phase 2: regression test (沒設 VIPLE_USE_VK_DECODER)

跑既有 720p NCNN cascade test：

```powershell
[Environment]::SetEnvironmentVariable("VIPLE_FRUC_NCNN_SHARED", "1", "Process")
& VipleStream.exe stream <host-ip> Desktop --720 --fps 60 --fruc-backend ncnn
```

期望 log 跟 v1.3.63 等價：
- §J.3.a video decode probe 全 OK
- loadModel step 1/6 → 6/6 完整跑
- ncnn cascade probe ~14ms (720p)
- destroy() 乾淨

### Phase 3: external API 試 wire（不接 PlVkRenderer，只測 API）

加 throwaway probe 在 NcnnFRUC::initialize：自己創一個 VkInstance/Device，呼
`ncnn::create_gpu_instance_external(...)`，看 ncnn 能不能 load model + run inference。

```cpp
// 在 NcnnFRUC::initialize 開頭，VIPLE_NCNN_EXTERNAL_PROBE=1 時
if (probeEnv) {
    VkInstance inst; VkPhysicalDevice phys; VkDevice dev;
    create_minimal_vk_instance(&inst, &phys, &dev);  // throwaway
    ncnn::create_gpu_instance_external(inst, phys, dev, /*qf*/ 0, 1, 0, 1, 0, 1);
    
    auto* probeNet = new ncnn::Net;
    probeNet->set_vulkan_device(0);
    int rc = probeNet->load_param("rife-v4.25-lite/flownet.param");
    SDL_LogInfo(... "external probe load_param rc=%d", rc);
    delete probeNet;
}
```

### Phase 4: 真正 wire (留 §J.3.e.2)

PlVkRenderer 收 callback 把 VkInstance/Device 給 NcnnFRUC，後者呼 external API。

---

## Out of scope (留給 §J.3.e.2+)

- AVVkFrame.img[0] (NV12 multi-plane) → ncnn::VkMat (fp32 RGB) 的 GLSL compute shader
- VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR → SHADER_READ_OPTIMAL transition
- NCNN inference forward 接 AVVkFrame
- ncnn 輸出 → libplacebo present (pl_frame wrapping)
- Frame pacing (interp at midpoint + real)

---

## Rebuild ncnn 流程備忘

ncnn source 在 `C:\Users\<user>\AppData\Local\Temp\rife_src\src\ncnn\`。build dir 是
`build_v/`（NMake + MSVC 2022）。

```powershell
$vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
$buildDir = "C:\Users\<user>\AppData\Local\Temp\rife_src\src\ncnn\build_v"
& cmd /c "call `"$vcvars`" && cd /d `"$buildDir`" && nmake ncnn"
```

build 完拷貝：
```powershell
cp "$buildDir\src\ncnn.dll" "D:\Mission\VipleStream\moonlight-qt\libs\windows\ncnn\runtimes\win-x64\native\ncnn.dll"
cp "$buildDir\src\ncnn.lib" "D:\Mission\VipleStream\moonlight-qt\libs\windows\ncnn\runtimes\win-x64\native\ncnn.lib"
```

也要更新 `moonlight-qt/libs/windows/ncnn/build/native/include/ncnn/gpu.h` 加新 API
declaration（手動 sync from src/gpu.h）。

---

## 既有 ncnn patch 可參考

| Patch | Commit | Lines added |
|---|---|---|
| v1.3.30 — VK_KHR_external_memory_win32 enable | 7bd838d | gpu.cpp +1 line `enabledExtensions.push_back("VK_KHR_external_memory_win32")` |
| v1.3.45 — timeline_semaphore + external_semaphore_win32 | bd5fe1b | gpu.cpp +12 lines (3 ext + feature struct) |

這兩個都單純加 push_back，沒動 ctor 結構。本 patch 是第一次動 ctor — 風險級別更高。

---

## Commit 規劃

| Commit | 內容 |
|---|---|
| §J.3.e.1.a — patch ncnn ctor + add external API | gpu.h + gpu.cpp 兩個檔，~250 LOC |
| §J.3.e.1.b — rebuild ncnn.dll + update libs | 純 binary update + gpu.h sync |
| §J.3.e.1.c — NcnnFRUC throwaway external probe | ~60 LOC，gated 在 VIPLE_NCNN_EXTERNAL_PROBE=1 |
| §J.3.e.1.d — wire NcnnFRUC ↔ PlVkRenderer external device handoff | ~80 LOC |

每個 commit 都要過 build + regression test (720p NCNN cascade probe 等價 v1.3.63)。
