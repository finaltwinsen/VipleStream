// VipleStream 2.0 §VR M4a R2 — 見 xrvrcontrollers.h
#include "xrvrcontrollers.h"

#ifdef HAVE_OPENXR

#include <Limelight.h>
#include <SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

namespace {

template <typename T>
T xrStruct(XrStructureType type)
{
    T s;
    memset(&s, 0, sizeof(s));
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

uint64_t steadyNowNs()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

constexpr float kClickOn = 0.70f;     // 類比值視為「按下」的門檻（沒有 click 元件的 profile）
constexpr float kTouchOn = 0.05f;
constexpr uint64_t kComboNs = 1000000000ull;  // menu＋trigger 1 s → SYSTEM

uint16_t unorm16(float v)
{
    v = std::min(std::max(v, 0.0f), 1.0f);
    return static_cast<uint16_t>(std::lround(v * 65535.0f));
}

int16_t snorm16(float v)
{
    v = std::min(std::max(v, -1.0f), 1.0f);
    return static_cast<int16_t>(std::lround(v * 32767.0f));
}

const char* handName(int h)
{
    return h ? "R" : "L";
}

} // namespace

XrVrControllers::XrVrControllers(bool testInput)
    : m_TestInput(testInput)
{
}

XrVrControllers::~XrVrControllers()
{
    destroy();
}

bool XrVrControllers::create(XrInstance instance, XrSession session, const QStringList& enabledExts, QString* error)
{
    m_Instance = instance;
    m_Session = session;
    m_CreatedNs = steadyNowNs();
    xrStringToPath(instance, "/user/hand/left", &m_Hands[0]);
    xrStringToPath(instance, "/user/hand/right", &m_Hands[1]);

    XrActionSetCreateInfo asci = xrStruct<XrActionSetCreateInfo>(XR_TYPE_ACTION_SET_CREATE_INFO);
    qstrncpy(asci.actionSetName, "viple_pcvr", sizeof(asci.actionSetName));
    qstrncpy(asci.localizedActionSetName, "VipleStream PCVR", sizeof(asci.localizedActionSetName));
    XrResult r = xrCreateActionSet(instance, &asci, &m_Set);
    if (XR_FAILED(r)) {
        *error = QStringLiteral("xrCreateActionSet(pcvr): %1").arg(xrStr(instance, r));
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
    if (!mk("grip_pose", "Hand pose", XR_ACTION_TYPE_POSE_INPUT, &m_GripPose) ||
        !mk("trigger_value", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT, &m_TriggerValue) ||
        !mk("trigger_click", "Trigger click", XR_ACTION_TYPE_BOOLEAN_INPUT, &m_TriggerClick) ||
        !mk("trigger_touch", "Trigger touch", XR_ACTION_TYPE_BOOLEAN_INPUT, &m_TriggerTouch) ||
        !mk("squeeze_value", "Grip", XR_ACTION_TYPE_FLOAT_INPUT, &m_SqueezeValue) ||
        !mk("squeeze_click", "Grip click", XR_ACTION_TYPE_BOOLEAN_INPUT, &m_SqueezeClick) ||
        !mk("thumbstick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT, &m_Stick) ||
        !mk("thumbstick_click", "Thumbstick click", XR_ACTION_TYPE_BOOLEAN_INPUT, &m_StickClick) ||
        !mk("thumbstick_touch", "Thumbstick touch", XR_ACTION_TYPE_BOOLEAN_INPUT, &m_StickTouch) ||
        !mk("button_a", "A / X", XR_ACTION_TYPE_BOOLEAN_INPUT, &m_BtnLo) ||
        !mk("button_a_touch", "A / X touch", XR_ACTION_TYPE_BOOLEAN_INPUT, &m_BtnLoTouch) ||
        !mk("button_b", "B / Y", XR_ACTION_TYPE_BOOLEAN_INPUT, &m_BtnHi) ||
        !mk("button_b_touch", "B / Y touch", XR_ACTION_TYPE_BOOLEAN_INPUT, &m_BtnHiTouch) ||
        !mk("menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT, &m_Menu) ||
        !mk("system", "System", XR_ACTION_TYPE_BOOLEAN_INPUT, &m_System) ||
        !mk("thumbrest_touch", "Thumbrest touch", XR_ACTION_TYPE_BOOLEAN_INPUT, &m_ThumbrestTouch) ||
        !mk("haptic", "Haptics", XR_ACTION_TYPE_VIBRATION_OUTPUT, &m_Haptic)) {
        return false;
    }

    struct B {
        XrAction action;
        const char* path;
    };
    auto suggestOnce = [&](const char* profile, const std::vector<B>& list, bool quiet) -> bool {
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
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-INPUT] bindings accepted: %s (%zu)", profile, sb.size());
        }
        else if (!quiet) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-INPUT] bindings rejected: %s: %s", profile,
                        qUtf8Printable(xrStr(instance, rr)));
        }
        return XR_SUCCEEDED(rr);
    };
    // 同一 profile 的建議整組接受或拒絕：先送完整清單，被拒再退回核心清單（每次建議取代前一次）
    auto suggest = [&](const char* profile, const std::vector<B>& full, const std::vector<B>& core) {
        if (suggestOnce(profile, full, true)) {
            return;
        }
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-INPUT] %s: full binding list rejected, retrying core set",
                    profile);
        suggestOnce(profile, core, false);
    };
    // ── khr/simple_controller：select 當 trigger，menu，grip pose，haptic ──
    {
        std::vector<B> v = {
            {m_GripPose, "/user/hand/left/input/grip/pose"},  {m_GripPose, "/user/hand/right/input/grip/pose"},
            {m_TriggerValue, "/user/hand/left/input/select/click"}, {m_TriggerValue, "/user/hand/right/input/select/click"},
            {m_TriggerClick, "/user/hand/left/input/select/click"}, {m_TriggerClick, "/user/hand/right/input/select/click"},
            {m_Menu, "/user/hand/left/input/menu/click"},     {m_Menu, "/user/hand/right/input/menu/click"},
            {m_Haptic, "/user/hand/left/output/haptic"},      {m_Haptic, "/user/hand/right/output/haptic"},
        };
        suggest("/interaction_profiles/khr/simple_controller", v, v);
    }
    // ── oculus/touch_controller：左 x/y/menu、右 a/b/system；沒有 trigger/click（由 value 門檻推）──
    {
        std::vector<B> core = {
            {m_GripPose, "/user/hand/left/input/grip/pose"},   {m_GripPose, "/user/hand/right/input/grip/pose"},
            {m_TriggerValue, "/user/hand/left/input/trigger/value"}, {m_TriggerValue, "/user/hand/right/input/trigger/value"},
            {m_SqueezeValue, "/user/hand/left/input/squeeze/value"}, {m_SqueezeValue, "/user/hand/right/input/squeeze/value"},
            {m_Stick, "/user/hand/left/input/thumbstick"},     {m_Stick, "/user/hand/right/input/thumbstick"},
            {m_StickClick, "/user/hand/left/input/thumbstick/click"}, {m_StickClick, "/user/hand/right/input/thumbstick/click"},
            {m_BtnLo, "/user/hand/left/input/x/click"},        {m_BtnLo, "/user/hand/right/input/a/click"},
            {m_BtnHi, "/user/hand/left/input/y/click"},        {m_BtnHi, "/user/hand/right/input/b/click"},
            {m_Menu, "/user/hand/left/input/menu/click"},
            {m_Haptic, "/user/hand/left/output/haptic"},       {m_Haptic, "/user/hand/right/output/haptic"},
        };
        std::vector<B> full = core;
        full.insert(full.end(), {
            {m_TriggerTouch, "/user/hand/left/input/trigger/touch"}, {m_TriggerTouch, "/user/hand/right/input/trigger/touch"},
            {m_StickTouch, "/user/hand/left/input/thumbstick/touch"}, {m_StickTouch, "/user/hand/right/input/thumbstick/touch"},
            {m_BtnLoTouch, "/user/hand/left/input/x/touch"},   {m_BtnLoTouch, "/user/hand/right/input/a/touch"},
            {m_BtnHiTouch, "/user/hand/left/input/y/touch"},   {m_BtnHiTouch, "/user/hand/right/input/b/touch"},
            {m_ThumbrestTouch, "/user/hand/left/input/thumbrest/touch"}, {m_ThumbrestTouch, "/user/hand/right/input/thumbrest/touch"},
            {m_System, "/user/hand/right/input/system/click"},
        });
        suggest("/interaction_profiles/oculus/touch_controller", full, core);
    }
    // ── valve/index_controller：兩手 a/b、system、trigger click/value/touch、squeeze value ──
    {
        std::vector<B> core = {
            {m_GripPose, "/user/hand/left/input/grip/pose"},   {m_GripPose, "/user/hand/right/input/grip/pose"},
            {m_TriggerValue, "/user/hand/left/input/trigger/value"}, {m_TriggerValue, "/user/hand/right/input/trigger/value"},
            {m_TriggerClick, "/user/hand/left/input/trigger/click"}, {m_TriggerClick, "/user/hand/right/input/trigger/click"},
            {m_SqueezeValue, "/user/hand/left/input/squeeze/value"}, {m_SqueezeValue, "/user/hand/right/input/squeeze/value"},
            {m_Stick, "/user/hand/left/input/thumbstick"},     {m_Stick, "/user/hand/right/input/thumbstick"},
            {m_StickClick, "/user/hand/left/input/thumbstick/click"}, {m_StickClick, "/user/hand/right/input/thumbstick/click"},
            {m_BtnLo, "/user/hand/left/input/a/click"},        {m_BtnLo, "/user/hand/right/input/a/click"},
            {m_BtnHi, "/user/hand/left/input/b/click"},        {m_BtnHi, "/user/hand/right/input/b/click"},
            {m_Haptic, "/user/hand/left/output/haptic"},       {m_Haptic, "/user/hand/right/output/haptic"},
        };
        std::vector<B> full = core;
        full.insert(full.end(), {
            {m_TriggerTouch, "/user/hand/left/input/trigger/touch"}, {m_TriggerTouch, "/user/hand/right/input/trigger/touch"},
            {m_StickTouch, "/user/hand/left/input/thumbstick/touch"}, {m_StickTouch, "/user/hand/right/input/thumbstick/touch"},
            {m_BtnLoTouch, "/user/hand/left/input/a/touch"},   {m_BtnLoTouch, "/user/hand/right/input/a/touch"},
            {m_BtnHiTouch, "/user/hand/left/input/b/touch"},   {m_BtnHiTouch, "/user/hand/right/input/b/touch"},
            {m_System, "/user/hand/left/input/system/click"},  {m_System, "/user/hand/right/input/system/click"},
        });
        suggest("/interaction_profiles/valve/index_controller", full, core);
    }
    // ── valve/frame_controller_valve（Valve OpenXR Unity 套件文件，見 xrinput.cpp）──
    // 右手 a/b（x/y 未用）＋menu；左手面鍵是 dpad，暫定 dpad_down→A/X、dpad_up→B/Y（左右鍵未用），
    // view→MENU——實際按鍵幾何待實機確認。兩手 trigger／squeeze 有 value 與 click，thumbstick 同 Touch。
    if (enabledExts.contains(QStringLiteral("XR_VALVE_frame_controller_interaction"))) {
        std::vector<B> core = {
            {m_GripPose, "/user/hand/left/input/grip/pose"},   {m_GripPose, "/user/hand/right/input/grip/pose"},
            {m_TriggerValue, "/user/hand/left/input/trigger/value"}, {m_TriggerValue, "/user/hand/right/input/trigger/value"},
            {m_TriggerClick, "/user/hand/left/input/trigger/click"}, {m_TriggerClick, "/user/hand/right/input/trigger/click"},
            {m_SqueezeValue, "/user/hand/left/input/squeeze/value"}, {m_SqueezeValue, "/user/hand/right/input/squeeze/value"},
            {m_SqueezeClick, "/user/hand/left/input/squeeze/click"}, {m_SqueezeClick, "/user/hand/right/input/squeeze/click"},
            {m_Stick, "/user/hand/left/input/thumbstick"},     {m_Stick, "/user/hand/right/input/thumbstick"},
            {m_StickClick, "/user/hand/left/input/thumbstick/click"}, {m_StickClick, "/user/hand/right/input/thumbstick/click"},
            {m_BtnLo, "/user/hand/left/input/dpad_down/click"}, {m_BtnLo, "/user/hand/right/input/a/click"},
            {m_BtnHi, "/user/hand/left/input/dpad_up/click"},  {m_BtnHi, "/user/hand/right/input/b/click"},
            {m_Menu, "/user/hand/left/input/view/click"},      {m_Menu, "/user/hand/right/input/menu/click"},
            {m_Haptic, "/user/hand/left/output/haptic"},       {m_Haptic, "/user/hand/right/output/haptic"},
        };
        std::vector<B> full = core;
        full.insert(full.end(), {
            {m_TriggerTouch, "/user/hand/left/input/trigger/touch"}, {m_TriggerTouch, "/user/hand/right/input/trigger/touch"},
            {m_StickTouch, "/user/hand/left/input/thumbstick/touch"}, {m_StickTouch, "/user/hand/right/input/thumbstick/touch"},
            {m_System, "/user/hand/left/input/system/click"},  {m_System, "/user/hand/right/input/system/click"},
        });
        suggest("/interaction_profiles/valve/frame_controller_valve", full, core);
    }

    XrSessionActionSetsAttachInfo ai = xrStruct<XrSessionActionSetsAttachInfo>(XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO);
    ai.countActionSets = 1;
    ai.actionSets = &m_Set;
    r = xrAttachSessionActionSets(session, &ai);
    if (XR_FAILED(r)) {
        *error = QStringLiteral("xrAttachSessionActionSets(pcvr): %1").arg(xrStr(instance, r));
        return false;
    }
    for (int h = 0; h < 2; h++) {
        XrActionSpaceCreateInfo sci = xrStruct<XrActionSpaceCreateInfo>(XR_TYPE_ACTION_SPACE_CREATE_INFO);
        sci.action = m_GripPose;
        sci.subactionPath = m_Hands[h];
        sci.poseInActionSpace.orientation.w = 1.0f;
        r = xrCreateActionSpace(session, &sci, &m_GripSpace[h]);
        if (XR_FAILED(r)) {
            m_GripSpace[h] = XR_NULL_HANDLE;
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-INPUT] grip space (%s): %s", handName(h),
                        qUtf8Printable(xrStr(instance, r)));
        }
    }
    if (m_TestInput) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-VR-INPUT] dev --vr-test-input: synthetic 8 s button sequence overrides controller buttons "
                    "(poses still come from the runtime)");
    }
    return true;
}

