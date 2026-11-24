#pragma once

#include <android/log.h>

// tag 固定。訊息本體一律再以 [VIPLE-QXR-<子系統>] 起頭，logcat 過濾時兩段都看得到。
#define QXR_LOG_TAG "viple-openxr"

#define QXR_LOG(level, sub, fmt, ...) \
    __android_log_print((level), QXR_LOG_TAG, "[VIPLE-QXR-" sub "] " fmt, ##__VA_ARGS__)

#define QXR_LOGE(sub, fmt, ...) QXR_LOG(ANDROID_LOG_ERROR, sub, fmt, ##__VA_ARGS__)
#define QXR_LOGW(sub, fmt, ...) QXR_LOG(ANDROID_LOG_WARN, sub, fmt, ##__VA_ARGS__)
#define QXR_LOGI(sub, fmt, ...) QXR_LOG(ANDROID_LOG_INFO, sub, fmt, ##__VA_ARGS__)
