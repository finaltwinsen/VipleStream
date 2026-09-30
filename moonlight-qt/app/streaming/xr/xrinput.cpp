// VipleStream 2.0 §VR M3a X4 — XrInput 實作。設計見 xrinput.h。

#include "xrinput.h"

#include <Limelight.h>
#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

constexpr float kPi = 3.14159265358979f;

// One-Euro 參數（UV 為 0..1 的正規化座標，速度單位 UV/s）
constexpr float kFilterMinCutoffHz = 1.5f;
constexpr float kFilterBeta = 10.0f;
constexpr float kFilterDCutoffHz = 1.0f;
// 按鍵遲滯與搖桿
constexpr float kPressOn = 0.75f;
constexpr float kPressOff = 0.60f;
constexpr float kStickDeadzone = 0.20f;
constexpr float kScrollUnitsPerSec = 1200.0f;  // 滿推 10 格／秒（120＝一格）

template <typename T>
T xrStruct(XrStructureType type)
{
    T s;
    std::memset(&s, 0, sizeof(s));
    s.type = type;
    return s;
}

QString xrStr(XrInstance instance, XrResult r)
{
    char buf[XR_MAX_RESULT_STRING_SIZE] = {};
    if (instance != XR_NULL_HANDLE && XR_SUCCEEDED(xrResultToString(instance, r, buf))) {
        return QString::fromLatin1(buf);
    }
    return QStringLiteral("XrResult(%1)").arg(static_cast<int>(r));
}

