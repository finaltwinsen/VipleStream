// seh_guard.h - VipleStream §VR：driver 回呼的 crash containment（設計 §E.2 driver_main 的 drv-M-8）。
//
// 規則：
//  1. 每個 SteamVR 回呼入口「在取鎖之前」先看 degraded()；已 degraded 就返回安全值。
//  2. guarded() 以 __try/__except 呼叫內層函式；/EHsc 下 SEH 穿過內層時不跑解構子，所以鎖與 COM 參照可能沒放，
//     degraded 之後一律不再碰那些物件。不用 /EHa、不設 SetUnhandledExceptionFilter。
//  3. 只攔 ACCESS_VIOLATION、C++ 例外（0xE06D7363）、整數除零；STACK_OVERFLOW、HEAP_CORRUPTION 等回
//     EXCEPTION_CONTINUE_SEARCH（吞掉只會把損壞延後到 SteamVR 自己的程式碼）。
//  4. 攔到後由 mark_degraded() 記一次 log、寫 driver_status.degraded_code、送 STATE DEGRADED（driver_main.cpp 實作）。
#pragma once

#include <cstdint>

#include <windows.h>

namespace vrdrv {

  // driver_main.cpp 實作
  bool degraded();
  void mark_degraded(uint32_t code, const char *where);

  inline int seh_filter(uint32_t code) {
    switch (code) {
      case EXCEPTION_ACCESS_VIOLATION:
      case 0xE06D7363u:  // MSVC C++ 例外
      case EXCEPTION_INT_DIVIDE_BY_ZERO:
        return EXCEPTION_EXECUTE_HANDLER;
      default:
        return EXCEPTION_CONTINUE_SEARCH;
    }
  }

  // 內層不可以有需要解構的區域物件跨過 __try 邊界；F 由呼叫端的 lambda 包住所有 C++ 物件。
  // 回傳 false 表示攔到例外（已 degraded）。
  template <class F>
  bool guarded(const char *where, F &&f) {
    uint32_t code = 0;
    __try {
      f();
      return true;
    } __except (seh_filter(code = GetExceptionCode())) {
    }
    mark_degraded(code, where);
    return false;
  }

}  // namespace vrdrv
