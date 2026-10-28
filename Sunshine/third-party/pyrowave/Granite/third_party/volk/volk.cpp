// VipleStream：volk 以 C++ 編譯並開 VOLK_NAMESPACE，vk* 函式指標放進 namespace volk。
// 不然 volk 的全域變數 vkCreateInstance… 會跟同一個執行檔連結的 Vulkan 載入器（Linux 的
// libvulkan、libplacebo）裡同名的函式撞名（Linux LTO 連結失敗；不開 LTO 時更糟，會綁錯符號）。
#ifndef VOLK_NAMESPACE
#define VOLK_NAMESPACE
#endif
#include "volk.c"