XrQuaternionf qmul(const XrQuaternionf& a, const XrQuaternionf& b)
{
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

XrQuaternionf yawQuat(float yaw)
{
    return {0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
}

XrRay::Vec3 sub(const XrRay::Vec3& a, const XrRay::Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
XrRay::Vec3 add(const XrRay::Vec3& a, const XrRay::Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
XrRay::Vec3 fromXr(const XrVector3f& v) { return {v.x, v.y, v.z}; }
XrRay::Vec3 normalize(const XrRay::Vec3& v)
{
    const float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return l > 1e-9f ? XrRay::Vec3{v.x / l, v.y / l, v.z / l} : v;
}

}  // namespace

// ─────────────────────────── XrRay（純數學） ───────────────────────────

namespace XrRay {

Vec3 rotate(const XrQuaternionf& q, const Vec3& v)
{
    const float tx = 2.0f * (q.y * v.z - q.z * v.y);
    const float ty = 2.0f * (q.z * v.x - q.x * v.z);
    const float tz = 2.0f * (q.x * v.y - q.y * v.x);
    return {v.x + q.w * tx + (q.y * tz - q.z * ty),
            v.y + q.w * ty + (q.z * tx - q.x * tz),
            v.z + q.w * tz + (q.x * ty - q.y * tx)};
}

Vec3 rotateInv(const XrQuaternionf& q, const Vec3& v)
{
    const XrQuaternionf c = {-q.x, -q.y, -q.z, q.w};
    return rotate(c, v);
}

bool intersectQuad(const XrPosef& pose, float w, float h, const Vec3& origin, const Vec3& dir,
                   float* u, float* v, float* t)
{
    const Vec3 o = rotateInv(pose.orientation, sub(origin, fromXr(pose.position)));
    const Vec3 d = rotateInv(pose.orientation, dir);
    if (std::fabs(d.z) < 1e-6f || w <= 0.0f || h <= 0.0f) {
        return false;
    }
    const float tt = -o.z / d.z;
    if (tt <= 0.0f) {
        return false;
    }
    const float x = o.x + tt * d.x;
    const float y = o.y + tt * d.y;
    const float uu = x / w + 0.5f;
    const float vv = 0.5f - y / h;
    if (uu < 0.0f || uu > 1.0f || vv < 0.0f || vv > 1.0f) {
        return false;
    }
    *u = uu;
    *v = vv;
    *t = tt;
    return true;
}

bool intersectCylinder(const XrPosef& pose, float radius, float centralAngle, float aspect,
                       const Vec3& origin, const Vec3& dir, float* u, float* v, float* t)
{
    if (radius <= 0.0f || centralAngle <= 0.0f || aspect <= 0.0f) {
        return false;
    }
    const Vec3 o = rotateInv(pose.orientation, sub(origin, fromXr(pose.position)));
    const Vec3 d = rotateInv(pose.orientation, dir);
    const float a = d.x * d.x + d.z * d.z;
    if (a < 1e-9f) {
        return false;  // 射線與軸平行
    }
    const float b = 2.0f * (o.x * d.x + o.z * d.z);
    const float c = o.x * o.x + o.z * o.z - radius * radius;
    const float disc = b * b - 4.0f * a * c;
    if (disc < 0.0f) {
        return false;
    }
    const float tt = (-b + std::sqrt(disc)) / (2.0f * a);  // 從圓柱內出發：取較遠（正）的根
    if (tt <= 0.0f) {
        return false;
    }
    const float x = o.x + tt * d.x;
    const float y = o.y + tt * d.y;
    const float z = o.z + tt * d.z;
    const float theta = std::atan2(x, -z);  // 0 在 -Z，往 +X 為正
    const float height = radius * centralAngle / aspect;
    const float uu = theta / centralAngle + 0.5f;
    const float vv = 0.5f - y / height;
    if (uu < 0.0f || uu > 1.0f || vv < 0.0f || vv > 1.0f) {
        return false;
    }
    *u = uu;
    *v = vv;
    *t = tt;
    return true;
}

Vec3 quadPoint(const XrPosef& pose, float w, float h, float u, float v)
{
    const Vec3 local = {(u - 0.5f) * w, (0.5f - v) * h, 0.0f};
    return add(fromXr(pose.position), rotate(pose.orientation, local));
}

Vec3 cylinderPoint(const XrPosef& pose, float radius, float centralAngle, float aspect, float u, float v)
{
    const float theta = (u - 0.5f) * centralAngle;
    const float height = radius * centralAngle / aspect;
    const Vec3 local = {radius * std::sin(theta), (0.5f - v) * height, -radius * std::cos(theta)};
    return add(fromXr(pose.position), rotate(pose.orientation, local));
}

float OneEuro::filter(float x, float dtSec)
{
    auto alpha = [](float cutoff, float dt) {
        const float tau = 1.0f / (2.0f * kPi * cutoff);
        return 1.0f / (1.0f + tau / dt);
    };
    if (!m_Init || dtSec <= 0.0f) {
        m_Init = true;
        m_X = x;
        m_Dx = 0.0f;
        m_LastCutoff = m_MinCutoff;
        return x;
    }
    const float dx = (x - m_X) / dtSec;
    m_Dx = m_Dx + alpha(m_DCutoff, dtSec) * (dx - m_Dx);
    m_LastCutoff = m_MinCutoff + m_Beta * std::fabs(m_Dx);
    m_X = m_X + alpha(m_LastCutoff, dtSec) * (x - m_X);
    return m_X;
}

int selfTest(const std::function<void(const char*, bool, const QString&)>& report)
{
    int failures = 0;
    auto approx = [](float a, float b, float eps) { return std::fabs(a - b) <= eps; };
    auto check = [&](const char* name, bool ok, const QString& detail) {
        if (!ok) {
            failures++;
        }
        report(name, ok, detail);
    };
    const Vec3 zero = {0.0f, 0.0f, 0.0f};
    XrPosef qp;
    qp.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
    qp.position = {0.0f, 0.0f, -2.0f};
    float u = -1, v = -1, t = -1;

    bool hit = intersectQuad(qp, 2.0f, 1.0f, zero, {0.0f, 0.0f, -1.0f}, &u, &v, &t);
    check("quad-center", hit && approx(u, 0.5f, 1e-4f) && approx(v, 0.5f, 1e-4f) && approx(t, 2.0f, 1e-4f),
          QStringLiteral("hit=%1 u=%2 v=%3 t=%4").arg(hit).arg(u).arg(v).arg(t));

    hit = intersectQuad(qp, 2.0f, 1.0f, zero, normalize({0.5f, 0.25f, -2.0f}), &u, &v, &t);
    check("quad-upper-right", hit && approx(u, 0.75f, 1e-4f) && approx(v, 0.25f, 1e-4f),
          QStringLiteral("hit=%1 u=%2 v=%3").arg(hit).arg(u).arg(v));

    hit = intersectQuad(qp, 2.0f, 1.0f, zero, normalize({2.0f, 0.0f, -2.0f}), &u, &v, &t);
    check("quad-miss-side", !hit, QStringLiteral("hit=%1").arg(hit));

    hit = intersectQuad(qp, 2.0f, 1.0f, zero, {0.0f, 0.0f, 1.0f}, &u, &v, &t);
    check("quad-miss-behind", !hit, QStringLiteral("hit=%1").arg(hit));

    XrPosef qr;  // 轉 +90°（yaw）：-Z 轉到 -X，螢幕在 (-2,0,0) 面向原點
    qr.orientation = yawQuat(kPi * 0.5f);
    qr.position = {-2.0f, 0.0f, 0.0f};
    hit = intersectQuad(qr, 2.0f, 1.0f, zero, {-1.0f, 0.0f, 0.0f}, &u, &v, &t);
    check("quad-rotated-center", hit && approx(u, 0.5f, 1e-4f) && approx(v, 0.5f, 1e-4f),
          QStringLiteral("hit=%1 u=%2 v=%3").arg(hit).arg(u).arg(v));

    const Vec3 rp = quadPoint(qr, 2.0f, 1.0f, 0.2f, 0.8f);
    hit = intersectQuad(qr, 2.0f, 1.0f, zero, normalize(rp), &u, &v, &t);
    check("quad-roundtrip", hit && approx(u, 0.2f, 1e-3f) && approx(v, 0.8f, 1e-3f),
          QStringLiteral("hit=%1 u=%2 v=%3").arg(hit).arg(u).arg(v));

    XrPosef cp;
    cp.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
    cp.position = {0.0f, 0.0f, 0.0f};
    const float ang = 60.0f * kPi / 180.0f;
    const float aspect = 16.0f / 9.0f;
    hit = intersectCylinder(cp, 1.5f, ang, aspect, zero, {0.0f, 0.0f, -1.0f}, &u, &v, &t);
    check("cyl-center", hit && approx(u, 0.5f, 1e-4f) && approx(v, 0.5f, 1e-4f) && approx(t, 1.5f, 1e-4f),
          QStringLiteral("hit=%1 u=%2 v=%3 t=%4").arg(hit).arg(u).arg(v).arg(t));

    const float a20 = 20.0f * kPi / 180.0f;
    hit = intersectCylinder(cp, 1.5f, ang, aspect, zero, {std::sin(a20), 0.0f, -std::cos(a20)}, &u, &v, &t);
    check("cyl-right-20deg", hit && approx(u, 20.0f / 60.0f + 0.5f, 1e-4f) && approx(v, 0.5f, 1e-4f),
          QStringLiteral("hit=%1 u=%2 v=%3").arg(hit).arg(u).arg(v));

    const float a40 = 40.0f * kPi / 180.0f;
    hit = intersectCylinder(cp, 1.5f, ang, aspect, zero, {std::sin(a40), 0.0f, -std::cos(a40)}, &u, &v, &t);
    check("cyl-miss-40deg", !hit, QStringLiteral("hit=%1").arg(hit));

    XrPosef cr = cp;
    cr.orientation = yawQuat(0.7f);
    cr.position = {0.3f, 1.6f, -0.2f};
    const Vec3 head = {0.35f, 1.55f, -0.1f};  // 軸附近（圓柱內）
    const Vec3 cpnt = cylinderPoint(cr, 1.5f, ang, aspect, 0.3f, 0.7f);
    hit = intersectCylinder(cr, 1.5f, ang, aspect, head, normalize(sub(cpnt, head)), &u, &v, &t);
    check("cyl-roundtrip-offaxis", hit && approx(u, 0.3f, 1e-3f) && approx(v, 0.7f, 1e-3f),
          QStringLiteral("hit=%1 u=%2 v=%3").arg(hit).arg(u).arg(v));

    OneEuro f(kFilterMinCutoffHz, kFilterBeta, kFilterDCutoffHz);
    float y = 0.0f;
    for (int i = 0; i < 200; i++) {
        y = f.filter(0.4f, 1.0f / 90.0f);
    }
    check("oneeuro-constant", approx(y, 0.4f, 1e-5f), QStringLiteral("y=%1").arg(y));
    f.reset();
    f.filter(0.0f, 1.0f / 90.0f);
    const float step = f.filter(1.0f, 1.0f / 90.0f);
    check("oneeuro-step-lags", step > 0.0f && step < 1.0f, QStringLiteral("first step output=%1").arg(step));
    return failures;
}

}  // namespace XrRay

// ─────────────────────────── XrInput ───────────────────────────

XrInput::XrInput(const XrInputSink& sink, bool testPointer, const QString& testKeyboardText)
    : m_Sink(sink),
      m_TestPointer(testPointer),
      m_FiltU(kFilterMinCutoffHz, kFilterBeta, kFilterDCutoffHz),
      m_FiltV(kFilterMinCutoffHz, kFilterBeta, kFilterDCutoffHz),
      m_TestKbText(testKeyboardText)
{
    // dev --xr-test-keyboard：把文字展開成按鍵序列（大寫／Shift 字元先點一次黏滯 Shift）
    const int shiftKey = m_Kb.findKind(XrKeyboard::Kind::Shift);
    for (const QChar c : m_TestKbText) {
        int idx = -1;
        bool needShift = false;
        if (!m_Kb.keyForChar(c, &idx, &needShift)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR-INPUT] dev test keyboard: no key for U+%04X, skipped",
                        static_cast<unsigned>(c.unicode()));
            continue;
        }
        if (needShift && shiftKey >= 0) {
            m_TestKbSteps.push_back({shiftKey});
        }
        m_TestKbSteps.push_back({idx});
    }
}

XrInput::~XrInput()
{
    destroy();
}

bool XrInput::create(XrInstance instance, XrSession session, const QStringList& enabledExts, QString* error)
{
    m_Instance = instance;
    m_Session = session;
    xrStringToPath(instance, "/user/hand/left", &m_Hands[0]);
    xrStringToPath(instance, "/user/hand/right", &m_Hands[1]);

    XrActionSetCreateInfo asci = xrStruct<XrActionSetCreateInfo>(XR_TYPE_ACTION_SET_CREATE_INFO);
    qstrncpy(asci.actionSetName, "viple_xr", sizeof(asci.actionSetName));
    qstrncpy(asci.localizedActionSetName, "VipleStream XR", sizeof(asci.localizedActionSetName));
    XrResult r = xrCreateActionSet(instance, &asci, &m_Set);
    if (XR_FAILED(r)) {
        *error = QStringLiteral("xrCreateActionSet: %1").arg(xrStr(instance, r));
        return false;
    }
    auto mk = [&](const char* name, const char* loc, XrActionType type, XrAction* out) {
        XrActionCreateInfo aci = xrStruct<XrActionCreateInfo>(XR_TYPE_ACTION_CREATE_INFO);
        qstrncpy(aci.actionName, name, sizeof(aci.actionName));
        qstrncpy(aci.localizedActionName, loc, sizeof(aci.localizedActionName));
        aci.actionType = type;
        aci.countSubactionPaths = 2;
        aci.subactionPaths = m_Hands;
        const XrResult rr = xrCreateAction(m_Set, &aci, out);
        if (XR_FAILED(rr)) {
            *error = QStringLiteral("xrCreateAction(%1): %2").arg(QLatin1String(name)).arg(xrStr(instance, rr));
            return false;
        }
        return true;
    };
    if (!mk("aim", "Aim", XR_ACTION_TYPE_POSE_INPUT, &m_Aim) ||
        !mk("trigger", "Trigger (left click)", XR_ACTION_TYPE_FLOAT_INPUT, &m_Trigger) ||
        !mk("squeeze", "Grip (right click)", XR_ACTION_TYPE_FLOAT_INPUT, &m_Squeeze) ||
        !mk("secondary", "B (right click)", XR_ACTION_TYPE_BOOLEAN_INPUT, &m_Secondary) ||
        !mk("scroll", "Thumbstick (scroll)", XR_ACTION_TYPE_VECTOR2F_INPUT, &m_Stick) ||
        !mk("keyboard", "Virtual keyboard (toggle)", XR_ACTION_TYPE_BOOLEAN_INPUT, &m_KbToggleAction)) {
        return false;
    }

    struct B {
        XrAction action;
        const char* path;
    };
    auto suggest = [&](const char* profile, const std::vector<B>& list) -> bool {
        XrPath prof = XR_NULL_PATH;
        if (XR_FAILED(xrStringToPath(instance, profile, &prof))) {
            return false;
        }
        std::vector<XrActionSuggestedBinding> sb;
        for (const B& b : list) {
            XrPath p = XR_NULL_PATH;
            if (XR_SUCCEEDED(xrStringToPath(instance, b.path, &p))) {
                sb.push_back({b.action, p});
            }
        }
        XrInteractionProfileSuggestedBinding isb =
            xrStruct<XrInteractionProfileSuggestedBinding>(XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING);
        isb.interactionProfile = prof;
        isb.countSuggestedBindings = static_cast<uint32_t>(sb.size());
        isb.suggestedBindings = sb.data();
        const XrResult rr = xrSuggestInteractionProfileBindings(instance, &isb);
        if (XR_SUCCEEDED(rr)) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR-INPUT] bindings accepted: %s (%zu)", profile, sb.size());
        }
        else {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR-INPUT] bindings rejected: %s: %s", profile,
                        qUtf8Printable(xrStr(instance, rr)));
        }
        return XR_SUCCEEDED(rr);
    };
    // 鍵盤開關鍵是選用綁定：同一 profile 的建議會整組被接受或拒絕，路徑不被 runtime 認得時
    // 去掉它重送，射線滑鼠的綁定不受影響（每次建議會取代同 profile 的前一次）
    auto suggestWithKb = [&](const char* profile, std::vector<B> list, const char* kbPath) {
        std::vector<B> withKb = list;
        withKb.push_back({m_KbToggleAction, kbPath});
        if (!suggest(profile, withKb)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-XR-INPUT] %s: retrying without the keyboard toggle (%s)", profile, kbPath);
            suggest(profile, list);
        }
    };

    suggest("/interaction_profiles/khr/simple_controller",
            {{m_Aim, "/user/hand/left/input/aim/pose"},
             {m_Aim, "/user/hand/right/input/aim/pose"},
             {m_Trigger, "/user/hand/left/input/select/click"},
             {m_Trigger, "/user/hand/right/input/select/click"},
             {m_Secondary, "/user/hand/left/input/menu/click"},
             {m_Secondary, "/user/hand/right/input/menu/click"}});
    const std::vector<B> touchLike = {
        {m_Aim, "/user/hand/left/input/aim/pose"},
        {m_Aim, "/user/hand/right/input/aim/pose"},
        {m_Trigger, "/user/hand/left/input/trigger/value"},
        {m_Trigger, "/user/hand/right/input/trigger/value"},
        {m_Squeeze, "/user/hand/left/input/squeeze/value"},
        {m_Squeeze, "/user/hand/right/input/squeeze/value"},
        {m_Secondary, "/user/hand/left/input/y/click"},
        {m_Secondary, "/user/hand/right/input/b/click"},
        {m_Stick, "/user/hand/left/input/thumbstick"},
        {m_Stick, "/user/hand/right/input/thumbstick"},
    };
    suggestWithKb("/interaction_profiles/oculus/touch_controller", touchLike, "/user/hand/left/input/menu/click");
    suggestWithKb("/interaction_profiles/valve/index_controller",
            {{m_Aim, "/user/hand/left/input/aim/pose"},
             {m_Aim, "/user/hand/right/input/aim/pose"},
             {m_Trigger, "/user/hand/left/input/trigger/value"},
             {m_Trigger, "/user/hand/right/input/trigger/value"},
             {m_Squeeze, "/user/hand/left/input/squeeze/value"},
             {m_Squeeze, "/user/hand/right/input/squeeze/value"},
             {m_Secondary, "/user/hand/left/input/b/click"},
             {m_Secondary, "/user/hand/right/input/b/click"},
             {m_Stick, "/user/hand/left/input/thumbstick"},
             {m_Stick, "/user/hand/right/input/thumbstick"}},
            "/user/hand/left/input/thumbstick/click");
    if (enabledExts.contains(QStringLiteral("XR_VALVE_frame_controller_interaction"))) {
        // XR_VALVE_frame_controller_interaction（2026-09-30 依 Valve OpenXR Unity 套件文件
        // ValveSoftware/Unity com.valvesoftware.openxr.utils 的 Steam Frame Controller Profile）：
        // profile 是 /interaction_profiles/valve/frame_controller_valve（不是 frame_controller——
        // 舊路徑被 SteamVR 2.17.10 以 PATH_UNSUPPORTED 拒絕）。trigger、squeeze 有 value／click、
        // thumbstick 同 Touch；面鍵左右不同：右手 a/b/x/y，左手 dpad_up/left/down/right；另有
        // shoulder、menu（右）／view（左）、system。右鍵取「外側」面鍵：右 b、左 dpad_left。
        suggestWithKb("/interaction_profiles/valve/frame_controller_valve",
                {{m_Aim, "/user/hand/left/input/aim/pose"},
                 {m_Aim, "/user/hand/right/input/aim/pose"},
                 {m_Trigger, "/user/hand/left/input/trigger/value"},
                 {m_Trigger, "/user/hand/right/input/trigger/value"},
                 {m_Squeeze, "/user/hand/left/input/squeeze/value"},
                 {m_Squeeze, "/user/hand/right/input/squeeze/value"},
                 {m_Secondary, "/user/hand/left/input/dpad_left/click"},
                 {m_Secondary, "/user/hand/right/input/b/click"},
                 {m_Stick, "/user/hand/left/input/thumbstick"},
                 {m_Stick, "/user/hand/right/input/thumbstick"}},
                "/user/hand/left/input/view/click");
    }

    XrSessionActionSetsAttachInfo ai = xrStruct<XrSessionActionSetsAttachInfo>(XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO);
    ai.countActionSets = 1;
    ai.actionSets = &m_Set;
    r = xrAttachSessionActionSets(session, &ai);
    if (XR_FAILED(r)) {
        *error = QStringLiteral("xrAttachSessionActionSets: %1").arg(xrStr(instance, r));
        return false;
    }
    for (int h = 0; h < 2; h++) {
        XrActionSpaceCreateInfo sci = xrStruct<XrActionSpaceCreateInfo>(XR_TYPE_ACTION_SPACE_CREATE_INFO);
        sci.action = m_Aim;
        sci.subactionPath = m_Hands[h];
        sci.poseInActionSpace.orientation.w = 1.0f;
        r = xrCreateActionSpace(session, &sci, &m_AimSpace[h]);
        if (XR_FAILED(r)) {
            m_AimSpace[h] = XR_NULL_HANDLE;
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR-INPUT] aim space (%s): %s", h ? "right" : "left",
                        qUtf8Printable(xrStr(instance, r)));
        }
    }
    if (m_TestPointer) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-XR-INPUT] dev --xr-test-pointer: synthetic ray (4 s circle, trigger at the top), controllers ignored");
    }
    if (!m_TestKbText.isEmpty()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-XR-INPUT] dev --xr-test-keyboard: %zu key presses queued (keyboard opens 5 s after the screen is placed)",
                    m_TestKbSteps.size());
    }
    return true;
}

