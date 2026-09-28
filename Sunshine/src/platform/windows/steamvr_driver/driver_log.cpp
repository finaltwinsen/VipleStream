// driver_log.cpp - 見 driver_log.h。
#include "driver_log.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <windows.h>

namespace vrdrv::log {
  namespace {
    // 全部是常數初始化（SRWLOCK_INIT 與 POD），DLL 載入時不執行任何程式碼。
    SRWLOCK g_lock = SRWLOCK_INIT;
    sink_fn g_sink = nullptr;
    void *g_sink_ctx = nullptr;
    sink_fn g_forward = nullptr;
    void *g_forward_ctx = nullptr;

    // token bucket：容量 20、每秒補 20。
    constexpr int64_t k_burst = 20;
    constexpr int64_t k_rate_per_s = 20;
    int64_t g_tokens_milli = k_burst * 1000;  // 以 1/1000 行為單位，避免浮點
    int64_t g_last_qpc = 0;
    int64_t g_qpf = 0;
    uint32_t g_suppressed = 0;

    // 在 g_lock 內呼叫。回傳 true = 這一行可以送出。
    bool take_token_locked() {
      LARGE_INTEGER now;
      QueryPerformanceCounter(&now);
      if (g_qpf == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_qpf = f.QuadPart > 0 ? f.QuadPart : 1;
        g_last_qpc = now.QuadPart;
      }
      const int64_t dt = now.QuadPart - g_last_qpc;
      if (dt > 0) {
        // 補充量 = dt / qpf × rate × 1000；先除再乘避免溢位（dt 最多數小時也不會溢位）
        const int64_t add = (dt * k_rate_per_s * 1000) / g_qpf;
        if (add > 0) {
          g_tokens_milli += add;
          if (g_tokens_milli > k_burst * 1000) {
            g_tokens_milli = k_burst * 1000;
          }
          g_last_qpc = now.QuadPart;
        }
      }
      if (g_tokens_milli >= 1000) {
        g_tokens_milli -= 1000;
        return true;
      }
      return false;
    }

    // 只留可列印 ASCII；換行、控制字元、非 ASCII 一律換成 '?'。
    void sanitize(char *s) {
      for (; *s; ++s) {
        const unsigned char ch = (unsigned char) *s;
        if (ch < 0x20 || ch >= 0x7F) {
          *s = '?';
        }
      }
    }
  }  // namespace

  void set_sink(sink_fn fn, void *ctx) {
    AcquireSRWLockExclusive(&g_lock);
    g_sink = fn;
    g_sink_ctx = ctx;
    ReleaseSRWLockExclusive(&g_lock);
  }

  void set_forward(sink_fn fn, void *ctx) {
    AcquireSRWLockExclusive(&g_lock);
    g_forward = fn;
    g_forward_ctx = ctx;
    ReleaseSRWLockExclusive(&g_lock);
  }

  void write(int level, const char *fmt, ...) {
    if (level < info) {
      level = info;
    }
    if (level > error) {
      level = error;
    }
    char body[480];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    if (n < 0) {
      return;
    }
    sanitize(body);

    // 節流與 sink 呼叫都在鎖內：sink／forward 不可再呼叫 vrdrv::log（header 已註明）。
    // 鎖內呼叫 sink 讓多執行緒的行不會交錯，也讓 set_sink(nullptr) 之後保證沒有進行中的呼叫。
    AcquireSRWLockExclusive(&g_lock);
    if (!take_token_locked()) {
      ++g_suppressed;
      ReleaseSRWLockExclusive(&g_lock);
      return;
    }
    char line[512];
    if (g_suppressed != 0) {
      snprintf(line, sizeof(line), "(suppressed %u) %s", g_suppressed, body);
      g_suppressed = 0;
    } else {
      std::memcpy(line, body, sizeof(body));
      line[sizeof(body) - 1] = '\0';
    }
    if (g_sink) {
      g_sink(g_sink_ctx, level, line);
    }
    if (g_forward) {
      g_forward(g_forward_ctx, level, line);
    }
    ReleaseSRWLockExclusive(&g_lock);
  }

  uint32_t suppressed_pending() {
    AcquireSRWLockShared(&g_lock);
    const uint32_t v = g_suppressed;
    ReleaseSRWLockShared(&g_lock);
    return v;
  }

  void reset_throttle_for_test() {
    AcquireSRWLockExclusive(&g_lock);
    g_tokens_milli = k_burst * 1000;
    g_suppressed = 0;
    g_qpf = 0;
    ReleaseSRWLockExclusive(&g_lock);
  }

}  // namespace vrdrv::log