void XrVrControllers::destroy()
{
    for (XrSpace& s : m_GripSpace) {
        if (s != XR_NULL_HANDLE) {
            xrDestroySpace(s);
            s = XR_NULL_HANDLE;
        }
    }
    for (XrAction* a : {&m_GripPose, &m_TriggerValue, &m_TriggerClick, &m_TriggerTouch, &m_SqueezeValue,
                        &m_SqueezeClick, &m_Stick, &m_StickClick, &m_StickTouch, &m_BtnLo, &m_BtnLoTouch,
                        &m_BtnHi, &m_BtnHiTouch, &m_Menu, &m_System, &m_ThumbrestTouch, &m_Haptic}) {
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

bool XrVrControllers::boolState(XrAction a, int h) const
{
    XrActionStateGetInfo gi = xrStruct<XrActionStateGetInfo>(XR_TYPE_ACTION_STATE_GET_INFO);
    gi.action = a;
    gi.subactionPath = m_Hands[h];
    XrActionStateBoolean st = xrStruct<XrActionStateBoolean>(XR_TYPE_ACTION_STATE_BOOLEAN);
    return XR_SUCCEEDED(xrGetActionStateBoolean(m_Session, &gi, &st)) && st.isActive && st.currentState;
}

float XrVrControllers::floatState(XrAction a, int h) const
{
    XrActionStateGetInfo gi = xrStruct<XrActionStateGetInfo>(XR_TYPE_ACTION_STATE_GET_INFO);
    gi.action = a;
    gi.subactionPath = m_Hands[h];
    XrActionStateFloat st = xrStruct<XrActionStateFloat>(XR_TYPE_ACTION_STATE_FLOAT);
    if (XR_SUCCEEDED(xrGetActionStateFloat(m_Session, &gi, &st)) && st.isActive) {
        return st.currentState;
    }
    return 0.0f;
}

XrVector2f XrVrControllers::vecState(XrAction a, int h) const
{
    XrActionStateGetInfo gi = xrStruct<XrActionStateGetInfo>(XR_TYPE_ACTION_STATE_GET_INFO);
    gi.action = a;
    gi.subactionPath = m_Hands[h];
    XrActionStateVector2f st = xrStruct<XrActionStateVector2f>(XR_TYPE_ACTION_STATE_VECTOR2F);
    if (XR_SUCCEEDED(xrGetActionStateVector2f(m_Session, &gi, &st)) && st.isActive) {
        return st.currentState;
    }
    return XrVector2f {0.0f, 0.0f};
}

bool XrVrControllers::poseIsActive(int h) const
{
    XrActionStateGetInfo gi = xrStruct<XrActionStateGetInfo>(XR_TYPE_ACTION_STATE_GET_INFO);
    gi.action = m_GripPose;
    gi.subactionPath = m_Hands[h];
    XrActionStatePose st = xrStruct<XrActionStatePose>(XR_TYPE_ACTION_STATE_POSE);
    return XR_SUCCEEDED(xrGetActionStatePose(m_Session, &gi, &st)) && st.isActive;
}

bool XrVrControllers::locateGrip(int h, XrSpace trackSpace, XrTime t, Hand* out) const
{
    if (m_GripSpace[h] == XR_NULL_HANDLE || trackSpace == XR_NULL_HANDLE || t == 0) {
        return false;
    }
    XrSpaceVelocity vel = xrStruct<XrSpaceVelocity>(XR_TYPE_SPACE_VELOCITY);
    XrSpaceLocation loc = xrStruct<XrSpaceLocation>(XR_TYPE_SPACE_LOCATION);
    loc.next = &vel;
    if (XR_FAILED(xrLocateSpace(m_GripSpace[h], trackSpace, t, &loc))) {
        return false;
    }
    const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    if ((loc.locationFlags & need) != need) {
        return false;
    }
    out->pos[0] = loc.pose.position.x;
    out->pos[1] = loc.pose.position.y;
    out->pos[2] = loc.pose.position.z;
    out->rot[0] = loc.pose.orientation.x;
    out->rot[1] = loc.pose.orientation.y;
    out->rot[2] = loc.pose.orientation.z;
    out->rot[3] = loc.pose.orientation.w;
    const bool lv = (vel.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0;
    const bool av = (vel.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) != 0;
    out->linVel[0] = lv ? vel.linearVelocity.x : 0.0f;
    out->linVel[1] = lv ? vel.linearVelocity.y : 0.0f;
    out->linVel[2] = lv ? vel.linearVelocity.z : 0.0f;
    out->angVel[0] = av ? vel.angularVelocity.x : 0.0f;
    out->angVel[1] = av ? vel.angularVelocity.y : 0.0f;
    out->angVel[2] = av ? vel.angularVelocity.z : 0.0f;
    out->linValid = lv;
    out->angValid = av;
    out->poseValid = true;
    return true;
}

// dev：8 s 一輪的合成按鍵（右手 A、B、trigger 漸進、搖桿右推；左手 grip、menu＋trigger 1.4 s 觸發 SYSTEM）
void XrVrControllers::applyTestInput(int h, uint32_t* buttons, uint32_t* touches, float* trigger, float* grip,
                                     float* sx, float* sy, uint64_t nowNs)
{
    const double t = std::fmod(static_cast<double>(nowNs - m_CreatedNs) / 1e9, 8.0);
    *buttons = 0;
    *touches = 0;
    *trigger = 0.0f;
    *grip = 0.0f;
    *sx = 0.0f;
    *sy = 0.0f;
    if (h == 1) {
        if (t >= 0.0 && t < 0.3) {
            *buttons |= VIPLE_VR_BTN_A;
        }
        if (t >= 1.0 && t < 1.3) {
            *buttons |= VIPLE_VR_BTN_B;
        }
        if (t >= 2.0 && t < 3.0) {
            *trigger = static_cast<float>(t - 2.0);  // 0→1 漸進，過 0.7 視為按下
        }
        if (t >= 6.0 && t < 6.5) {
            *sx = 1.0f;
        }
    }
    else {
        if (t >= 7.0 && t < 7.2) {
            *grip = 1.0f;
        }
        if (t >= 4.0 && t < 5.4) {
            *buttons |= VIPLE_VR_BTN_MENU;
            *trigger = 1.0f;
        }
    }
    *touches = *buttons;
}

void XrVrControllers::updateFrame(XrTime predictedDisplayTime, bool focused, XrSpace trackSpace)
{
    const uint64_t nowNs = steadyNowNs();
    Hand next[2];
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        next[0] = m_Hand[0];
        next[1] = m_Hand[1];
        if (m_Focused && !focused) {
            m_StatFocusLoss++;
        }
        m_Focused = focused;
    }

    for (int h = 0; h < 2; h++) {
        Hand& hd = next[h];
        hd.poseActive = poseIsActive(h);
        Hand located = hd;
        located.poseValid = false;
        if (locateGrip(h, trackSpace, predictedDisplayTime, &located)) {
            hd.poseValid = true;
            memcpy(hd.pos, located.pos, sizeof(hd.pos));
            memcpy(hd.rot, located.rot, sizeof(hd.rot));
            memcpy(hd.linVel, located.linVel, sizeof(hd.linVel));
            memcpy(hd.angVel, located.angVel, sizeof(hd.angVel));
        }
        else {
            hd.poseValid = false;
        }

        uint32_t buttons = 0, touches = 0;
        float trigger = 0.0f, grip = 0.0f, sx = 0.0f, sy = 0.0f;
        if (m_TestInput) {
            applyTestInput(h, &buttons, &touches, &trigger, &grip, &sx, &sy, nowNs);
        }
        else if (focused) {
            trigger = floatState(m_TriggerValue, h);
            grip = floatState(m_SqueezeValue, h);
            const XrVector2f st = vecState(m_Stick, h);
            sx = st.x;
            sy = st.y;
            if (boolState(m_System, h)) buttons |= VIPLE_VR_BTN_SYSTEM;
            if (boolState(m_Menu, h)) buttons |= VIPLE_VR_BTN_MENU;
            if (boolState(m_BtnLo, h)) buttons |= VIPLE_VR_BTN_A;
            if (boolState(m_BtnHi, h)) buttons |= VIPLE_VR_BTN_B;
            if (boolState(m_StickClick, h)) buttons |= VIPLE_VR_BTN_THUMBSTICK;
            if (boolState(m_TriggerClick, h)) buttons |= VIPLE_VR_BTN_TRIGGER;
            if (boolState(m_SqueezeClick, h)) buttons |= VIPLE_VR_BTN_GRIP;
            if (boolState(m_BtnLoTouch, h)) touches |= VIPLE_VR_BTN_A;
            if (boolState(m_BtnHiTouch, h)) touches |= VIPLE_VR_BTN_B;
            if (boolState(m_StickTouch, h)) touches |= VIPLE_VR_BTN_THUMBSTICK;
            if (boolState(m_TriggerTouch, h)) touches |= VIPLE_VR_BTN_TRIGGER;
            if (boolState(m_ThumbrestTouch, h)) touches |= VIPLE_VR_BTN_THUMBREST;
        }
        // 類比值推導的 click／touch（沒有 click 元件的 profile，例如 Touch 的 trigger、squeeze）
        if (trigger >= kClickOn) buttons |= VIPLE_VR_BTN_TRIGGER;
        if (grip >= kClickOn) buttons |= VIPLE_VR_BTN_GRIP;
        if (trigger > kTouchOn) touches |= VIPLE_VR_BTN_TRIGGER;
        if (grip > kTouchOn) touches |= VIPLE_VR_BTN_GRIP;
        touches |= buttons & (VIPLE_VR_BTN_A | VIPLE_VR_BTN_B | VIPLE_VR_BTN_THUMBSTICK | VIPLE_VR_BTN_TRIGGER);

        if (!focused && !m_TestInput) {
            // 沒有輸入焦點：全部放開（pressCtr 不動）
            buttons = touches = 0;
            trigger = grip = sx = sy = 0.0f;
        }

        // menu＋trigger 按住 1 s → SYSTEM（期間遮掉 MENU 與 TRIGGER）
        const bool comboHeld = (buttons & VIPLE_VR_BTN_MENU) && (buttons & VIPLE_VR_BTN_TRIGGER);
        if (!comboHeld) {
            hd.comboStartNs = 0;
            hd.comboActive = false;
        }
        else {
            if (hd.comboStartNs == 0) {
                hd.comboStartNs = nowNs;
            }
            if (!hd.comboActive && nowNs - hd.comboStartNs >= kComboNs) {
                hd.comboActive = true;
                std::lock_guard<std::mutex> lk(m_Mutex);
                m_StatCombos++;
            }
        }
        if (hd.comboActive) {
            buttons = (buttons & ~(VIPLE_VR_BTN_MENU | VIPLE_VR_BTN_TRIGGER)) | VIPLE_VR_BTN_SYSTEM;
            touches &= ~VIPLE_VR_BTN_TRIGGER;
            trigger = 0.0f;
        }

        // pressCtr：每個按下邊緣把該鍵的 2 bit 計數 +1（mod 4）
        const uint32_t rising = buttons & ~hd.buttons & 0xFFFFu;
        if (rising != 0) {
            for (int i = 0; i < 16; i++) {
                if (rising & (1u << i)) {
                    const uint32_t shift = 2u * static_cast<uint32_t>(i);
                    const uint32_t ctr = ((hd.pressCtr >> shift) + 1u) & 3u;
                    hd.pressCtr = (hd.pressCtr & ~(3u << shift)) | (ctr << shift);
                    hd.edges++;
                }
            }
        }
        hd.buttons = buttons;
        hd.touches = touches;
        hd.trigger = unorm16(trigger);
        hd.grip = unorm16(grip);
        hd.stickX = snorm16(sx);
        hd.stickY = snorm16(sy);
    }

    // edges 只在 frame thread 累加（takeStatsLine 也在 frame thread 歸零），直接以 next 覆蓋
    std::lock_guard<std::mutex> lk(m_Mutex);
    m_Hand[0] = next[0];
    m_Hand[1] = next[1];
}

void XrVrControllers::fill(_VIPLE_VR_TRACKING* sample, XrSpace trackSpace, XrTime xrTime)
{
    Hand snap[2];
    bool focused;
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        snap[0] = m_Hand[0];
        snap[1] = m_Hand[1];
        focused = m_Focused;
    }
    for (int h = 0; h < 2; h++) {
        Hand& hd = snap[h];
        if (xrTime != 0) {
            // thread 模式：當下 locate（2×Hz 都是新樣本）；失敗就沿用 frame loop 的 pose
            Hand located = hd;
            if (locateGrip(h, trackSpace, xrTime, &located)) {
                hd = located;
            }
        }
        const bool active = hd.poseActive && hd.poseValid;
        VIPLE_VR_POSE& p = sample->pose[h == 0 ? VIPLE_VR_POSE_LEFT : VIPLE_VR_POSE_RIGHT];
        VIPLE_VR_CONTROLLER_INPUT& in = sample->input[h];
        if (active) {
            memcpy(p.pos, hd.pos, sizeof(p.pos));
            memcpy(p.rot, hd.rot, sizeof(p.rot));
            memcpy(p.linVel, hd.linVel, sizeof(p.linVel));
            memcpy(p.angVel, hd.angVel, sizeof(p.angVel));
            sample->flags |= (h == 0 ? VIPLE_VR_TRK_LEFT : VIPLE_VR_TRK_RIGHT);
        }
        in.buttons = hd.buttons;
        in.touches = hd.touches;
        in.pressCtr = hd.pressCtr;
        in.trigger = hd.trigger;
        in.grip = hd.grip;
        in.stickX = hd.stickX;
        in.stickY = hd.stickY;
        in.battery = 255;  // runtime 沒有標準的電量 API
        in.flags = static_cast<uint8_t>((active ? VIPLE_VR_CTRL_ACTIVE : 0) |
                                        ((focused || m_TestInput) ? VIPLE_VR_CTRL_FOCUSED : 0));
        in.profile = m_Profile[h].load(std::memory_order_relaxed);  // M4a 收尾：server 選控制器外觀
        in.reserved = 0;
        if (active) {
            // 2026-10-05：速度有效率與最大值（拍子外插只靠這兩個速度；無效時送 0＝SteamVR 不外插）
            const float lin = std::sqrt(hd.linVel[0] * hd.linVel[0] + hd.linVel[1] * hd.linVel[1] + hd.linVel[2] * hd.linVel[2]);
            const float ang = std::sqrt(hd.angVel[0] * hd.angVel[0] + hd.angVel[1] * hd.angVel[1] + hd.angVel[2] * hd.angVel[2]);
            std::lock_guard<std::mutex> lk(m_Mutex);
            VelStat& vs = m_VelStat[h];
            vs.samples++;
            vs.linValid += hd.linValid ? 1 : 0;
            vs.angValid += hd.angValid ? 1 : 0;
            vs.linMax = (std::max)(vs.linMax, lin);
            vs.angMax = (std::max)(vs.angMax, ang);
        }
    }
}

void XrVrControllers::queueHaptic(uint8_t device, uint32_t durationUs, float frequencyHz, float amplitude)
{
    std::lock_guard<std::mutex> lk(m_HapticMutex);
    if (m_Haptics.size() >= 64) {
        m_Haptics.pop_front();
        m_StatHapticDropped++;
    }
    m_Haptics.push_back({device, durationUs, frequencyHz, amplitude});
}

void XrVrControllers::applyPendingHaptics()
{
    std::deque<HapticReq> todo;
    {
        std::lock_guard<std::mutex> lk(m_HapticMutex);
        todo.swap(m_Haptics);
    }
    for (const HapticReq& r : todo) {
        const int h = r.device == VIPLE_VR_POSE_RIGHT ? 1 : (r.device == VIPLE_VR_POSE_LEFT ? 0 : -1);
        if (h < 0 || m_Haptic == XR_NULL_HANDLE) {
            m_StatHapticFailed++;
            continue;
        }
        XrHapticActionInfo hai = xrStruct<XrHapticActionInfo>(XR_TYPE_HAPTIC_ACTION_INFO);
        hai.action = m_Haptic;
        hai.subactionPath = m_Hands[h];
        XrHapticVibration vib = xrStruct<XrHapticVibration>(XR_TYPE_HAPTIC_VIBRATION);
        vib.duration = r.durationUs == 0 ? XR_MIN_HAPTIC_DURATION : static_cast<XrDuration>(r.durationUs) * 1000;
        vib.frequency = (std::isfinite(r.frequencyHz) && r.frequencyHz > 0.0f) ? r.frequencyHz : XR_FREQUENCY_UNSPECIFIED;
        vib.amplitude = std::isfinite(r.amplitude) ? std::min(std::max(r.amplitude, 0.0f), 1.0f) : 0.0f;
        const XrResult rr = xrApplyHapticFeedback(m_Session, &hai,
                                                  reinterpret_cast<const XrHapticBaseHeader*>(&vib));
        if (XR_SUCCEEDED(rr)) {
            m_StatHapticApplied++;
        }
        else {
            m_StatHapticFailed++;
            if (m_StatHapticFailed <= 3) {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-HAPTIC] xrApplyHapticFeedback(%s): %s",
                            handName(h), qUtf8Printable(xrStr(m_Instance, rr)));
            }
        }
    }
}