void XrInput::destroy()
{
    for (XrSpace& s : m_AimSpace) {
        if (s != XR_NULL_HANDLE) {
            xrDestroySpace(s);
            s = XR_NULL_HANDLE;
        }
    }
    for (XrAction* a : {&m_Aim, &m_Trigger, &m_Squeeze, &m_Secondary, &m_Stick, &m_KbToggleAction}) {
        if (*a != XR_NULL_HANDLE) {
            xrDestroyAction(*a);
            *a = XR_NULL_HANDLE;
        }
    }
    if (m_Set != XR_NULL_HANDLE) {
        xrDestroyActionSet(m_Set);
        m_Set = XR_NULL_HANDLE;
    }
}

void XrInput::onProfileChanged()
{
    for (int h = 0; h < 2; h++) {
        XrInteractionProfileState st = xrStruct<XrInteractionProfileState>(XR_TYPE_INTERACTION_PROFILE_STATE);
        if (XR_FAILED(xrGetCurrentInteractionProfile(m_Session, m_Hands[h], &st))) {
            continue;
        }
        char buf[XR_MAX_PATH_LENGTH] = "(none)";
        uint32_t n = 0;
        if (st.interactionProfile != XR_NULL_PATH) {
            xrPathToString(m_Instance, st.interactionProfile, sizeof(buf), &n, buf);
        }
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR-INPUT] interaction profile %s: %s", h ? "right" : "left", buf);
    }
}

