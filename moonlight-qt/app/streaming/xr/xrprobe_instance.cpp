// VipleStream §SF-PROBE（M2a）— xr-probe A 段的 OpenXR 實作（只在 CONFIG+=openxr 編譯，
// 定義 HAVE_OPENXR；app.pro 的 openxr 區塊以 pkg-config openxr 連 loader）。
//
// 只做 instance／system 層（不建 XrSession、不建 VkDevice），回答 Day-1 的問題：
//   - PoC-F／G-PKG：沙箱內 loader 能不能載入 runtime（xrCreateInstance 成功與否）
//   - PoC-2：擴充清單；XR_KHR_convert_timespec_time 有無（決定 tracking 走 thread 或 frameloop，
//     vr_architecture.md §2.4:309）；VALVE 前綴的擴充（Frame 手把的擴充正確名稱 UNVERIFIED，
//     所以不寫死名字，只把含 "VALVE" 的全列出來）
//   - vulkan_enable2（缺時 vulkan_enable）的 API 版本需求與 runtime 指定的 GPU
//   - system 屬性（含眼動）、PRIMARY_STEREO 的建議 image rect 與 blend mode
// refresh rate、FOV、reference space 都要 session，屬於 B 段（M3a）。
//
// 每個步驟失敗都只記錄、繼續下一步；結束前一定 xrDestroyInstance（沒 destroy 就 _Exit，
// vrserver 會把我們當成當掉的 client）。

#include "xrprobe_instance.h"

#include "cli/probeutil.h"
#include "cli/xrprobe.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <utility>   // std::as_const
#include <vector>

#if !defined(Q_OS_WIN)
#include <dlfcn.h>
#endif

// XR_KHR_convert_timespec_time 的函式型別要 XR_USE_TIMESPEC＋struct timespec
#if defined(Q_OS_LINUX)
#include <time.h>
#ifndef XR_USE_TIMESPEC
#define XR_USE_TIMESPEC
#endif
#endif

// vulkan_enable／enable2 的函式型別要 XR_USE_GRAPHICS_API_VULKAN＋Vulkan 型別。
// 只用 header 的型別；函式一律經 vkGetInstanceProcAddr 取得（不連 -lvulkan）。
#if __has_include(<vulkan/vulkan.h>)
#include <vulkan/vulkan.h>
#ifndef XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_GRAPHICS_API_VULKAN
#endif
#define VIPLE_XRPROBE_VULKAN 1
#endif

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
// 放在 openxr 標頭之後：本檔先定義 XR_USE_TIMESPEC 等巨集，xrcontext.h 若先引入會讓那些原型被 guard 掉
#include "xrcontext.h"  // XrContext::loadVulkanLoader（Linux dlopen／Windows vulkan-1.dll）

#if __has_include(<openxr/openxr_reflection.h>)
#include <openxr/openxr_reflection.h>
#define VIPLE_XRPROBE_REFLECTION 1
#endif

