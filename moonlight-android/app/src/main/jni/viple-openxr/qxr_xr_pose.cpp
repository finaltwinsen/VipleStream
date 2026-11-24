#include "qxr_xr_pose.h"

#include "qxr_log.h"
#include "qxr_xr_input.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <time.h>

namespace {

constexpr int64_t kFallbackAheadNs = 20000000;

template <typename T>
T xrZero(XrStructureType type)
{
    T value{};
    value.type = type;
    return value;
}

uint32_t clampAheadNs(int64_t ahead)
{
    if (ahead <= 0) {
        return 0;
    }
    if (ahead > static_cast<int64_t>(UINT32_MAX)) {
        return UINT32_MAX;
    }
    return static_cast<uint32_t>(ahead);
}

void logPose(XrInstance instance, const char* fn, XrResult result)
{
    char text[XR_MAX_RESULT_STRING_SIZE] = {};
    const char* extra = "";
    if (instance != XR_NULL_HANDLE && XR_SUCCEEDED(xrResultToString(instance, result, text))) {
        extra = text;
    }
    QXR_LOGE("POSE", "%s failed: XrResult=%d %s", fn, static_cast<int>(result), extra);
}

void logPoseLimited(XrInstance instance, const char* fn, XrResult result, std::atomic<uint32_t>* counter)
{
    const uint32_t n = counter->fetch_add(1, std::memory_order_relaxed) + 1;
    if (n != 1 && (n % 300) != 0) {
        return;
    }
    char text[XR_MAX_RESULT_STRING_SIZE] = {};
    const char* extra = "";
    if (instance != XR_NULL_HANDLE && XR_SUCCEEDED(xrResultToString(instance, result, text))) {
        extra = text;
    }
    QXR_LOGE("POSE", "%s failed: XrResult=%d %s (occurrence %u)", fn, static_cast<int>(result), extra, n);
}

void copy3(float* dst, float x, float y, float z)
{
    dst[0] = x;
    dst[1] = y;
    dst[2] = z;
}

void copy4(float* dst, float x, float y, float z, float w)
{
    dst[0] = x;
    dst[1] = y;
    dst[2] = z;
    dst[3] = w;
}

}  // namespace

QxrXrPoseSource::QxrXrPoseSource(XrInstance instance, XrSession session, XrSpace track, XrSpace view,
                                 QxrXrControllers* controllers, QxrConvertTimespecFn convert, bool stage,
                                 bool timeExtensionEnabled)
    : instance_(instance),
      session_(session),
      track_(track),
      view_(view),
      controllers_(controllers),
      convert_(convert),
      stage_(stage)
{
    if (convert_ == nullptr) {
        // 沒有換算函式時，上行 locate 用不到「現在」，改吃影格執行緒剛留下的 predictedDisplayTime。
        QXR_LOGW("POSE",
                 "%s; uplink locate uses the frame thread predictedDisplayTime",
                 timeExtensionEnabled ? "xrConvertTimespecTimeToTimeKHR is unavailable"
                                      : "XR_KHR_convert_timespec_time is not enabled");
    }
    else {
        QXR_LOGI("POSE", "xrConvertTimespecTimeToTimeKHR ready");
    }
    if (!stage_) {
        QXR_LOGI("POSE", "play area unknown (tracking space is not STAGE)");
    }
    refreshPlayArea();
}

bool QxrXrPoseSource::nowXrTime(XrTime* out)
{
    if (convert_ == nullptr || instance_ == XR_NULL_HANDLE || out == nullptr) {
        return false;
    }
    timespec ts{};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        const uint32_t n = convertFails_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n == 1 || (n % 300) == 0) {
            QXR_LOGE("POSE", "clock_gettime(CLOCK_MONOTONIC) failed: errno=%d (occurrence %u)", errno, n);
        }
        return false;
    }
    const XrResult result = convert_(instance_, &ts, out);
    if (!XR_SUCCEEDED(result)) {
        logPoseLimited(instance_, "xrConvertTimespecTimeToTimeKHR", result, &convertFails_);
        return false;
    }
    return true;
}