void XrInput::setButton(int idx, bool pressed)
{
    if (m_Pressed[idx] == pressed) {
        return;
    }
    m_Pressed[idx] = pressed;
    m_StatButtons++;
    if (m_Sink.button) {
        m_Sink.button(idx == 0 ? BUTTON_LEFT : BUTTON_RIGHT, pressed);
    }
}

void XrInput::releaseAll(const char* why)
{
    bool any = false;
    for (int i = 0; i < 2; i++) {
        if (m_Pressed[i]) {
            any = true;
            setButton(i, false);
        }
    }
    m_TriggerDown[0] = m_TriggerDown[1] = false;
    m_TestTriggerDown = false;
    m_ScrollAccum = 0.0f;
    if (any) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR-INPUT] released all buttons (%s)", why);
    }
}

bool XrInput::locateAim(int hand, XrTime t, XrSpace local, XrRay::Vec3* origin, XrRay::Vec3* dir)
{
    if (m_AimSpace[hand] == XR_NULL_HANDLE) {
        return false;
    }
    XrSpaceLocation loc = xrStruct<XrSpaceLocation>(XR_TYPE_SPACE_LOCATION);
    if (XR_FAILED(xrLocateSpace(m_AimSpace[hand], local, t, &loc)) ||
        !(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) ||
        !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
        return false;
    }
    *origin = fromXr(loc.pose.position);
    *dir = XrRay::rotate(loc.pose.orientation, {0.0f, 0.0f, -1.0f});
    return true;
}