void XrVrControllers::onProfileChanged()
{
    for (int h = 0; h < 2; h++) {
        XrInteractionProfileState st = xrStruct<XrInteractionProfileState>(XR_TYPE_INTERACTION_PROFILE_STATE);
        if (XR_FAILED(xrGetCurrentInteractionProfile(m_Session, m_Hands[h], &st))) {
            continue;
        }
        char buf[XR_MAX_PATH_LENGTH] = "(none)";
        uint32_t n = 0;
        uint8_t profile = VIPLE_VR_CTRL_PROFILE_UNKNOWN;
        if (st.interactionProfile != XR_NULL_PATH &&
            XR_SUCCEEDED(xrPathToString(m_Instance, st.interactionProfile, sizeof(buf), &n, buf))) {
            const QByteArray p(buf);
            profile = p.contains("/touch_controller") ? VIPLE_VR_CTRL_PROFILE_TOUCH
                    : p.contains("/index_controller") ? VIPLE_VR_CTRL_PROFILE_INDEX
                    : p.contains("/frame_controller") ? VIPLE_VR_CTRL_PROFILE_FRAME
                                                       : VIPLE_VR_CTRL_PROFILE_OTHER;
        }
        m_Profile[h].store(profile, std::memory_order_relaxed);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-INPUT] interaction profile %s: %s (wire profile %u)",
                    handName(h), buf, static_cast<unsigned>(profile));
    }
}