bool QxrXrPoseSource::locateHead(XrTime t, HeadSample* out)
{
    if (out == nullptr || t == 0 || view_ == XR_NULL_HANDLE || track_ == XR_NULL_HANDLE) {
        return false;
    }
    XrSpaceVelocity velocity = xrZero<XrSpaceVelocity>(XR_TYPE_SPACE_VELOCITY);
    XrSpaceLocation location = xrZero<XrSpaceLocation>(XR_TYPE_SPACE_LOCATION);
    location.next = &velocity;
    const XrResult result = xrLocateSpace(view_, track_, t, &location);
    if (!XR_SUCCEEDED(result)) {
        logPoseLimited(instance_, "xrLocateSpace(head)", result, &locateFails_);
        return false;
    }
    if ((location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) == 0) {
        const uint32_t n = headInvalid_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n == 1 || (n % 300) == 0) {
            QXR_LOGW("POSE", "head orientation not valid flags=0x%x (occurrence %u)",
                     static_cast<unsigned>(location.locationFlags), n);
        }
        return false;
    }
    out->valid = true;
    // §QUEST-PCVR：線上格式沒有「位置有效」這一位。POSITION_VALID 沒設時位置是未定義的，送 0，方向仍可用。
    if ((location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0) {
        copy3(out->pos, location.pose.position.x, location.pose.position.y, location.pose.position.z);
    }
    else {
        copy3(out->pos, 0.0f, 0.0f, 0.0f);
    }
    copy4(out->rot, location.pose.orientation.x, location.pose.orientation.y, location.pose.orientation.z,
          location.pose.orientation.w);
    const bool linear = (velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0;
    const bool angular = (velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) != 0;
    if (linear) {
        copy3(out->linVel, velocity.linearVelocity.x, velocity.linearVelocity.y, velocity.linearVelocity.z);
    }
    else {
        copy3(out->linVel, 0.0f, 0.0f, 0.0f);
    }
    if (angular) {
        copy3(out->angVel, velocity.angularVelocity.x, velocity.angularVelocity.y, velocity.angularVelocity.z);
    }
    else {
        copy3(out->angVel, 0.0f, 0.0f, 0.0f);
    }
    return true;
}

void QxrXrPoseSource::writeHead(VIPLE_VR_TRACKING* out, const HeadSample& head) const
{
    out->flags = static_cast<uint8_t>(out->flags | VIPLE_VR_TRK_HMD | VIPLE_VR_TRK_PRESENCE);
    VIPLE_VR_POSE& pose = out->pose[VIPLE_VR_POSE_HMD];
    std::memcpy(pose.pos, head.pos, sizeof(pose.pos));
    std::memcpy(pose.rot, head.rot, sizeof(pose.rot));
    std::memcpy(pose.linVel, head.linVel, sizeof(pose.linVel));
    std::memcpy(pose.angVel, head.angVel, sizeof(pose.angVel));
    out->predictNs = head.predictNs;
}

void QxrXrPoseSource::onFrameTiming(XrTime predictedDisplayTime, int64_t predictedDisplayPeriodNs)
{
    if (!sessionRunning_.load(std::memory_order_acquire)) {
        return;
    }
    int64_t ahead = 0;
    if (predictedDisplayPeriodNs > 0) {
        ahead = predictedDisplayPeriodNs * 2;
    }
    XrTime xrNow = 0;
    if (nowXrTime(&xrNow) && predictedDisplayTime > xrNow) {
        ahead = static_cast<int64_t>(predictedDisplayTime - xrNow);
    }
    const int64_t prev = predictAheadNs_.load(std::memory_order_relaxed);
    const int64_t smoothed = prev == 0 ? ahead : (prev * 7 + ahead) / 8;
    predictAheadNs_.store(smoothed, std::memory_order_relaxed);

    if (view_ != XR_NULL_HANDLE && track_ != XR_NULL_HANDLE) {
        HeadSample fresh{};
        if (locateHead(predictedDisplayTime, &fresh)) {
            // 快取記的是這一格自己的提前量，不是平滑後的值。上行 locate 失敗時才拿它來補。
            fresh.predictNs = clampAheadNs(ahead);
            std::lock_guard<std::mutex> lock(mu_);
            cache_ = fresh;
        }
    }
    if (controllers_ != nullptr) {
        controllers_->updateGripPose(predictedDisplayTime, track_);
    }
}

void QxrXrPoseSource::setSessionRunning(bool running)
{
    sessionRunning_.store(running, std::memory_order_release);
}

bool QxrXrPoseSource::sample(VIPLE_VR_TRACKING* out, uint64_t nowUs)
{
    (void)nowUs;
    if (out == nullptr) {
        return false;
    }
    // xrEndSession 之後 session 不再是 running。這時 xrLocateSpace 違反生命週期，快取也不該再送出去。
    if (!sessionRunning_.load(std::memory_order_acquire)) {
        return false;
    }
    const int64_t smoothed = predictAheadNs_.load(std::memory_order_relaxed);
    const int64_t ahead = smoothed > 0 ? smoothed : kFallbackAheadNs;
    XrTime handTime = 0;
    HeadSample live{};
    bool liveOk = false;
    XrTime xrNow = 0;
    if (convert_ != nullptr && nowXrTime(&xrNow)) {
        handTime = xrNow + ahead;
        liveOk = locateHead(handTime, &live);
        if (liveOk) {
            live.predictNs = clampAheadNs(ahead);
        }
    }

    HeadSample head{};
    if (liveOk) {
        head = live;
    }
    else {
        std::lock_guard<std::mutex> lock(mu_);
        head = cache_;
        if (!head.valid) {
            return false;
        }
    }
    writeHead(out, head);
    if (controllers_ != nullptr) {
        // 換算成功時，頭這次 locate 失敗也仍用同一個時刻找手。換算失敗則 t＝0，沿用影格執行緒的握把快照。
        controllers_->fill(out, handTime, track_);
    }
    return true;
}

void QxrXrPoseSource::refreshPlayArea()
{
    float width = 0.0f;
    float depth = 0.0f;
    if (stage_ && session_ != XR_NULL_HANDLE) {
        XrExtent2Df extent{};
        const XrResult result = xrGetReferenceSpaceBoundsRect(session_, XR_REFERENCE_SPACE_TYPE_STAGE, &extent);
        const float minM = static_cast<float>(VIPLE_VR_PLAYAREA_MIN_MM) / 1000.0f;
        const float maxM = static_cast<float>(VIPLE_VR_PLAYAREA_MAX_MM) / 1000.0f;
        // XR_SPACE_BOUNDS_UNAVAILABLE 也是成功碼，而且 extent 可能沒被寫入。只有 XR_SUCCESS 才讀寬深。
        if (result == XR_SUCCESS) {
            const bool inRange = extent.width >= minM && extent.width <= maxM && extent.height >= minM && extent.height <= maxM;
            if (inRange) {
                width = extent.width;
                depth = extent.height;
                QXR_LOGI("POSE", "STAGE play area bounds: %.2f x %.2f m", static_cast<double>(width),
                         static_cast<double>(depth));
            }
            else {
                QXR_LOGI("POSE",
                         "STAGE play area bounds out of range: xrGetReferenceSpaceBoundsRect XrResult=%d w=%.2f h=%.2f",
                         static_cast<int>(result), static_cast<double>(extent.width), static_cast<double>(extent.height));
            }
        }
        else if (!XR_SUCCEEDED(result)) {
            logPose(instance_, "xrGetReferenceSpaceBoundsRect", result);
        }
        else {
            QXR_LOGI("POSE", "STAGE play area bounds unavailable: xrGetReferenceSpaceBoundsRect XrResult=%d",
                     static_cast<int>(result));
        }
    }
    std::lock_guard<std::mutex> lock(mu_);
    playWidthM_ = width;
    playDepthM_ = depth;
}

void QxrXrPoseSource::copyPlayArea(float* widthM, float* depthM) const
{
    std::lock_guard<std::mutex> lock(mu_);
    if (widthM != nullptr) {
        *widthM = playWidthM_;
    }
    if (depthM != nullptr) {
        *depthM = playDepthM_;
    }
}