bool XrInput::syntheticRay(XrTime t, XrSpace local, XrSpace view, const Screen& s, uint64_t nowNs,
                           XrRay::Vec3* origin, XrRay::Vec3* dir, bool* trigger)
{
    XrSpaceLocation loc = xrStruct<XrSpaceLocation>(XR_TYPE_SPACE_LOCATION);
    if (XR_FAILED(xrLocateSpace(view, local, t, &loc)) || !(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
        return false;
    }
    if (m_TestStartNs == 0) {
        m_TestStartNs = nowNs;
    }
    const double elapsed = static_cast<double>(nowNs - m_TestStartNs) / 1e9;
    const int cycle = static_cast<int>(elapsed / 4.0);
    const float f = static_cast<float>(elapsed / 4.0 - cycle);
    const float a = 2.0f * kPi * f;
    const float u = 0.5f + 0.15f * std::cos(a);
    const float v = 0.5f - 0.25f * std::sin(a);  // f=0.25 時在圓的頂點
    const XrRay::Vec3 target = s.cylinder ? XrRay::cylinderPoint(s.pose, s.radiusM, s.angleRad, s.aspect, u, v)
                                          : XrRay::quadPoint(s.pose, s.widthM, s.heightM, u, v);
    *origin = fromXr(loc.pose.position);
    *dir = normalize(sub(target, *origin));
    *trigger = f > 0.23f && f < 0.27f;  // 頂點附近按下約 160 ms
    if (cycle != m_TestLastCycle) {
        m_TestLastCycle = cycle;
    }
    return true;
}

void XrInput::update(XrTime displayTime, XrSpace local, XrSpace view, const Screen& screen, bool focused, uint64_t nowNs)
{
    m_StatFrames++;
    if (!focused) {
        if (m_WasFocused) {
            releaseAll("focus lost");
            releaseKeyboard("focus lost");
            m_WasFocused = false;
        }
        m_Hit = false;
        m_FiltU.reset();
        m_FiltV.reset();
        return;
    }
    m_WasFocused = true;

    // ── 鍵盤開關：熱鍵要求與控制器（任一手）上升緣 ──
    for (int n = m_KbToggleReq.exchange(0, std::memory_order_acq_rel); n > 0; n--) {
        setKeyboardOpen(!m_KbOpen, "hotkey");
    }
    if (!m_TestPointer && m_TestKbText.isEmpty() && m_KbToggleAction != XR_NULL_HANDLE) {
        for (int h = 0; h < 2; h++) {
            XrActionStateGetInfo gi = xrStruct<XrActionStateGetInfo>(XR_TYPE_ACTION_STATE_GET_INFO);
            gi.action = m_KbToggleAction;
            gi.subactionPath = m_Hands[h];
            XrActionStateBoolean bs = xrStruct<XrActionStateBoolean>(XR_TYPE_ACTION_STATE_BOOLEAN);
            const bool now = XR_SUCCEEDED(xrGetActionStateBoolean(m_Session, &gi, &bs)) && bs.isActive && bs.currentState;
            if (now && !m_KbToggleWas[h]) {
                setKeyboardOpen(!m_KbOpen, "controller");
            }
            m_KbToggleWas[h] = now;
        }
    }

    if (!screen.valid) {
        m_Hit = false;
        return;
    }

    // ── 射線來源 ──
    XrRay::Vec3 origin = {0, 0, 0}, dir = {0, 0, -1};
    bool haveRay = false;
    bool leftDesired = false, rightDesired = false;
    float stickY = 0.0f;
    if (!m_TestKbText.isEmpty() && !m_TestKbDone) {
        bool trig = false;
        haveRay = syntheticKeyboardRay(displayTime, local, view, screen, nowNs, &origin, &dir, &trig);
        leftDesired = trig;
    }
    else if (m_TestPointer) {
        bool trig = false;
        haveRay = syntheticRay(displayTime, local, view, screen, nowNs, &origin, &dir, &trig);
        leftDesired = trig;
    }
    else {
        float trig[2] = {0.0f, 0.0f};
        for (int h = 0; h < 2; h++) {
            XrActionStateGetInfo gi = xrStruct<XrActionStateGetInfo>(XR_TYPE_ACTION_STATE_GET_INFO);
            gi.action = m_Trigger;
            gi.subactionPath = m_Hands[h];
            XrActionStateFloat fs = xrStruct<XrActionStateFloat>(XR_TYPE_ACTION_STATE_FLOAT);
            if (XR_SUCCEEDED(xrGetActionStateFloat(m_Session, &gi, &fs)) && fs.isActive) {
                trig[h] = fs.currentState;
            }
            const bool was = m_TriggerDown[h];
            m_TriggerDown[h] = was ? trig[h] > kPressOff : trig[h] > kPressOn;
            if (!was && m_TriggerDown[h] && h != m_Dominant) {
                // 慣用手＝最近按過 trigger 的那隻；切換時放開另一隻手留下的按鍵
                releaseAll("dominant hand change");
                m_Dominant = h;
                m_TriggerDown[h] = true;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR-INPUT] dominant hand: %s", h ? "right" : "left");
            }
        }
        const int d = m_Dominant;
        leftDesired = m_TriggerDown[d];

        XrActionStateGetInfo gi = xrStruct<XrActionStateGetInfo>(XR_TYPE_ACTION_STATE_GET_INFO);
        gi.subactionPath = m_Hands[d];
        gi.action = m_Squeeze;
        XrActionStateFloat sq = xrStruct<XrActionStateFloat>(XR_TYPE_ACTION_STATE_FLOAT);
        const bool squeeze = XR_SUCCEEDED(xrGetActionStateFloat(m_Session, &gi, &sq)) && sq.isActive &&
                             sq.currentState > (m_Pressed[1] ? kPressOff : kPressOn);
        gi.action = m_Secondary;
        XrActionStateBoolean sb = xrStruct<XrActionStateBoolean>(XR_TYPE_ACTION_STATE_BOOLEAN);
        const bool secondary = XR_SUCCEEDED(xrGetActionStateBoolean(m_Session, &gi, &sb)) && sb.isActive && sb.currentState;
        rightDesired = squeeze || secondary;
        gi.action = m_Stick;
        XrActionStateVector2f st = xrStruct<XrActionStateVector2f>(XR_TYPE_ACTION_STATE_VECTOR2F);
        if (XR_SUCCEEDED(xrGetActionStateVector2f(m_Session, &gi, &st)) && st.isActive) {
            stickY = st.currentState.y;
        }
        haveRay = locateAim(d, displayTime, local, &origin, &dir);
    }

    // ── 虛擬鍵盤（優先於影像螢幕）──
    bool kbHit = false;
    if (m_KbOpen && screen.kbValid && haveRay) {
        float ku = 0.0f, kv = 0.0f, kt = 0.0f;
        if (XrRay::intersectQuad(screen.kbPose, screen.kbWidthM, screen.kbHeightM, origin, dir, &ku, &kv, &kt)) {
            kbHit = true;
            m_KbHover = m_Kb.keyAt(ku, kv);
            const XrRay::Vec3 p = XrRay::quadPoint(screen.kbPose, screen.kbWidthM, screen.kbHeightM, ku, kv);
            m_HitPose.position = {p.x, p.y, p.z};
            m_HitPose.orientation = screen.kbPose.orientation;
        }
    }
    if (!kbHit) {
        m_KbHover = -1;
    }
    // trigger 上升緣落在鍵上＝按鍵按下；下降緣放開（按住時 host 端自己連發）
    if (leftDesired && !m_KbTrigPrev && kbHit && m_KbHover >= 0 && !m_Pressed[0]) {
        kbPress(m_KbHover);
    }
    else if (!leftDesired && m_KbTrigPrev) {
        kbRelease();
        m_TrigOnKb = false;
    }
    m_KbTrigPrev = leftDesired;
    m_KbHit = kbHit;
    if (kbHit) {
        // 指標畫在鍵盤上；不送滑鼠移動、不按下滑鼠鍵（已按住的照常放開）
        m_Hit = true;
        m_StatHits++;
        if (!leftDesired && m_Pressed[0]) {
            setButton(0, false);
        }
        if (!rightDesired && m_Pressed[1]) {
            setButton(1, false);
        }
        m_ScrollAccum = 0.0f;
        m_FiltU.reset();  // 回到螢幕時直接跳到命中點
        m_FiltV.reset();
        m_LastFilterNs = nowNs;
        return;
    }

    // ── 求交 ──
    float u = 0.0f, v = 0.0f, t = 0.0f;
    const bool hit = haveRay && (screen.cylinder
                                     ? XrRay::intersectCylinder(screen.pose, screen.radiusM, screen.angleRad, screen.aspect,
                                                                origin, dir, &u, &v, &t)
                                     : XrRay::intersectQuad(screen.pose, screen.widthM, screen.heightM, origin, dir, &u, &v, &t));
    const float dt = m_LastFilterNs == 0 ? 0.0f
                                         : std::clamp(static_cast<float>(nowNs - m_LastFilterNs) / 1e9f, 0.001f, 0.1f);
    m_LastFilterNs = nowNs;
    if (hit) {
        if (!m_Hit) {
            m_FiltU.reset();  // 重新進入螢幕：直接跳到命中點，不從舊位置滑過來
            m_FiltV.reset();
        }
        m_U = m_FiltU.filter(u, dt);
        m_V = m_FiltV.filter(v, dt);
        m_StatHits++;
        const float cutoff = 0.5f * (m_FiltU.lastCutoffHz() + m_FiltV.lastCutoffHz());
        if (cutoff > 0.0f) {
            m_StatLatencySumMs += 1000.0 / (2.0 * kPi * cutoff);
            m_StatLatencyN++;
        }
        // 指標 quad：擺在濾波後的點上，朝向與螢幕一致（cylinder 面向軸心）
        if (screen.cylinder) {
            const XrRay::Vec3 p = XrRay::cylinderPoint(screen.pose, screen.radiusM, screen.angleRad, screen.aspect, m_U, m_V);
            m_HitPose.position = {p.x, p.y, p.z};
            m_HitPose.orientation = qmul(screen.pose.orientation, yawQuat(-(m_U - 0.5f) * screen.angleRad));
        }
        else {
            const XrRay::Vec3 p = XrRay::quadPoint(screen.pose, screen.widthM, screen.heightM, m_U, m_V);
            m_HitPose.position = {p.x, p.y, p.z};
            m_HitPose.orientation = screen.pose.orientation;
        }
        if (std::fabs(m_U - m_SentU) > 1e-4f || std::fabs(m_V - m_SentV) > 1e-4f) {
            m_SentU = m_U;
            m_SentV = m_V;
            m_StatMoves++;
            if (m_Sink.pointer) {
                m_Sink.pointer(m_U, m_V);
            }
        }
    }
    m_Hit = hit;

    // ── 按鍵：按下只在命中時；放開隨時。按下始於鍵盤的這次 trigger 不轉成滑鼠左鍵 ──
    if (leftDesired && !m_Pressed[0] && hit && !m_TrigOnKb) {
        setButton(0, true);
    }
    else if (!leftDesired && m_Pressed[0]) {
        setButton(0, false);
    }
    if (rightDesired && !m_Pressed[1] && hit) {
        setButton(1, true);
    }
    else if (!rightDesired && m_Pressed[1]) {
        setButton(1, false);
    }

    // ── 捲動：搖桿 Y（上推＝往上捲），命中時才送 ──
    if (hit && std::fabs(stickY) > kStickDeadzone && dt > 0.0f) {
        const float mag = (std::fabs(stickY) - kStickDeadzone) / (1.0f - kStickDeadzone);
        m_ScrollAccum += (stickY > 0 ? 1.0f : -1.0f) * mag * kScrollUnitsPerSec * dt;
        const int amount = static_cast<int>(m_ScrollAccum);
        if (amount != 0) {
            m_ScrollAccum -= static_cast<float>(amount);
            m_StatScrolls++;
            if (m_Sink.scroll) {
                m_Sink.scroll(amount);
            }
        }
    }
    else {
        m_ScrollAccum = 0.0f;
    }
}