namespace {

const char* const kTag = "VIPLE-XR-PROBE";

// 要啟用的擴充（列舉後有才啟用）。XR_KHR_vulkan_enable 只在沒有 enable2 時啟用（備援路徑）。
const char* const kExtVulkan2 = "XR_KHR_vulkan_enable2";
const char* const kExtVulkan1 = "XR_KHR_vulkan_enable";
const char* const kExtTimespec = "XR_KHR_convert_timespec_time";
const char* const kExtEyeGaze = "XR_EXT_eye_gaze_interaction";
const char* const kWantedOptional[] = {
    "XR_FB_display_refresh_rate",
    "XR_KHR_composition_layer_cylinder",
    "XR_EXT_local_floor",
    "XR_MND_headless",   // 只有 Monado（S1 驗測）；啟用後 instance 層行為不變
};

// ── 小工具 ──

// 先清零再填 type：避免 {XR_TYPE_…} 部分初始化在 -Wextra 下的 missing-field-initializers 警告
template <typename T>
T xrStruct(XrStructureType type)
{
    T s;
    std::memset(&s, 0, sizeof(s));
    s.type = type;
    return s;
}

template <typename PFN>
PFN getProc(XrInstance instance, const char* name)
{
    PFN_xrVoidFunction fn = nullptr;
    if (XR_FAILED(xrGetInstanceProcAddr(instance, name, &fn))) {
        return nullptr;
    }
    return reinterpret_cast<PFN>(fn);
}

QString resultName(XrResult r)
{
#if defined(VIPLE_XRPROBE_REFLECTION)
    // 不用 xrResultToString：它要有效的 instance，xrCreateInstance 失敗時沒有
    switch (r) {
#define VIPLE_XR_ENUM_CASE(name, value) \
    case name:                          \
        return QStringLiteral(#name);
        XR_LIST_ENUM_XrResult(VIPLE_XR_ENUM_CASE)
#undef VIPLE_XR_ENUM_CASE
    default:
        break;
    }
#endif
    return QStringLiteral("XrResult(%1)").arg(static_cast<int>(r));
}

QString blendModeName(XrEnvironmentBlendMode m)
{
    switch (m) {
    case XR_ENVIRONMENT_BLEND_MODE_OPAQUE:
        return QStringLiteral("OPAQUE");
    case XR_ENVIRONMENT_BLEND_MODE_ADDITIVE:
        return QStringLiteral("ADDITIVE");
    case XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND:
        return QStringLiteral("ALPHA_BLEND");
    default:
        return QStringLiteral("%1").arg(static_cast<int>(m));
    }
}

QString viewConfigName(XrViewConfigurationType t)
{
#if defined(VIPLE_XRPROBE_REFLECTION)
    switch (t) {
#define VIPLE_XR_ENUM_CASE(name, value) \
    case name:                          \
        return QStringLiteral(#name).mid(int(sizeof("XR_VIEW_CONFIGURATION_TYPE_")) - 1);
        XR_LIST_ENUM_XrViewConfigurationType(VIPLE_XR_ENUM_CASE)
#undef VIPLE_XR_ENUM_CASE
    default:
        break;
    }
#endif
    return QStringLiteral("%1").arg(static_cast<int>(t));
}

QString xrVersionString(XrVersion v)
{
    return QStringLiteral("%1.%2.%3")
        .arg(static_cast<unsigned>(XR_VERSION_MAJOR(v)))
        .arg(static_cast<unsigned>(XR_VERSION_MINOR(v)))
        .arg(static_cast<unsigned>(XR_VERSION_PATCH(v)));
}

QString hex(uint32_t v, int width)
{
    return QStringLiteral("0x") + QString::number(v, 16).rightJustified(width, QLatin1Char('0'));
}

// instance 已建立後，這兩個結果代表 runtime 本身出事（rc 14），其餘失敗只是「這一項沒有」
bool isRuntimeBroken(XrResult r)
{
    return r == XR_ERROR_INSTANCE_LOST || r == XR_ERROR_RUNTIME_FAILURE;
}

struct ProbeState {
    bool runtimeBroken = false;

    // 記錄一個步驟的結果到 JSON，回傳是否成功
    bool check(QJsonObject& obj, const char* step, XrResult r)
    {
        obj[QString::fromLatin1(step)] = resultName(r);
        if (isRuntimeBroken(r)) {
            runtimeBroken = true;
        }
        return XR_SUCCEEDED(r);
    }
};

// ── loader 層（不需要 instance） ──

QJsonArray listApiLayers()
{
    QJsonArray arr;
    uint32_t count = 0;
    if (XR_FAILED(xrEnumerateApiLayerProperties(0, &count, nullptr)) || count == 0) {
        return arr;
    }
    std::vector<XrApiLayerProperties> layers(count, xrStruct<XrApiLayerProperties>(XR_TYPE_API_LAYER_PROPERTIES));
    if (XR_FAILED(xrEnumerateApiLayerProperties(count, &count, layers.data()))) {
        return arr;
    }
    layers.resize(count);
    for (const XrApiLayerProperties& l : layers) {
        QJsonObject o;
        o[QStringLiteral("name")] = QString::fromUtf8(l.layerName);
        o[QStringLiteral("specVersion")] = xrVersionString(l.specVersion);
        o[QStringLiteral("layerVersion")] = static_cast<int>(l.layerVersion);
        o[QStringLiteral("description")] = QString::fromUtf8(l.description);
        arr.append(o);
        ProbeUtil::printLine(kTag, "api-layer: %s spec=%s", l.layerName, qUtf8Printable(xrVersionString(l.specVersion)));
    }
    return arr;
}

// 注意：loader 為了回報 runtime 的擴充，會在這裡就載入 runtime；沒有 runtime 時這一步就失敗。
XrResult listExtensions(std::vector<XrExtensionProperties>& exts)
{
    uint32_t count = 0;
    XrResult r = xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
    if (XR_FAILED(r)) {
        return r;
    }
    exts.assign(count, xrStruct<XrExtensionProperties>(XR_TYPE_EXTENSION_PROPERTIES));
    r = xrEnumerateInstanceExtensionProperties(nullptr, count, &count, exts.data());
    if (XR_SUCCEEDED(r)) {
        exts.resize(count);
    }
    else {
        exts.clear();
    }
    return r;
}

bool hasExtension(const std::vector<XrExtensionProperties>& exts, const char* name)
{
    for (const XrExtensionProperties& e : exts) {
        if (std::strcmp(e.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

// ── instance 層 ──

#if defined(XR_USE_TIMESPEC)
// CLOCK_MONOTONIC → XrTime → timespec 來回一次：來回差應為 0；XrTime 與 monotonic 的差
// 看 runtime 的時間基準（多數 runtime 直接用 monotonic ns，差值 0）。
void probeTimespec(XrInstance instance, QJsonObject& out, ProbeState& st)
{
    QJsonObject o;
    auto toXr = getProc<PFN_xrConvertTimespecTimeToTimeKHR>(instance, "xrConvertTimespecTimeToTimeKHR");
    auto toTs = getProc<PFN_xrConvertTimeToTimespecTimeKHR>(instance, "xrConvertTimeToTimespecTimeKHR");
    if (toXr == nullptr || toTs == nullptr) {
        o[QStringLiteral("error")] = QStringLiteral("xrGetInstanceProcAddr failed");
        ProbeUtil::printLine(kTag, "timespec: function pointers unavailable");
        out[QStringLiteral("timespec")] = o;
        return;
    }

    struct timespec ts1;
    clock_gettime(CLOCK_MONOTONIC, &ts1);
    XrTime xrTime = 0;
    const auto t0 = std::chrono::steady_clock::now();
    const XrResult r1 = toXr(instance, &ts1, &xrTime);
    const auto t1 = std::chrono::steady_clock::now();
    struct timespec ts2;
    std::memset(&ts2, 0, sizeof(ts2));
    const XrResult r2 = XR_SUCCEEDED(r1) ? toTs(instance, xrTime, &ts2) : XR_ERROR_VALIDATION_FAILURE;
    const auto t2 = std::chrono::steady_clock::now();

    const bool ok1 = st.check(o, "toXrTime", r1);
    const bool ok2 = XR_SUCCEEDED(r1) && st.check(o, "toTimespec", r2);
    if (ok1 && ok2) {
        const int64_t ns1 = static_cast<int64_t>(ts1.tv_sec) * 1000000000LL + ts1.tv_nsec;
        const int64_t ns2 = static_cast<int64_t>(ts2.tv_sec) * 1000000000LL + ts2.tv_nsec;
        const long long roundTrip = static_cast<long long>(ns2 - ns1);
        const long long offset = static_cast<long long>(xrTime - ns1);
        const long long toXrUs = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
        const long long toTsUs = std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count();
        o[QStringLiteral("roundTripDiffNs")] = static_cast<double>(roundTrip);
        o[QStringLiteral("xrTimeMinusMonotonicNs")] = static_cast<double>(offset);
        o[QStringLiteral("toXrTimeUs")] = static_cast<double>(toXrUs);
        o[QStringLiteral("toTimespecUs")] = static_cast<double>(toTsUs);
        ProbeUtil::printLine(kTag, "timespec: roundtrip=%lldns xr-mono=%lldns toXr=%lldus toTs=%lldus",
                             roundTrip, offset, toXrUs, toTsUs);
    }
    else {
        ProbeUtil::printLine(kTag, "timespec: conversion failed (%s / %s)",
                             qUtf8Printable(resultName(r1)), XR_SUCCEEDED(r1) ? qUtf8Printable(resultName(r2)) : "-");
    }
    out[QStringLiteral("timespec")] = o;
}
#endif

bool probeSystem(XrInstance instance, bool eyeGazeEnabled, XrSystemId* systemId, QJsonObject& out, ProbeState& st)
{
    QJsonObject o;
    XrSystemGetInfo getInfo = xrStruct<XrSystemGetInfo>(XR_TYPE_SYSTEM_GET_INFO);
    getInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrResult r = xrGetSystem(instance, &getInfo, systemId);
    if (!st.check(o, "xrGetSystem", r)) {
        // 常見：XR_ERROR_FORM_FACTOR_UNAVAILABLE（runtime 在、頭盔沒接或沒開）
        ProbeUtil::printLine(kTag, "system: HMD unavailable (%s); view configuration and vulkan skipped",
                             qUtf8Printable(resultName(r)));
        out[QStringLiteral("system")] = o;
        return false;
    }

    XrSystemEyeGazeInteractionPropertiesEXT eyeGaze =
        xrStruct<XrSystemEyeGazeInteractionPropertiesEXT>(XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT);
    XrSystemProperties props = xrStruct<XrSystemProperties>(XR_TYPE_SYSTEM_PROPERTIES);
    if (eyeGazeEnabled) {
        // 只在擴充啟用時串：未啟用擴充的結構串進 next 是 validation 錯誤
        props.next = &eyeGaze;
    }
    r = xrGetSystemProperties(instance, *systemId, &props);
    if (st.check(o, "xrGetSystemProperties", r)) {
        o[QStringLiteral("systemId")] = QString::number(static_cast<qulonglong>(*systemId));
        o[QStringLiteral("name")] = QString::fromUtf8(props.systemName);
        o[QStringLiteral("vendorId")] = hex(props.vendorId, 4);
        o[QStringLiteral("maxSwapchainWidth")] = static_cast<int>(props.graphicsProperties.maxSwapchainImageWidth);
        o[QStringLiteral("maxSwapchainHeight")] = static_cast<int>(props.graphicsProperties.maxSwapchainImageHeight);
        o[QStringLiteral("maxLayerCount")] = static_cast<int>(props.graphicsProperties.maxLayerCount);
        o[QStringLiteral("orientationTracking")] = props.trackingProperties.orientationTracking != XR_FALSE;
        o[QStringLiteral("positionTracking")] = props.trackingProperties.positionTracking != XR_FALSE;
        if (eyeGazeEnabled) {
            o[QStringLiteral("supportsEyeGazeInteraction")] = eyeGaze.supportsEyeGazeInteraction != XR_FALSE;
        }
        ProbeUtil::printLine(kTag, "system: name=\"%s\" vendor=%s maxSwapchain=%ux%u layers=%u orient=%d pos=%d eyeGaze=%s",
                             props.systemName, qUtf8Printable(hex(props.vendorId, 4)),
                             props.graphicsProperties.maxSwapchainImageWidth,
                             props.graphicsProperties.maxSwapchainImageHeight,
                             props.graphicsProperties.maxLayerCount,
                             props.trackingProperties.orientationTracking != XR_FALSE ? 1 : 0,
                             props.trackingProperties.positionTracking != XR_FALSE ? 1 : 0,
                             eyeGazeEnabled ? (eyeGaze.supportsEyeGazeInteraction != XR_FALSE ? "1" : "0") : "n/a");
    }
    else {
        ProbeUtil::printLine(kTag, "system: xrGetSystemProperties failed (%s)", qUtf8Printable(resultName(r)));
    }
    out[QStringLiteral("system")] = o;
    return true;
}

void probeViewConfigurations(XrInstance instance, XrSystemId systemId, QJsonObject& out, ProbeState& st)
{
    QJsonObject o;
    uint32_t count = 0;
    XrResult r = xrEnumerateViewConfigurations(instance, systemId, 0, &count, nullptr);
    std::vector<XrViewConfigurationType> types;
    if (st.check(o, "xrEnumerateViewConfigurations", r) && count > 0) {
        types.resize(count);
        r = xrEnumerateViewConfigurations(instance, systemId, count, &count, types.data());
        if (st.check(o, "xrEnumerateViewConfigurations", r)) {
            types.resize(count);
        }
        else {
            types.clear();
        }
    }
    QStringList typeNames;
    bool hasStereo = false;
    for (XrViewConfigurationType t : types) {
        typeNames << viewConfigName(t);
        hasStereo = hasStereo || t == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    }
    o[QStringLiteral("types")] = QJsonArray::fromStringList(typeNames);
    ProbeUtil::printLine(kTag, "viewconfig: types=[%s]", qUtf8Printable(typeNames.join(QLatin1Char(','))));

    if (!hasStereo) {
        ProbeUtil::printLine(kTag, "viewconfig PRIMARY_STEREO: not offered by the runtime");
        out[QStringLiteral("viewConfigurations")] = o;
        return;
    }

    const XrViewConfigurationType stereo = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    QJsonObject so;

    XrViewConfigurationProperties vcp = xrStruct<XrViewConfigurationProperties>(XR_TYPE_VIEW_CONFIGURATION_PROPERTIES);
    r = xrGetViewConfigurationProperties(instance, systemId, stereo, &vcp);
    if (st.check(so, "xrGetViewConfigurationProperties", r)) {
        so[QStringLiteral("fovMutable")] = vcp.fovMutable != XR_FALSE;
    }

    std::vector<XrViewConfigurationView> views;
    uint32_t viewCount = 0;
    r = xrEnumerateViewConfigurationViews(instance, systemId, stereo, 0, &viewCount, nullptr);
    if (st.check(so, "xrEnumerateViewConfigurationViews", r) && viewCount > 0) {
        views.assign(viewCount, xrStruct<XrViewConfigurationView>(XR_TYPE_VIEW_CONFIGURATION_VIEW));
        r = xrEnumerateViewConfigurationViews(instance, systemId, stereo, viewCount, &viewCount, views.data());
        if (st.check(so, "xrEnumerateViewConfigurationViews", r)) {
            views.resize(viewCount);
        }
        else {
            views.clear();
        }
    }
    QJsonArray viewArr;
    for (const XrViewConfigurationView& v : views) {
        QJsonObject vj;
        vj[QStringLiteral("recommendedWidth")] = static_cast<int>(v.recommendedImageRectWidth);
        vj[QStringLiteral("recommendedHeight")] = static_cast<int>(v.recommendedImageRectHeight);
        vj[QStringLiteral("maxWidth")] = static_cast<int>(v.maxImageRectWidth);
        vj[QStringLiteral("maxHeight")] = static_cast<int>(v.maxImageRectHeight);
        vj[QStringLiteral("recommendedSamples")] = static_cast<int>(v.recommendedSwapchainSampleCount);
        vj[QStringLiteral("maxSamples")] = static_cast<int>(v.maxSwapchainSampleCount);
        viewArr.append(vj);
    }
    so[QStringLiteral("views")] = viewArr;

    std::vector<XrEnvironmentBlendMode> modes;
    uint32_t modeCount = 0;
    r = xrEnumerateEnvironmentBlendModes(instance, systemId, stereo, 0, &modeCount, nullptr);
    if (st.check(so, "xrEnumerateEnvironmentBlendModes", r) && modeCount > 0) {
        modes.resize(modeCount);
        r = xrEnumerateEnvironmentBlendModes(instance, systemId, stereo, modeCount, &modeCount, modes.data());
        if (st.check(so, "xrEnumerateEnvironmentBlendModes", r)) {
            modes.resize(modeCount);
        }
        else {
            modes.clear();
        }
    }
    QStringList modeNames;
    for (XrEnvironmentBlendMode m : modes) {
        modeNames << blendModeName(m);
    }
    so[QStringLiteral("blendModes")] = QJsonArray::fromStringList(modeNames);

    // 一行摘要：兩眼相同時只印一次，不同時各印
    QStringList viewDesc;
    for (int i = 0; i < static_cast<int>(views.size()); i++) {
        const XrViewConfigurationView& v = views[static_cast<size_t>(i)];
        const QString d = QStringLiteral("rec=%1x%2 max=%3x%4 samples=%5/%6")
                              .arg(v.recommendedImageRectWidth).arg(v.recommendedImageRectHeight)
                              .arg(v.maxImageRectWidth).arg(v.maxImageRectHeight)
                              .arg(v.recommendedSwapchainSampleCount).arg(v.maxSwapchainSampleCount);
        if (!viewDesc.contains(d)) {
            viewDesc << d;
        }
    }
    ProbeUtil::printLine(kTag, "viewconfig PRIMARY_STEREO: views=%d %s fovMutable=%s blend=[%s]",
                         static_cast<int>(views.size()), qUtf8Printable(viewDesc.join(QStringLiteral(" | "))),
                         so.contains(QStringLiteral("fovMutable"))
                             ? (so.value(QStringLiteral("fovMutable")).toBool() ? "1" : "0") : "?",
                         qUtf8Printable(modeNames.join(QLatin1Char(','))));

    o[QStringLiteral("primaryStereo")] = so;
    out[QStringLiteral("viewConfigurations")] = o;
}

#if defined(VIPLE_XRPROBE_VULKAN)

QString vkVersionString(uint32_t v)
{
    return QStringLiteral("%1.%2.%3").arg((v >> 22) & 0x7Fu).arg((v >> 12) & 0x3FFu).arg(v & 0xFFFu);
}

// vkGetInstanceProcAddr：執行期載入 Vulkan loader（app 不連 -lvulkan）。M3a X1 起與 XrContext 共用
// （Linux dlopen libvulkan.so.1、Windows vulkan-1.dll；不卸載）。
PFN_vkGetInstanceProcAddr loadVulkanGipa(QString* error)
{
    return XrContext::loadVulkanLoader(error);
}

// XR 的 API 版本需求（XrVersion：16/16/32 bit）轉 Vulkan 的 apiVersion（取 major.minor，至少 1.0）
uint32_t vkApiFromXr(XrVersion v)
{
    uint32_t major = XR_VERSION_MAJOR(v);
    uint32_t minor = XR_VERSION_MINOR(v);
    if (major < 1) {
        major = 1;
        minor = 0;
    }
    return (major << 22) | (minor << 12);   // == VK_MAKE_API_VERSION(0, major, minor, 0)
}

// runtime 指定的 physical device：名稱、ID、API 版本，以及它在這個 instance 列出的 N 個裝置中排第幾
void describePhysicalDevice(PFN_vkGetInstanceProcAddr gipa, VkInstance vkInstance, VkPhysicalDevice pd,
                            const QString& path, const QString& minApi, const QString& maxApi, QJsonObject& o)
{
    auto getProps = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(vkInstance, "vkGetPhysicalDeviceProperties"));
    auto enumDevices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(vkInstance, "vkEnumeratePhysicalDevices"));
    if (getProps == nullptr) {
        o[QStringLiteral("error")] = QStringLiteral("vkGetPhysicalDeviceProperties not found");
        return;
    }
    VkPhysicalDeviceProperties p;
    std::memset(&p, 0, sizeof(p));
    getProps(pd, &p);

    int pickedIndex = -1;
    uint32_t count = 0;
    if (enumDevices != nullptr && enumDevices(vkInstance, &count, nullptr) == VK_SUCCESS && count > 0) {
        std::vector<VkPhysicalDevice> pds(count);
        if (enumDevices(vkInstance, &count, pds.data()) >= VK_SUCCESS) {
            for (uint32_t i = 0; i < count && i < pds.size(); i++) {
                if (pds[i] == pd) {
                    pickedIndex = static_cast<int>(i);
                }
            }
        }
    }

    o[QStringLiteral("device")] = QString::fromUtf8(p.deviceName);
    o[QStringLiteral("vendorId")] = hex(p.vendorID, 4);
    o[QStringLiteral("deviceId")] = hex(p.deviceID, 4);
    o[QStringLiteral("deviceApiVersion")] = vkVersionString(p.apiVersion);
    o[QStringLiteral("driverVersionRaw")] = hex(p.driverVersion, 8);
    o[QStringLiteral("physicalDeviceCount")] = static_cast<int>(count);
    o[QStringLiteral("pickedIndex")] = pickedIndex;
    ProbeUtil::printLine(kTag, "vulkan: path=%s minApi=%s maxApi=%s device=\"%s\" vendor=%s id=%s api=%s (index %d of %u)",
                         qUtf8Printable(path), qUtf8Printable(minApi), qUtf8Printable(maxApi), p.deviceName,
                         qUtf8Printable(hex(p.vendorID, 4)), qUtf8Printable(hex(p.deviceID, 4)),
                         qUtf8Printable(vkVersionString(p.apiVersion)), pickedIndex, count);
}

void destroyVkInstance(PFN_vkGetInstanceProcAddr gipa, VkInstance vkInstance)
{
    if (vkInstance == VK_NULL_HANDLE) {
        return;
    }
    auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(gipa(vkInstance, "vkDestroyInstance"));
    if (destroy != nullptr) {
        destroy(vkInstance, nullptr);
    }
}

// XR_KHR_vulkan_enable2：需求 → xrCreateVulkanInstanceKHR（runtime 自己補 instance 擴充）
// → xrGetVulkanGraphicsDevice2KHR。不建 VkDevice。
void probeVulkanEnable2(XrInstance instance, XrSystemId systemId, QJsonObject& o, ProbeState& st)
{
    o[QStringLiteral("path")] = QStringLiteral("enable2");
    auto pfnReq = getProc<PFN_xrGetVulkanGraphicsRequirements2KHR>(instance, "xrGetVulkanGraphicsRequirements2KHR");
    auto pfnCreate = getProc<PFN_xrCreateVulkanInstanceKHR>(instance, "xrCreateVulkanInstanceKHR");
    auto pfnDevice = getProc<PFN_xrGetVulkanGraphicsDevice2KHR>(instance, "xrGetVulkanGraphicsDevice2KHR");
    if (pfnReq == nullptr || pfnCreate == nullptr || pfnDevice == nullptr) {
        o[QStringLiteral("error")] = QStringLiteral("xrGetInstanceProcAddr failed for vulkan_enable2 functions");
        ProbeUtil::printLine(kTag, "vulkan: enable2 function pointers unavailable");
        return;
    }

    XrGraphicsRequirementsVulkan2KHR req = xrStruct<XrGraphicsRequirementsVulkan2KHR>(XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR);
    XrResult r = pfnReq(instance, systemId, &req);
    if (!st.check(o, "xrGetVulkanGraphicsRequirements2KHR", r)) {
        ProbeUtil::printLine(kTag, "vulkan: requirements failed (%s)", qUtf8Printable(resultName(r)));
        return;
    }
    const QString minApi = xrVersionString(req.minApiVersionSupported);
    const QString maxApi = xrVersionString(req.maxApiVersionSupported);
    o[QStringLiteral("minApiVersion")] = minApi;
    o[QStringLiteral("maxApiVersion")] = maxApi;

    QString err;
    const PFN_vkGetInstanceProcAddr gipa = loadVulkanGipa(&err);
    if (gipa == nullptr) {
        o[QStringLiteral("error")] = err;
        ProbeUtil::printLine(kTag, "vulkan: minApi=%s maxApi=%s; %s", qUtf8Printable(minApi), qUtf8Printable(maxApi),
                             qUtf8Printable(err));
        return;
    }

    VkApplicationInfo ai;
    std::memset(&ai, 0, sizeof(ai));
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.pApplicationName = "VipleStream xr-probe";
    ai.pEngineName = "VipleStream";
    ai.apiVersion = vkApiFromXr(req.minApiVersionSupported);
    VkInstanceCreateInfo ici;
    std::memset(&ici, 0, sizeof(ici));
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &ai;

    XrVulkanInstanceCreateInfoKHR xci = xrStruct<XrVulkanInstanceCreateInfoKHR>(XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR);
    xci.systemId = systemId;
    xci.createFlags = 0;
    xci.pfnGetInstanceProcAddr = gipa;
    xci.vulkanCreateInfo = &ici;
    xci.vulkanAllocator = nullptr;

    VkInstance vkInstance = VK_NULL_HANDLE;
    VkResult vkr = VK_SUCCESS;
    r = pfnCreate(instance, &xci, &vkInstance, &vkr);
    st.check(o, "xrCreateVulkanInstanceKHR", r);
    o[QStringLiteral("vkCreateInstance")] = static_cast<int>(vkr);
    if (XR_FAILED(r) || vkr != VK_SUCCESS || vkInstance == VK_NULL_HANDLE) {
        ProbeUtil::printLine(kTag, "vulkan: minApi=%s maxApi=%s xrCreateVulkanInstanceKHR=%s VkResult=%d",
                             qUtf8Printable(minApi), qUtf8Printable(maxApi), qUtf8Printable(resultName(r)),
                             static_cast<int>(vkr));
        // 失敗時 runtime 不該回傳 instance；萬一回了照樣收掉
        if (XR_SUCCEEDED(r)) {
            destroyVkInstance(gipa, vkInstance);
        }
        return;
    }

    XrVulkanGraphicsDeviceGetInfoKHR gi = xrStruct<XrVulkanGraphicsDeviceGetInfoKHR>(XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR);
    gi.systemId = systemId;
    gi.vulkanInstance = vkInstance;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    r = pfnDevice(instance, &gi, &pd);
    if (st.check(o, "xrGetVulkanGraphicsDevice2KHR", r) && pd != VK_NULL_HANDLE) {
        describePhysicalDevice(gipa, vkInstance, pd, QStringLiteral("enable2"), minApi, maxApi, o);
    }
    else {
        ProbeUtil::printLine(kTag, "vulkan: minApi=%s maxApi=%s xrGetVulkanGraphicsDevice2KHR=%s",
                             qUtf8Printable(minApi), qUtf8Printable(maxApi), qUtf8Printable(resultName(r)));
    }
    destroyVkInstance(gipa, vkInstance);
}

// 以空白分隔的擴充字串（vulkan_enable 的回傳格式）
QList<QByteArray> splitExtensionString(const QByteArray& s)
{
    QList<QByteArray> out;
    const QList<QByteArray> parts = s.split(' ');
    for (const QByteArray& p : parts) {
        const QByteArray t = p.trimmed();
        if (!t.isEmpty()) {
            out.append(t);
        }
    }
    return out;
}

// xrGetVulkanInstanceExtensionsKHR 與 xrGetVulkanDeviceExtensionsKHR 的函式型別完全相同，共用這支
QByteArray queryExtensionString(PFN_xrGetVulkanInstanceExtensionsKHR fn, XrInstance instance, XrSystemId systemId,
                                XrResult* result)
{
    uint32_t n = 0;
    *result = fn(instance, systemId, 0, &n, nullptr);
    if (XR_FAILED(*result) || n == 0) {
        return QByteArray();
    }
    QByteArray buf(static_cast<int>(n), '\0');
    *result = fn(instance, systemId, n, &n, buf.data());
    if (XR_FAILED(*result)) {
        return QByteArray();
    }
    // n 含結尾的 NUL
    return QByteArray(buf.constData());
}

QJsonArray toJsonArray(const QList<QByteArray>& list)
{
    QJsonArray arr;
    for (const QByteArray& s : list) {
        arr.append(QString::fromUtf8(s));
    }
    return arr;
}

// XR_KHR_vulkan_enable（備援）：需求 → runtime 要的 instance 擴充 → 自己 vkCreateInstance
// → xrGetVulkanGraphicsDeviceKHR；另記 runtime 要的 device 擴充（M3a 建 device 時要用）。
void probeVulkanEnable1(XrInstance instance, XrSystemId systemId, QJsonObject& o, ProbeState& st)
{
    o[QStringLiteral("path")] = QStringLiteral("enable");
    auto pfnReq = getProc<PFN_xrGetVulkanGraphicsRequirementsKHR>(instance, "xrGetVulkanGraphicsRequirementsKHR");
    auto pfnInstExt = getProc<PFN_xrGetVulkanInstanceExtensionsKHR>(instance, "xrGetVulkanInstanceExtensionsKHR");
    auto pfnDevExt = getProc<PFN_xrGetVulkanDeviceExtensionsKHR>(instance, "xrGetVulkanDeviceExtensionsKHR");
    auto pfnDevice = getProc<PFN_xrGetVulkanGraphicsDeviceKHR>(instance, "xrGetVulkanGraphicsDeviceKHR");
    if (pfnReq == nullptr || pfnInstExt == nullptr || pfnDevExt == nullptr || pfnDevice == nullptr) {
        o[QStringLiteral("error")] = QStringLiteral("xrGetInstanceProcAddr failed for vulkan_enable functions");
        ProbeUtil::printLine(kTag, "vulkan: enable function pointers unavailable");
        return;
    }

    XrGraphicsRequirementsVulkanKHR req = xrStruct<XrGraphicsRequirementsVulkanKHR>(XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR);
    XrResult r = pfnReq(instance, systemId, &req);
    if (!st.check(o, "xrGetVulkanGraphicsRequirementsKHR", r)) {
        ProbeUtil::printLine(kTag, "vulkan: requirements failed (%s)", qUtf8Printable(resultName(r)));
        return;
    }
    const QString minApi = xrVersionString(req.minApiVersionSupported);
    const QString maxApi = xrVersionString(req.maxApiVersionSupported);
    o[QStringLiteral("minApiVersion")] = minApi;
    o[QStringLiteral("maxApiVersion")] = maxApi;

    const QList<QByteArray> instExts = splitExtensionString(queryExtensionString(pfnInstExt, instance, systemId, &r));
    st.check(o, "xrGetVulkanInstanceExtensionsKHR", r);
    const QList<QByteArray> devExts = splitExtensionString(queryExtensionString(pfnDevExt, instance, systemId, &r));
    st.check(o, "xrGetVulkanDeviceExtensionsKHR", r);
    o[QStringLiteral("requiredInstanceExtensions")] = toJsonArray(instExts);
    o[QStringLiteral("requiredDeviceExtensions")] = toJsonArray(devExts);

    QString err;
    const PFN_vkGetInstanceProcAddr gipa = loadVulkanGipa(&err);
    if (gipa == nullptr) {
        o[QStringLiteral("error")] = err;
        ProbeUtil::printLine(kTag, "vulkan: minApi=%s maxApi=%s; %s", qUtf8Printable(minApi), qUtf8Printable(maxApi),
                             qUtf8Printable(err));
        return;
    }
    auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    if (createInstance == nullptr) {
        o[QStringLiteral("error")] = QStringLiteral("vkCreateInstance not found");
        return;
    }

    std::vector<const char*> extPtrs;
    extPtrs.reserve(static_cast<size_t>(instExts.size()));
    for (const QByteArray& e : instExts) {
        extPtrs.push_back(e.constData());
    }
    VkApplicationInfo ai;
    std::memset(&ai, 0, sizeof(ai));
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.pApplicationName = "VipleStream xr-probe";
    ai.pEngineName = "VipleStream";
    ai.apiVersion = vkApiFromXr(req.minApiVersionSupported);
    VkInstanceCreateInfo ici;
    std::memset(&ici, 0, sizeof(ici));
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &ai;
    ici.enabledExtensionCount = static_cast<uint32_t>(extPtrs.size());
    ici.ppEnabledExtensionNames = extPtrs.empty() ? nullptr : extPtrs.data();

    VkInstance vkInstance = VK_NULL_HANDLE;
    const VkResult vkr = createInstance(&ici, nullptr, &vkInstance);
    o[QStringLiteral("vkCreateInstance")] = static_cast<int>(vkr);
    if (vkr != VK_SUCCESS || vkInstance == VK_NULL_HANDLE) {
        ProbeUtil::printLine(kTag, "vulkan: minApi=%s maxApi=%s vkCreateInstance VkResult=%d (runtime instance extensions: %d)",
                             qUtf8Printable(minApi), qUtf8Printable(maxApi), static_cast<int>(vkr),
                             static_cast<int>(instExts.size()));
        return;
    }

    VkPhysicalDevice pd = VK_NULL_HANDLE;
    r = pfnDevice(instance, systemId, vkInstance, &pd);
    if (st.check(o, "xrGetVulkanGraphicsDeviceKHR", r) && pd != VK_NULL_HANDLE) {
        describePhysicalDevice(gipa, vkInstance, pd, QStringLiteral("enable"), minApi, maxApi, o);
    }
    else {
        ProbeUtil::printLine(kTag, "vulkan: minApi=%s maxApi=%s xrGetVulkanGraphicsDeviceKHR=%s",
                             qUtf8Printable(minApi), qUtf8Printable(maxApi), qUtf8Printable(resultName(r)));
    }
    destroyVkInstance(gipa, vkInstance);
}

#endif // VIPLE_XRPROBE_VULKAN

// applicationVersion：同 Android versionCode 的公式（major*10000 + minor*1000 + patch），只給 runtime 記錄用
uint32_t appVersionCode()
{
    const QStringList parts = QCoreApplication::applicationVersion().split(QLatin1Char('.'));
    if (parts.size() < 3) {
        return 0;
    }
    return parts.at(0).toUInt() * 10000u + parts.at(1).toUInt() * 1000u + parts.at(2).toUInt();
}

} // namespace

int xrProbeInstance(const XrProbeOptions& options, QJsonObject& out)
{
    Q_UNUSED(options);   // --session、環境覆寫都由 cli/xrprobe.cpp 在呼叫前處理
    ProbeState st;

    ProbeUtil::printLine(kTag, "loader: OpenXR headers %s", qUtf8Printable(xrVersionString(XR_CURRENT_API_VERSION)));
    out[QStringLiteral("headerVersion")] = xrVersionString(XR_CURRENT_API_VERSION);
    out[QStringLiteral("apiLayers")] = listApiLayers();

    // ── 擴充：列舉後才啟用 ──
    std::vector<XrExtensionProperties> exts;
    const XrResult enumResult = listExtensions(exts);
    out[QStringLiteral("xrEnumerateInstanceExtensionProperties")] = resultName(enumResult);
    if (XR_FAILED(enumResult)) {
        // 沒有 runtime 時 loader 通常在這一步就失敗；照樣試一次 xrCreateInstance（不帶擴充），
        // 讓 JSON 同時有兩個錯誤碼
        ProbeUtil::printLine(kTag, "extensions: xrEnumerateInstanceExtensionProperties failed (%s)",
                             qUtf8Printable(resultName(enumResult)));
    }

    const bool hasVk2 = hasExtension(exts, kExtVulkan2);
    const bool hasVk1 = hasExtension(exts, kExtVulkan1);
    const bool hasTimespec = hasExtension(exts, kExtTimespec);
    const bool hasEyeGaze = hasExtension(exts, kExtEyeGaze);

    std::vector<const char*> enable;
#if defined(VIPLE_XRPROBE_VULKAN)
    if (hasVk2) {
        enable.push_back(kExtVulkan2);
    }
    else if (hasVk1) {
        enable.push_back(kExtVulkan1);
    }
#endif
#if defined(XR_USE_TIMESPEC)
    if (hasTimespec) {
        enable.push_back(kExtTimespec);
    }
#endif
    if (hasEyeGaze) {
        enable.push_back(kExtEyeGaze);
    }
    for (const char* name : kWantedOptional) {
        if (hasExtension(exts, name)) {
            enable.push_back(name);
        }
    }

    QJsonArray extArr;
    QStringList valveNames;
    for (const XrExtensionProperties& e : exts) {
        bool enabled = false;
        for (const char* name : enable) {
            enabled = enabled || std::strcmp(e.extensionName, name) == 0;
        }
        QJsonObject ej;
        ej[QStringLiteral("name")] = QString::fromUtf8(e.extensionName);
        ej[QStringLiteral("version")] = static_cast<int>(e.extensionVersion);
        ej[QStringLiteral("enabled")] = enabled;
        extArr.append(ej);
        // 一行一個：SDL 單則 log 有長度上限（sdl2-compat 下的實際值 UNVERIFIED），不併成一行
        ProbeUtil::printLine(kTag, "ext: %s v%u%s", e.extensionName, e.extensionVersion, enabled ? " [enabled]" : "");
        if (std::strstr(e.extensionName, "VALVE") != nullptr) {
            valveNames << QString::fromUtf8(e.extensionName);
        }
    }
    out[QStringLiteral("extensions")] = extArr;
    out[QStringLiteral("valveExtensions")] = QJsonArray::fromStringList(valveNames);
    for (const QString& name : std::as_const(valveNames)) {
        ProbeUtil::printLine(kTag, "valve-ext: %s", qUtf8Printable(name));
    }

    {
        QJsonObject req;
        req[QStringLiteral("vulkanEnable2")] = hasVk2;
        req[QStringLiteral("vulkanEnable")] = hasVk1;
        req[QStringLiteral("convertTimespec")] = hasTimespec;
        req[QStringLiteral("tracking")] = hasTimespec ? QStringLiteral("thread") : QStringLiteral("frameloop");
        req[QStringLiteral("graphics")] = hasVk2 ? QStringLiteral("enable2")
                                                 : (hasVk1 ? QStringLiteral("enable") : QStringLiteral("none"));
        out[QStringLiteral("required")] = req;
        ProbeUtil::printLine(kTag, "required: vulkan_enable2=%d vulkan_enable=%d convert_timespec=%d -> tracking=%s graphics=%s",
                             hasVk2 ? 1 : 0, hasVk1 ? 1 : 0, hasTimespec ? 1 : 0,
                             hasTimespec ? "thread" : "frameloop",
                             hasVk2 ? "enable2" : (hasVk1 ? "enable" : "none"));
    }

    // ── xrCreateInstance：先 1.1，runtime 不支援再退 1.0 ──
#if defined(XR_API_VERSION_1_1)
    const XrVersion apiTry[] = {XR_API_VERSION_1_1, XR_API_VERSION_1_0};
#elif defined(XR_API_VERSION_1_0)
    const XrVersion apiTry[] = {XR_API_VERSION_1_0};
#else
    const XrVersion apiTry[] = {XR_MAKE_VERSION(1, 0, XR_VERSION_PATCH(XR_CURRENT_API_VERSION))};
#endif

    XrInstanceCreateInfo ci = xrStruct<XrInstanceCreateInfo>(XR_TYPE_INSTANCE_CREATE_INFO);
    qstrncpy(ci.applicationInfo.applicationName, "VipleStream xr-probe", sizeof(ci.applicationInfo.applicationName));
    qstrncpy(ci.applicationInfo.engineName, "VipleStream", sizeof(ci.applicationInfo.engineName));
    ci.applicationInfo.applicationVersion = appVersionCode();
    ci.applicationInfo.engineVersion = ci.applicationInfo.applicationVersion;
    ci.enabledExtensionCount = static_cast<uint32_t>(enable.size());
    ci.enabledExtensionNames = enable.empty() ? nullptr : enable.data();

    XrInstance instance = XR_NULL_HANDLE;
    XrResult r = XR_ERROR_RUNTIME_UNAVAILABLE;
    XrVersion usedApi = 0;
    QJsonArray attempts;
    const size_t tryCount = sizeof(apiTry) / sizeof(apiTry[0]);
    for (size_t i = 0; i < tryCount; i++) {
        ci.applicationInfo.apiVersion = apiTry[i];
        r = xrCreateInstance(&ci, &instance);
        QJsonObject a;
        a[QStringLiteral("apiVersion")] = xrVersionString(apiTry[i]);
        a[QStringLiteral("result")] = resultName(r);
        attempts.append(a);
        if (r != XR_ERROR_API_VERSION_UNSUPPORTED) {
            usedApi = apiTry[i];
            break;
        }
    }
    out[QStringLiteral("createInstanceAttempts")] = attempts;

    if (XR_FAILED(r) || instance == XR_NULL_HANDLE) {
        out[QStringLiteral("xrCreateInstance")] = resultName(r);
        ProbeUtil::printLine(kTag, "instance: xrCreateInstance failed (%s)", qUtf8Printable(resultName(r)));
        return ProbeUtil::kExitCapabilityAbsent;
    }
    out[QStringLiteral("xrCreateInstance")] = resultName(r);
    out[QStringLiteral("apiVersion")] = xrVersionString(usedApi);

    // ── instance 層 ──
    {
        QJsonObject io;
        XrInstanceProperties ip = xrStruct<XrInstanceProperties>(XR_TYPE_INSTANCE_PROPERTIES);
        r = xrGetInstanceProperties(instance, &ip);
        if (st.check(io, "xrGetInstanceProperties", r)) {
            io[QStringLiteral("runtimeName")] = QString::fromUtf8(ip.runtimeName);
            io[QStringLiteral("runtimeVersion")] = xrVersionString(ip.runtimeVersion);
        }
        out[QStringLiteral("instance")] = io;
        ProbeUtil::printLine(kTag, "instance: runtime=\"%s\" ver=%s api=%u.%u exts=%d enabled=%d",
                             XR_SUCCEEDED(r) ? ip.runtimeName : "?",
                             XR_SUCCEEDED(r) ? qUtf8Printable(xrVersionString(ip.runtimeVersion)) : "?",
                             static_cast<unsigned>(XR_VERSION_MAJOR(usedApi)),
                             static_cast<unsigned>(XR_VERSION_MINOR(usedApi)),
                             static_cast<int>(exts.size()), static_cast<int>(enable.size()));
    }

#if defined(XR_USE_TIMESPEC)
    if (hasTimespec) {
        probeTimespec(instance, out, st);
    }
#endif

    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    const bool eyeGazeEnabled = hasEyeGaze;
    if (!st.runtimeBroken && probeSystem(instance, eyeGazeEnabled, &systemId, out, st)) {
        if (!st.runtimeBroken) {
            probeViewConfigurations(instance, systemId, out, st);
        }
#if defined(VIPLE_XRPROBE_VULKAN)
        if (!st.runtimeBroken && (hasVk2 || hasVk1)) {
            QJsonObject vo;
            if (hasVk2) {
                probeVulkanEnable2(instance, systemId, vo, st);
            }
            else {
                probeVulkanEnable1(instance, systemId, vo, st);
            }
            out[QStringLiteral("vulkan")] = vo;
        }
        else if (!hasVk2 && !hasVk1) {
            ProbeUtil::printLine(kTag, "vulkan: runtime offers neither XR_KHR_vulkan_enable2 nor XR_KHR_vulkan_enable");
        }
#else
        ProbeUtil::printLine(kTag, "vulkan: skipped (built without Vulkan headers)");
#endif
    }

    // refresh rate／FOV／reference space 要 session（B 段，M3a）
    ProbeUtil::printLine(kTag, "note: refresh rate, FOV and reference spaces need a session (xr-probe --session, M3a)");

    // ── 一定 destroy ──
    r = xrDestroyInstance(instance);
    out[QStringLiteral("xrDestroyInstance")] = resultName(r);
    ProbeUtil::printLine(kTag, "instance: destroyed (%s)", qUtf8Printable(resultName(r)));

    if (st.runtimeBroken) {
        ProbeUtil::printLine(kTag, "result: runtime reported INSTANCE_LOST/RUNTIME_FAILURE during the probe (rc=14)");
        return ProbeUtil::kExitRuntimeError;
    }
    return ProbeUtil::kExitOk;
}
