/**
 * @file src/nvenc/nvenc_d3d11.cpp
 * @brief Definitions for abstract Direct3D11 NVENC encoder.
 */
// local includes
#include "src/logging.h"

#ifdef _WIN32
  #include "nvenc_d3d11.h"

namespace nvenc {

  nvenc_d3d11::nvenc_d3d11(NV_ENC_DEVICE_TYPE device_type):
      nvenc_base(device_type) {
    async_event_handle = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!async_event_handle) {
      BOOST_LOG(error) << "NvEnc: CreateEvent() failed for async event handle";
    }
  }

  nvenc_d3d11::~nvenc_d3d11() {
    // 確保在卸載 nvEncodeAPI DLL 之前銷毀 encoder，避免調用已卸載模組之函式指針崩潰
    if (encoder) {
      destroy_encoder();
    }
    if (dll) {
      FreeLibrary(dll);
      dll = nullptr;
    }
    if (async_event_handle) {
      CloseHandle(async_event_handle);
      async_event_handle = nullptr;
    }
  }

  bool nvenc_d3d11::init_library() {
    if (dll) {
      return true;
    }

  #ifdef _WIN64
    constexpr auto dll_name = "nvEncodeAPI64.dll";
  #else
    constexpr auto dll_name = "nvEncodeAPI.dll";
  #endif

    if ((dll = LoadLibraryEx(dll_name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))) {
      if (auto create_instance = (decltype(NvEncodeAPICreateInstance) *) GetProcAddress(dll, "NvEncodeAPICreateInstance")) {
        auto new_nvenc = std::make_unique<NV_ENCODE_API_FUNCTION_LIST>();
        new_nvenc->version = min_struct_version(NV_ENCODE_API_FUNCTION_LIST_VER, sdk12_ver::function_list);
        if (nvenc_failed(create_instance(new_nvenc.get()))) {
          BOOST_LOG(error) << "NvEnc: NvEncodeAPICreateInstance() failed: " << last_nvenc_error_string;
        } else {
          nvenc = std::move(new_nvenc);
          return true;
        }
      } else {
        BOOST_LOG(error) << "NvEnc: No NvEncodeAPICreateInstance() in " << dll_name;
      }
    } else {
      BOOST_LOG(debug) << "NvEnc: Couldn't load NvEnc library " << dll_name;
    }

    if (dll) {
      FreeLibrary(dll);
      dll = nullptr;
    }

    return false;
  }

  bool nvenc_d3d11::wait_for_async_event(uint32_t timeout_ms) {
    return WaitForSingleObject(async_event_handle, timeout_ms) == WAIT_OBJECT_0;
  }

  // §NVENC-SPLIT：在 13.0 與 11/12 之間切換時由 create_encoder 呼叫（預設路徑不會呼叫）
  bool nvenc_d3d11::recreate_function_list() {
    if (!dll) {
      return false;
    }
    auto create_instance = (decltype(NvEncodeAPICreateInstance) *) GetProcAddress(dll, "NvEncodeAPICreateInstance");
    if (!create_instance) {
      return false;
    }
    auto new_nvenc = std::make_unique<NV_ENCODE_API_FUNCTION_LIST>();
    new_nvenc->version = min_struct_version(NV_ENCODE_API_FUNCTION_LIST_VER, sdk12_ver::function_list);
    if (nvenc_failed(create_instance(new_nvenc.get()))) {
      BOOST_LOG(error) << "NvEnc: NvEncodeAPICreateInstance() failed: " << last_nvenc_error_string;
      return false;
    }
    nvenc = std::move(new_nvenc);
    return true;
  }

  // §NVENC-SPLIT：只有 nvenc_split_encode 開著時才會呼叫
  uint32_t nvenc_d3d11::driver_max_api_version() {
    if (!dll) {
      return 0;
    }
    auto get_max_version = (decltype(NvEncodeAPIGetMaxSupportedVersion) *) GetProcAddress(dll, "NvEncodeAPIGetMaxSupportedVersion");
    if (!get_max_version) {
      return 0;
    }
    uint32_t version = 0;
    if (get_max_version(&version) != NV_ENC_SUCCESS) {
      return 0;
    }
    return version;
  }

}  // namespace nvenc
#endif