void XrInput::setKeyboardOpen(bool open, const char* why)
{
    if (open == m_KbOpen) {
        return;
    }
    if (!open) {
        releaseKeyboard(why);
    }
    m_KbOpen = open;
    m_KbDirty = true;
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR-INPUT] keyboard %s (%s)", open ? "opened" : "closed", why);
}

void XrInput::releaseKeyboard(const char* why)
{
    const bool had = m_KbActiveKey >= 0 || m_Sticky != 0;
    kbRelease();
    if (m_Sticky != 0) {
        m_Sticky = 0;
        m_KbDirty = true;
    }
    m_KbHover = -1;
    m_KbTrigPrev = false;
    m_TrigOnKb = false;
    if (had) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR-INPUT] keyboard keys and modifiers released (%s)", why);
    }
}

void XrInput::sendKey(int vk, bool down, uint8_t mods)
{
    if (!m_TestKbText.isEmpty()) {
        // dev 才逐鍵記錄（一般模式只記數量，避免 log 變成 keylogger）
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR-INPUT] dev key vk=0x%02X %s mods=0x%X", vk,
                    down ? "down" : "up", static_cast<unsigned>(mods));
    }
    if (down && !m_TestKbText.isEmpty()) {
        m_TestKbSent++;
    }
    if (m_Sink.key) {
        m_Sink.key(vk, down, mods);
    }
}