QString XrVrControllers::takeStatsLine()
{
    Hand snap[2];
    bool focused;
    uint32_t combos, focusLoss;
    VelStat vel[2];
    {
        std::lock_guard<std::mutex> lk(m_Mutex);
        snap[0] = m_Hand[0];
        snap[1] = m_Hand[1];
        focused = m_Focused;
        combos = m_StatCombos;
        focusLoss = m_StatFocusLoss;
        m_StatCombos = 0;
        m_StatFocusLoss = 0;
        m_Hand[0].edges = 0;
        m_Hand[1].edges = 0;
        vel[0] = m_VelStat[0];
        vel[1] = m_VelStat[1];
        m_VelStat[0] = VelStat {};
        m_VelStat[1] = VelStat {};
    }
    const uint32_t applied = m_StatHapticApplied, failed = m_StatHapticFailed, dropped = m_StatHapticDropped;
    m_StatHapticApplied = m_StatHapticFailed = m_StatHapticDropped = 0;
    // 2026-10-05：尾端加速度有效率（%）與最大值，前段欄位順序不變
    auto velText = [](const VelStat& v) {
        auto pct = [&v](uint32_t k) { return v.samples ? 100.0 * k / v.samples : 0.0; };
        return QStringLiteral("n=%1 linValid=%2% angValid=%3% |v|max=%4 m/s |w|max=%5 rad/s")
            .arg(v.samples)
            .arg(pct(v.linValid), 0, 'f', 1)
            .arg(pct(v.angValid), 0, 'f', 1)
            .arg(static_cast<double>(v.linMax), 0, 'f', 2)
            .arg(static_cast<double>(v.angMax), 0, 'f', 2);
    };
    const QString velLine = QStringLiteral(" | vel L %1 | vel R %2").arg(velText(vel[0]), velText(vel[1]));
    return QStringLiteral("L active=%1 edges=%2 pressCtr=0x%3 | R active=%4 edges=%5 pressCtr=0x%6 | focused=%7 "
                          "systemCombos=%8 focusLoss=%9 | haptic applied=%10 failed=%11 dropped=%12%13%14")
        .arg(snap[0].poseActive && snap[0].poseValid ? 1 : 0)
        .arg(snap[0].edges)
        .arg(snap[0].pressCtr, 8, 16, QLatin1Char('0'))
        .arg(snap[1].poseActive && snap[1].poseValid ? 1 : 0)
        .arg(snap[1].edges)
        .arg(snap[1].pressCtr, 8, 16, QLatin1Char('0'))
        .arg(focused ? 1 : 0)
        .arg(combos)
        .arg(focusLoss)
        .arg(applied)
        .arg(failed)
        .arg(dropped)
        .arg(m_TestInput ? QStringLiteral(" [dev test-input]") : QString())
        .arg(velLine);
}

#endif // HAVE_OPENXR
