// 測試用的替身：產品的 app/SDL_compat.h 會帶進整個 SDL。這裡只提供 vrpreflightcheck.cpp、vrlauncher.cpp
// 用到的三個 log 函式，把每一行交給測試（test_check.cpp 的 testLogLine）檢查內容。
// 建置時這個目錄要排在 include 路徑的最前面（見 run_tests.ps1）。

#pragma once

#include <cstdarg>
#include <cstdio>

#define SDL_LOG_CATEGORY_APPLICATION 0

void testLogLine(const char* level, const char* line);

inline void testLogFormat(const char* level, const char* fmt, va_list ap)
{
    char buf[2048];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    testLogLine(level, buf);
}

inline void SDL_LogInfo(int, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    testLogFormat("info", fmt, ap);
    va_end(ap);
}

inline void SDL_LogWarn(int, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    testLogFormat("warn", fmt, ap);
    va_end(ap);
}

inline void SDL_LogError(int, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    testLogFormat("error", fmt, ap);
    va_end(ap);
}