void XrInput::kbPress(int idx)
{
    using K = XrKeyboard::Kind;
    const XrKeyboard::Key& k = m_Kb.key(idx);
    m_TrigOnKb = true;
    switch (k.kind) {
    case K::Shift:
    case K::Ctrl:
    case K::Alt:
    case K::Win:
        // 黏滯：按一下保持，再按一下取消；下一個一般鍵放開後自動放開
        m_Sticky ^= XrKeyboard::modBit(k.kind);
        m_KbDirty = true;
        if (!m_TestKbText.isEmpty()) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR-INPUT] dev sticky modifiers=0x%X",
                        static_cast<unsigned>(m_Sticky));
        }
        break;
    case K::Close:
        setKeyboardOpen(false, "close key");
        break;
    case K::Ime:
        // 中/英：Shift 單擊（Windows 注音預設的中／英切換；見 xrkeyboard.h）
        sendKey(k.vk, true, MODIFIER_SHIFT);
        sendKey(k.vk, false, 0);
        m_StatKeys++;
        break;
    case K::Normal: {
        const uint8_t mods = m_Sticky;
        uint8_t cur = 0;
        for (K mk : {K::Ctrl, K::Alt, K::Shift, K::Win}) {
            if (mods & XrKeyboard::modBit(mk)) {
                cur |= XrKeyboard::modBit(mk);
                sendKey(XrKeyboard::modVk(mk), true, cur);
            }
        }
        sendKey(k.vk, true, mods);
        m_KbActiveKey = idx;
        m_KbActiveMods = mods;
        m_StatKeys++;
        break;
    }
    }
}

void XrInput::kbRelease()
{
    using K = XrKeyboard::Kind;
    if (m_KbActiveKey < 0) {
        return;
    }
    uint8_t cur = m_KbActiveMods;
    sendKey(m_Kb.key(m_KbActiveKey).vk, false, cur);
    for (K mk : {K::Win, K::Shift, K::Alt, K::Ctrl}) {
        if (cur & XrKeyboard::modBit(mk)) {
            cur &= static_cast<uint8_t>(~XrKeyboard::modBit(mk));
            sendKey(XrKeyboard::modVk(mk), false, cur);
        }
    }
    m_KbActiveKey = -1;
    m_KbActiveMods = 0;
    if (m_Sticky != 0) {
        m_Sticky = 0;  // 黏滯修飾鍵只作用一個鍵
        m_KbDirty = true;
    }
}

bool XrInput::syntheticKeyboardRay(XrTime t, XrSpace local, XrSpace view, const Screen& s, uint64_t nowNs,
                                   XrRay::Vec3* origin, XrRay::Vec3* dir, bool* trigger)
{
    *trigger = false;
    if (m_TestKbStartNs == 0) {
        m_TestKbStartNs = nowNs;
    }
    const double elapsed = static_cast<double>(nowNs - m_TestKbStartNs) / 1e9;
    if (!m_TestKbOpened) {
        if (elapsed < 5.0) {
            return false;
        }
        m_TestKbOpened = true;
        setKeyboardOpen(true, "dev test");
        return false;
    }
    if (!s.kbValid) {
        return false;  // 開啟後，貼圖畫好並擺好才顯示
    }
    if (m_TestKbShownNs == 0) {
        m_TestKbShownNs = nowNs;
    }
    constexpr double kStep = 0.6;  // 每鍵：0.30 s 瞄準、0.12 s 按下、0.18 s 放開
    const double seq = static_cast<double>(nowNs - m_TestKbShownNs) / 1e9 - 1.0;
    if (seq < 0.0) {
        return false;
    }
    const int step = static_cast<int>(seq / kStep);
    if (step >= static_cast<int>(m_TestKbSteps.size())) {
        m_TestKbDone = true;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-XR-INPUT] dev test keyboard done: %zu presses, %d keys sent",
                    m_TestKbSteps.size(), m_TestKbSent);
        return false;
    }
    const double f = seq / kStep - step;
    XrSpaceLocation loc = xrStruct<XrSpaceLocation>(XR_TYPE_SPACE_LOCATION);
    if (XR_FAILED(xrLocateSpace(view, local, t, &loc)) || !(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
        return false;
    }
    float cu = 0.0f, cv = 0.0f, du = 0.0f, dv = 0.0f;
    m_Kb.keyUv(m_TestKbSteps[static_cast<size_t>(step)].key, &cu, &cv, &du, &dv);
    const XrRay::Vec3 target = XrRay::quadPoint(s.kbPose, s.kbWidthM, s.kbHeightM, cu, cv);
    *origin = fromXr(loc.pose.position);
    *dir = normalize(sub(target, *origin));
    *trigger = f >= 0.5 && f < 0.7;
    return true;
}

bool XrInput::pointerPose(XrPosef* pose) const
{
    if (!m_Hit) {
        return false;
    }
    *pose = m_HitPose;
    return true;
}

QString XrInput::takeStatsLine()
{
    const double hitPct = m_StatFrames ? 100.0 * static_cast<double>(m_StatHits) / static_cast<double>(m_StatFrames) : 0.0;
    const double lat = m_StatLatencyN ? m_StatLatencySumMs / static_cast<double>(m_StatLatencyN) : 0.0;
    const QString line = QStringLiteral("frames=%1 hit=%2% moves=%3 buttons=%4 scrolls=%5 hand=%6 filterLatency~%7 ms "
                                        "keyboard=%9 keys=%10 sticky=0x%11%8")
                             .arg(m_StatFrames)
                             .arg(hitPct, 0, 'f', 1)
                             .arg(m_StatMoves)
                             .arg(m_StatButtons)
                             .arg(m_StatScrolls)
                             .arg(m_Dominant ? QStringLiteral("right") : QStringLiteral("left"))
                             .arg(lat, 0, 'f', 1)
                             .arg(m_TestPointer ? QStringLiteral(" (dev test pointer)") : QString())
                             .arg(m_KbOpen ? QStringLiteral("open") : QStringLiteral("closed"))
                             .arg(m_StatKeys)
                             .arg(m_Sticky, 0, 16);
    m_StatFrames = m_StatHits = m_StatMoves = m_StatButtons = m_StatScrolls = 0;
    m_StatKeys = 0;
    m_StatLatencySumMs = 0.0;
    m_StatLatencyN = 0;
    return line;
}
