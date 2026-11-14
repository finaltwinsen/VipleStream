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
constexpr uint64_t kWheelComboNs = 1500000000ull;  // §VR-WHEEL：兩支搖桿同時按住 1.5 s → 切換方向盤模式
constexpr uint64_t kKbComboNs = 1000000000ull;     // §VR-KBD：同一手 menu＋grip 按住 1 s → 開關虛擬鍵盤

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

XrVrControllers::XrVrControllers(bool testInput, std::function<void()> exitCombo)
    : m_TestInput(testInput), m_ExitCombo(std::move(exitCombo))
{
}

XrVrControllers::~XrVrControllers()
{
    destroy();
}

void XrVrControllers::setWheel(uint8_t wire, std::function<void(bool on)> toggled)
{
    m_Wheel.store(wire, std::memory_order_relaxed);
    m_WheelToggled = std::move(toggled);
}

void XrVrControllers::setKeyboard(std::function<void(int vk, bool down, uint8_t mods)> key, const QString& testText)
{
    m_KbKey = std::move(key);
    m_KbTestText = testText;
    m_KbTestKeys.clear();
    for (const QChar c : testText) {
        int idx = -1;
        bool needShift = false;
        if (m_Kb.keyForChar(c, &idx, &needShift) && !needShift) {
            m_KbTestKeys.push_back(idx);
        }
    }
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
        !mk("button_c", "Steam Frame X / D-pad left", XR_ACTION_TYPE_BOOLEAN_INPUT, &m_BtnC) ||
        !mk("button_d", "Steam Frame Y / D-pad up", XR_ACTION_TYPE_BOOLEAN_INPUT, &m_BtnD) ||
        !mk("bumper", "Steam Frame bumper", XR_ACTION_TYPE_BOOLEAN_INPUT, &m_Bumper) ||
        !mk("menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT, &m_Menu) ||
        !mk("system", "System", XR_ACTION_TYPE_BOOLEAN_INPUT, &m_System) ||
        !mk("thumbrest_touch", "Thumbrest touch", XR_ACTION_TYPE_BOOLEAN_INPUT, &m_ThumbrestTouch) ||
        !mk("haptic", "Haptics", XR_ACTION_TYPE_VIBRATION_OUTPUT, &m_Haptic) ||
        !mk("combo_x", "X (with A, trigger and grip: end the stream)", XR_ACTION_TYPE_BOOLEAN_INPUT, &m_ComboX) ||
        !mk("aim_pose", "Pointer (virtual keyboard)", XR_ACTION_TYPE_POSE_INPUT, &m_AimPose)) {
        return false;
    }

    if (enabledExts.contains(QStringLiteral("XR_EXT_eye_gaze_interaction"))) {
        XrActionCreateInfo aci = xrStruct<XrActionCreateInfo>(XR_TYPE_ACTION_CREATE_INFO);
        qstrncpy(aci.actionName, "eye_gaze_pose", sizeof(aci.actionName));
        qstrncpy(aci.localizedActionName, "Eye Gaze Pose", sizeof(aci.localizedActionName));
        aci.actionType = XR_ACTION_TYPE_POSE_INPUT;
        aci.countSubactionPaths = 0;
        aci.subactionPaths = nullptr;
        const XrResult rr = xrCreateAction(m_Set, &aci, &m_EyeGazeAction);
        if (XR_FAILED(rr)) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-GAZE] xrCreateAction(eye_gaze): %s",
                        qUtf8Printable(xrStr(instance, rr)));
            m_EyeGazeAction = XR_NULL_HANDLE;
        }
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
    // §VR-KBD：aim pose 只放在完整清單（被拒就退回核心清單＝沒有鍵盤的射線，其他按鍵不受影響）
    const std::vector<B> aim = {
        {m_AimPose, "/user/hand/left/input/aim/pose"}, {m_AimPose, "/user/hand/right/input/aim/pose"},
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
        std::vector<B> full = v;
        full.insert(full.end(), aim.begin(), aim.end());
        suggest("/interaction_profiles/khr/simple_controller", full, v);
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
            {m_ComboX, "/user/hand/left/input/x/click"},
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
        full.insert(full.end(), aim.begin(), aim.end());
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
        full.insert(full.end(), aim.begin(), aim.end());
        suggest("/interaction_profiles/valve/index_controller", full, core);
    }
    // ── valve/frame_controller_valve（Valve OpenXR Unity 套件文件，見 xrinput.cpp）──
    // §VR-FRAME-CTRL：四個面鍵照 SteamVR frame_controller_profile 的鏡像對應送 A／B／C／D——右 a/b/x/y、
    // 左 dpad_down/right/left/up；shoulder→BUMPER；menu（右）／view（左）→MENU。server 的 driver 以 Frame 手把
    // 身分出現，沒有 Frame 對應檔的遊戲由 SteamVR 自己的相容層（模擬 Touch）處理，與 Steam 內建串流相同。
    // 兩手 trigger／squeeze 有 value 與 click，thumbstick 同 Touch。
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
            {m_BtnHi, "/user/hand/left/input/dpad_right/click"}, {m_BtnHi, "/user/hand/right/input/b/click"},
            {m_Menu, "/user/hand/left/input/view/click"},      {m_Menu, "/user/hand/right/input/menu/click"},
            {m_ComboX, "/user/hand/right/input/x/click"},
            {m_Haptic, "/user/hand/left/output/haptic"},       {m_Haptic, "/user/hand/right/output/haptic"},
        };
        std::vector<B> full = core;
        full.insert(full.end(), {
            {m_TriggerTouch, "/user/hand/left/input/trigger/touch"}, {m_TriggerTouch, "/user/hand/right/input/trigger/touch"},
            {m_StickTouch, "/user/hand/left/input/thumbstick/touch"}, {m_StickTouch, "/user/hand/right/input/thumbstick/touch"},
            {m_System, "/user/hand/left/input/system/click"},  {m_System, "/user/hand/right/input/system/click"},
        });
        // C／D／bumper 的路徑還沒在實機確認過：多一層，被拒時退回原本的核心清單（不影響既有按鍵）
        const std::vector<B> extra = {
            {m_BtnC, "/user/hand/left/input/dpad_left/click"}, {m_BtnC, "/user/hand/right/input/x/click"},
            {m_BtnD, "/user/hand/left/input/dpad_up/click"},   {m_BtnD, "/user/hand/right/input/y/click"},
            {m_Bumper, "/user/hand/left/input/shoulder/click"}, {m_Bumper, "/user/hand/right/input/shoulder/click"},
        };
        full.insert(full.end(), extra.begin(), extra.end());
        std::vector<B> coreExtra = core;
        coreExtra.insert(coreExtra.end(), extra.begin(), extra.end());
        // §VR-KBD：再多一層——含 aim pose 的清單被拒時，退回這次改動之前的那一組（full）
        std::vector<B> fullAim = full;
        fullAim.insert(fullAim.end(), aim.begin(), aim.end());
        if (!suggestOnce("/interaction_profiles/valve/frame_controller_valve", fullAim, true) &&
            !suggestOnce("/interaction_profiles/valve/frame_controller_valve", full, true)) {
            suggest("/interaction_profiles/valve/frame_controller_valve", coreExtra, core);
        }
    }

    if (m_EyeGazeAction != XR_NULL_HANDLE) {
        XrPath gazeProf = XR_NULL_PATH;
        XrPath gazePath = XR_NULL_PATH;
        if (XR_SUCCEEDED(xrStringToPath(instance, "/interaction_profiles/ext/eye_gaze_interaction", &gazeProf)) &&
            XR_SUCCEEDED(xrStringToPath(instance, "/user/eyes_ext/input/gaze_ext/pose", &gazePath))) {
            XrActionSuggestedBinding sb = {m_EyeGazeAction, gazePath};
            XrInteractionProfileSuggestedBinding isb =
                xrStruct<XrInteractionProfileSuggestedBinding>(XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING);
            isb.interactionProfile = gazeProf;
            isb.countSuggestedBindings = 1;
            isb.suggestedBindings = &sb;
            const XrResult rr = xrSuggestInteractionProfileBindings(instance, &isb);
            if (XR_SUCCEEDED(rr)) {
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-GAZE] eye gaze bindings accepted: /interaction_profiles/ext/eye_gaze_interaction");
            } else {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-GAZE] eye gaze bindings rejected: %s",
                            qUtf8Printable(xrStr(instance, rr)));
            }
        }
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
    for (int h = 0; h < 2; h++) {
        XrActionSpaceCreateInfo sci = xrStruct<XrActionSpaceCreateInfo>(XR_TYPE_ACTION_SPACE_CREATE_INFO);
        sci.action = m_AimPose;
        sci.subactionPath = m_Hands[h];
        sci.poseInActionSpace.orientation.w = 1.0f;
        r = xrCreateActionSpace(session, &sci, &m_AimSpace[h]);
        if (XR_FAILED(r)) {
            m_AimSpace[h] = XR_NULL_HANDLE;
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-KBD] aim space (%s): %s", handName(h),
                        qUtf8Printable(xrStr(instance, r)));
        }
    }
    if (m_EyeGazeAction != XR_NULL_HANDLE) {
        XrActionSpaceCreateInfo sci = xrStruct<XrActionSpaceCreateInfo>(XR_TYPE_ACTION_SPACE_CREATE_INFO);
        sci.action = m_EyeGazeAction;
        sci.subactionPath = XR_NULL_PATH;
        sci.poseInActionSpace.orientation.w = 1.0f;
        r = xrCreateActionSpace(session, &sci, &m_EyeGazeSpace);
        if (XR_FAILED(r)) {
            m_EyeGazeSpace = XR_NULL_HANDLE;
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-GAZE] eye gaze action space failed: %s",
                        qUtf8Printable(xrStr(instance, r)));
        } else {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-GAZE] eye gaze action space created");
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
    if (m_EyeGazeSpace != XR_NULL_HANDLE) {
        xrDestroySpace(m_EyeGazeSpace);
        m_EyeGazeSpace = XR_NULL_HANDLE;
    }
    if (m_EyeGazeAction != XR_NULL_HANDLE) {
        xrDestroyAction(m_EyeGazeAction);
        m_EyeGazeAction = XR_NULL_HANDLE;
    }
    for (XrSpace& s : m_GripSpace) {
        if (s != XR_NULL_HANDLE) {
            xrDestroySpace(s);
            s = XR_NULL_HANDLE;
        }
    }
    for (XrSpace& s : m_AimSpace) {
        if (s != XR_NULL_HANDLE) {
            xrDestroySpace(s);
            s = XR_NULL_HANDLE;
        }
    }
    for (XrAction* a : {&m_GripPose, &m_TriggerValue, &m_TriggerClick, &m_TriggerTouch, &m_SqueezeValue,
                        &m_SqueezeClick, &m_Stick, &m_StickClick, &m_StickTouch, &m_BtnLo, &m_BtnLoTouch,
                        &m_BtnHi, &m_BtnHiTouch, &m_BtnC, &m_BtnD, &m_Bumper, &m_Menu, &m_System, &m_ThumbrestTouch, &m_Haptic, &m_ComboX, &m_AimPose}) {
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

// §PERF-1113：握把姿態這一半（原本在 updateFrame 的每手迴圈開頭）。留在 xrWaitFrame 之後，用這一格的預測顯示時間。
// 只寫 Hand 的姿態欄位；按鍵欄位由 updateButtons 寫（兩者都在 frame thread，不會交錯）
void XrVrControllers::updateGripPose(XrTime predictedDisplayTime, XrSpace trackSpace)
{
    bool active[2] = {false, false};
    bool ok[2] = {false, false};
    Hand located[2];
    for (int h = 0; h < 2; h++) {
        active[h] = poseIsActive(h);
        ok[h] = locateGrip(h, trackSpace, predictedDisplayTime, &located[h]);
    }
    std::lock_guard<std::mutex> lk(m_Mutex);
    for (int h = 0; h < 2; h++) {
        Hand& hd = m_Hand[h];
        hd.poseActive = active[h];
        if (ok[h]) {
            hd.poseValid = true;
            memcpy(hd.pos, located[h].pos, sizeof(hd.pos));
            memcpy(hd.rot, located[h].rot, sizeof(hd.rot));
            memcpy(hd.linVel, located[h].linVel, sizeof(hd.linVel));
            memcpy(hd.angVel, located[h].angVel, sizeof(hd.angVel));
            // §REVIEW-1104：frameloop 不重 locate，速度有效旗標沒抄的話統計永遠是 0%
            hd.linValid = located[h].linValid;
            hd.angValid = located[h].angValid;
        }
        else {
            hd.poseValid = false;
            // §REVIEW-1104：locate 失敗時清掉上一格留下的速度有效旗標
            hd.linValid = false;
            hd.angValid = false;
        }
    }
}

// §PERF-1113：按鍵這一半。緊接在 xrSyncActions 後面呼叫（action state 只在 sync 那一刻更新，晚讀只是晚發布）
void XrVrControllers::updateButtons(bool focused, uint64_t syncDoneNs)
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

    // §VR-EXIT-COMBO（2026-10-22 頭盔實測：VR 串流裡按了沒反應）：PCVR 不建 XrInput，組合鍵原本只在 XR 桌面有效。
    // 右手 A＋X（Touch 左手、Frame 右手）＋任一手扳機＋任一手握把整組按住的上升緣；這幾鍵照樣轉送給主機
    if (m_ExitCombo && focused && !m_TestInput) {
        const bool combo = boolState(m_BtnLo, 1) && (boolState(m_ComboX, 0) || boolState(m_ComboX, 1)) &&
                           std::max(floatState(m_TriggerValue, 0), floatState(m_TriggerValue, 1)) >= kClickOn &&
                           std::max(floatState(m_SqueezeValue, 0), floatState(m_SqueezeValue, 1)) >= kClickOn;
        if (combo && !m_ExitComboWas) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-INPUT] exit combo (A+X+trigger+grip)");
            m_ExitCombo();
        }
        m_ExitComboWas = combo;
    }
    else {
        m_ExitComboWas = false;
    }

    // §VR-KBD：開關（同一手 menu＋grip 按住 1 s；扳機也按著時不算，那是別的組合）與這一格真實的扳機
    for (int n = m_KbToggleReq.exchange(0, std::memory_order_acq_rel); n > 0; n--) {
        setKeyboardOpen(!m_KbOpen, "request");
    }
    bool kbAnyHeld = false;
    for (int h = 0; h < 2; h++) {
        const bool live = focused && !m_TestInput && m_KbKey;
        const bool trig = live && (floatState(m_TriggerValue, h) >= kClickOn || boolState(m_TriggerClick, h));
        const bool grip = live && (floatState(m_SqueezeValue, h) >= kClickOn || boolState(m_SqueezeClick, h));
        const bool menu = live && boolState(m_Menu, h);
        m_KbRawTrigger[h] = trig;
        kbAnyHeld = kbAnyHeld || trig || grip || menu;
        if (!(menu && grip && !trig)) {
            m_KbComboStartNs[h] = 0;
            m_KbComboFired[h] = false;
        }
        else {
            if (m_KbComboStartNs[h] == 0) {
                m_KbComboStartNs[h] = nowNs;
            }
            if (!m_KbComboFired[h] && nowNs - m_KbComboStartNs[h] >= kKbComboNs) {
                m_KbComboFired[h] = true;
                setKeyboardOpen(!m_KbOpen, "menu+grip");
            }
        }
    }
    if (m_KbMaskUntilRelease && !kbAnyHeld) {
        m_KbMaskUntilRelease = false;
    }
    if (m_KbOpen && !focused && m_KbTestState != 1) {
        kbRelease();  // 失去焦點：按著的鍵放開（鍵盤留著）。dev 測試不看焦點（無人場次多半沒有焦點）
    }
    const bool kbMask = m_KbOpen || m_KbMaskUntilRelease;

    for (int h = 0; h < 2; h++) {
        Hand& hd = next[h];  // 姿態欄位是從 m_Hand 抄來的原值，這裡不動（updateGripPose 負責）

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
            if (boolState(m_BtnC, h)) buttons |= VIPLE_VR_BTN_C;
            if (boolState(m_BtnD, h)) buttons |= VIPLE_VR_BTN_D;
            if (boolState(m_Bumper, h)) buttons |= VIPLE_VR_BTN_BUMPER;
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
        touches |= buttons & (VIPLE_VR_BTN_A | VIPLE_VR_BTN_B | VIPLE_VR_BTN_C | VIPLE_VR_BTN_D | VIPLE_VR_BTN_BUMPER |
                              VIPLE_VR_BTN_THUMBSTICK | VIPLE_VR_BTN_TRIGGER);

        if ((!focused && !m_TestInput) || kbMask) {
            // 沒有輸入焦點、或 §VR-KBD 鍵盤開著：全部放開（pressCtr 不動）
            buttons = touches = 0;
            trigger = grip = sx = sy = 0.0f;
        }

        // menu＋trigger 按住 1 s → SYSTEM（期間遮掉 MENU 與 TRIGGER）
        // §REVIEW-1111（第二輪）：規則搬到 xrcombo.h（組合結束後還按著的那一鍵繼續遮到放開，不再變成一次新的按下）
        if (xrcombo::apply(hd.combo, VIPLE_VR_BTN_MENU, VIPLE_VR_BTN_TRIGGER, VIPLE_VR_BTN_SYSTEM, kComboNs, nowNs, buttons,
                           touches, trigger)) {
            std::lock_guard<std::mutex> lk(m_Mutex);
            m_StatCombos++;
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

    // §VR-WHEEL 快捷鍵：兩支搖桿同時按下 1.5 秒切換一次，放開才能再觸發（host 在兩支同時按下時不送 LS／RS）
    const bool wheelCombo = m_WheelToggled && focused && !m_TestInput && (next[0].buttons & VIPLE_VR_BTN_THUMBSTICK) &&
                            (next[1].buttons & VIPLE_VR_BTN_THUMBSTICK);
    if (!wheelCombo) {
        m_WheelComboStartNs = 0;
        m_WheelComboFired = false;
    }
    else {
        if (m_WheelComboStartNs == 0) {
            m_WheelComboStartNs = nowNs;
        }
        if (!m_WheelComboFired && nowNs - m_WheelComboStartNs >= kWheelComboNs) {
            m_WheelComboFired = true;
            uint8_t w = m_Wheel.load(std::memory_order_relaxed);
            w ^= VIPLE_VR_WHEEL_ON;
            m_Wheel.store(w, std::memory_order_relaxed);
            const bool on = (w & VIPLE_VR_WHEEL_ON) != 0;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-WHEEL] hotkey (both stick clicks): steering wheel %s",
                        on ? "on" : "off");
            m_WheelToggled(on);
        }
    }

    // edges 只在 frame thread 累加（takeStatsLine 也在 frame thread 歸零），直接以 next 覆蓋
    // （姿態欄位也只有 frame thread 的 updateGripPose 會寫，next 裡的就是目前的值）
    const uint64_t doneNs = syncDoneNs != 0 ? steadyNowNs() : 0;
    std::lock_guard<std::mutex> lk(m_Mutex);
    m_Hand[0] = next[0];
    m_Hand[1] = next[1];
    if (syncDoneNs != 0 && doneNs >= syncDoneNs) {
        // §PERF-1113：xrSyncActions 返回 → 快照可以被送出（改之前約一整格，改之後應該在 0.1 ms 上下）
        const uint32_t us = static_cast<uint32_t>((std::min<uint64_t>)((doneNs - syncDoneNs) / 1000ull, 0xffffffffu));
        m_StatSnapSumUs += us;
        m_StatSnapN++;
        m_StatSnapMaxUs = (std::max)(m_StatSnapMaxUs, us);
    }
}

// ── §VR-KBD ──
void XrVrControllers::setKeyboardOpen(bool open, const char* why)
{
    if (open == m_KbOpen || !m_KbKey) {
        return;
    }
    if (!open) {
        kbRelease();
        m_KbSticky = 0;
        m_KbHover = -1;
        m_KbHit = false;
        m_KbDragHand = -1;
        m_KbBarHover = false;
        m_KbMoveReq = false;
    }
    m_KbOpen = open;
    m_KbMaskUntilRelease = true;  // 開關那一下按著的鍵（menu、grip、或按關閉鍵的扳機）不送給遊戲
    m_KbTrigPrev[0] = m_KbRawTrigger[0];
    m_KbTrigPrev[1] = m_KbRawTrigger[1];
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-KBD] keyboard %s (%s)", open ? "opened" : "closed", why);
}

void XrVrControllers::kbSend(int vk, bool down, uint8_t mods)
{
    if (!m_KbTestText.isEmpty()) {
        // dev 才逐鍵記錄（一般模式只記數量，避免 log 變成 keylogger）
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-KBD] dev key vk=0x%02X %s mods=0x%X", vk, down ? "down" : "up",
                    static_cast<unsigned>(mods));
    }
    if (m_KbKey) {
        m_KbKey(vk, down, mods);
    }
}

// 規則同 XrInput::kbPress／kbRelease（XR 桌面的鍵盤）
void XrVrControllers::kbPress(int idx)
{
    using K = XrKeyboard::Kind;
    const XrKeyboard::Key& k = m_Kb.key(idx);
    switch (k.kind) {
    case K::Shift:
    case K::Ctrl:
    case K::Alt:
    case K::Win:
        m_KbSticky ^= XrKeyboard::modBit(k.kind);  // 黏滯：按一下保持，下一個一般鍵放開後自動放開
        break;
    case K::Close:
        setKeyboardOpen(false, "close key");
        break;
    case K::Ime:
        kbSend(k.vk, true, MODIFIER_SHIFT);  // 中/英：Shift 單擊（見 xrkeyboard.h）
        kbSend(k.vk, false, 0);
        m_StatKbKeys++;
        break;
    case K::Normal: {
        const uint8_t mods = m_KbSticky;
        uint8_t cur = 0;
        for (K mk : {K::Ctrl, K::Alt, K::Shift, K::Win}) {
            if (mods & XrKeyboard::modBit(mk)) {
                cur |= XrKeyboard::modBit(mk);
                kbSend(XrKeyboard::modVk(mk), true, cur);
            }
        }
        kbSend(k.vk, true, mods);
        m_KbActiveKey = idx;
        m_KbActiveMods = mods;
        m_StatKbKeys++;
        break;
    }
    }
}

void XrVrControllers::kbRelease()
{
    using K = XrKeyboard::Kind;
    m_KbPressHand = -1;
    if (m_KbActiveKey < 0) {
        return;
    }
    uint8_t cur = m_KbActiveMods;
    kbSend(m_Kb.key(m_KbActiveKey).vk, false, cur);
    for (K mk : {K::Win, K::Shift, K::Alt, K::Ctrl}) {
        if (cur & XrKeyboard::modBit(mk)) {
            cur &= static_cast<uint8_t>(~XrKeyboard::modBit(mk));
            kbSend(XrKeyboard::modVk(mk), false, cur);
        }
    }
    m_KbActiveKey = -1;
    m_KbActiveMods = 0;
    m_KbSticky = 0;  // 黏滯修飾鍵只作用一個鍵
}

bool XrVrControllers::keyboardPointer(XrPosef* pose) const
{
    if (!m_KbOpen || !m_KbHit) {
        return false;
    }
    *pose = m_KbHitPose;
    return true;
}

void XrVrControllers::updateKeyboard(XrTime t, XrSpace space, bool shown, const XrPosef& pose, float w, float h, const XrPosef& bar,
                                     float barW, float barH)
{
    // dev --xr-test-keyboard：20 s 後打開，鍵盤畫出來 1 s 後每 0.4 s 按一個鍵（前半按住），按完 1 s 後關
    if (!m_KbTestKeys.empty() && m_KbTestState < 2) {
        const uint64_t nowNs = steadyNowNs();
        if (m_KbTestState == 0) {
            if (nowNs - m_CreatedNs > 20000000000ull) {
                m_KbTestState = 1;
                m_KbTestShownNs = 0;
                setKeyboardOpen(true, "dev test");
            }
            return;
        }
        if (!shown) {
            return;
        }
        if (m_KbTestShownNs == 0) {
            m_KbTestShownNs = nowNs;
        }
        const int64_t ms = static_cast<int64_t>((nowNs - m_KbTestShownNs) / 1000000ull) - 1000;
        const int step = ms < 0 ? -1 : static_cast<int>(ms / 400);
        if (step >= static_cast<int>(m_KbTestKeys.size())) {
            kbRelease();
            if (!m_KbTestMoved && ms > static_cast<int64_t>(m_KbTestKeys.size()) * 400 + 300) {
                // §VR-KBD-DRAG 的自測：把鍵盤往右 10 cm、往上 5 cm 移一次（走和拖動相同的路徑）
                m_KbTestMoved = true;
                const XrRay::Vec3 d = XrRay::rotate(pose.orientation, {0.10f, 0.05f, 0.0f});
                m_KbMovePos = {pose.position.x + d.x, pose.position.y + d.y, pose.position.z + d.z};
                m_KbMoveReq = true;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-KBD] dev move: keyboard centre -> (%.2f, %.2f, %.2f)",
                            static_cast<double>(m_KbMovePos.x), static_cast<double>(m_KbMovePos.y), static_cast<double>(m_KbMovePos.z));
            }
            if (ms > static_cast<int64_t>(m_KbTestKeys.size()) * 400 + 1000) {
                m_KbTestState = 2;
                SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-KBD] dev test done: %u keys, keyboard at (%.2f, %.2f, %.2f)", m_StatKbKeys,
                            static_cast<double>(pose.position.x), static_cast<double>(pose.position.y), static_cast<double>(pose.position.z));
                setKeyboardOpen(false, "dev test");
            }
            return;
        }
        if (step < 0) {
            return;
        }
        const int idx = m_KbTestKeys[static_cast<size_t>(step)];
        const bool down = (ms % 400) < 200;
        float cu = 0.0f, cv = 0.0f, du = 0.0f, dv = 0.0f;
        m_Kb.keyUv(idx, &cu, &cv, &du, &dv);
        const XrRay::Vec3 c = XrRay::quadPoint(pose, w, h, cu, cv);
        const XrRay::Vec3 nrm = XrRay::rotate(pose.orientation, {0.0f, 0.0f, 0.004f});
        m_KbHover = idx;
        m_KbHit = true;
        m_KbHitPose.orientation = pose.orientation;
        m_KbHitPose.position = {c.x + nrm.x, c.y + nrm.y, c.z + nrm.z};
        if (down && m_KbActiveKey != idx) {
            kbRelease();
            kbPress(idx);
        }
        else if (!down) {
            kbRelease();
        }
        return;
    }

    if (!m_KbOpen || !shown || !m_Focused) {
        if (m_KbOpen) {
            kbRelease();
        }
        m_KbHover = -1;
        m_KbHit = false;
        m_KbDragHand = -1;
        m_KbBarHover = false;
        m_KbTrigPrev[0] = m_KbRawTrigger[0];
        m_KbTrigPrev[1] = m_KbRawTrigger[1];
        return;
    }
    // 兩手各自求交
    int key[2] = {-1, -1};
    bool hit[2] = {false, false};
    bool barHit[2] = {false, false};
    bool aimOk[2] = {false, false};
    XrPosef aim[2] = {};
    XrRay::Vec3 point[2] = {};
    for (int hnd = 0; hnd < 2; hnd++) {
        if (m_AimSpace[hnd] == XR_NULL_HANDLE) {
            continue;
        }
        XrSpaceLocation loc = xrStruct<XrSpaceLocation>(XR_TYPE_SPACE_LOCATION);
        const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
        if (XR_FAILED(xrLocateSpace(m_AimSpace[hnd], space, t, &loc)) || (loc.locationFlags & need) != need) {
            continue;
        }
        const XrRay::Vec3 origin = {loc.pose.position.x, loc.pose.position.y, loc.pose.position.z};
        const XrRay::Vec3 dir = XrRay::rotate(loc.pose.orientation, {0.0f, 0.0f, -1.0f});
        aimOk[hnd] = true;
        aim[hnd] = loc.pose;
        float u = 0.0f, v = 0.0f, dist = 0.0f;
        if (XrRay::intersectQuad(pose, w, h, origin, dir, &u, &v, &dist)) {
            hit[hnd] = true;
            key[hnd] = m_Kb.keyAt(u, v);
            point[hnd] = XrRay::quadPoint(pose, w, h, u, v);
        }
        else if (barW > 0.0f && XrRay::intersectQuad(bar, barW, barH, origin, dir, &u, &v, &dist)) {
            barHit[hnd] = true;
            point[hnd] = XrRay::quadPoint(bar, barW, barH, u, v);
        }
    }
    // §VR-KBD-DRAG：抓著白條時鍵盤像黏在那一手的射線上（抓住那一刻的相對位置不變），放開扳機就停在那裡
    if (m_KbDragHand >= 0) {
        const int d = m_KbDragHand;
        const bool held = m_KbRawTrigger[d] && !m_KbMaskUntilRelease;
        if (held && aimOk[d]) {
            const XrRay::Vec3 off = XrRay::rotate(aim[d].orientation, {m_KbDragRel.x, m_KbDragRel.y, m_KbDragRel.z});
            m_KbMovePos = {aim[d].position.x + off.x, aim[d].position.y + off.y, aim[d].position.z + off.z};
            m_KbMoveReq = true;
        }
        if (!held) {
            m_KbDragHand = -1;
            m_StatKbDrags++;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "[VIPLE-VR-KBD] keyboard moved (%s hand), now at (%.2f, %.2f, %.2f)",
                        d == 0 ? "left" : "right", static_cast<double>(pose.position.x), static_cast<double>(pose.position.y),
                        static_cast<double>(pose.position.z));
        }
        m_KbBarHover = true;
        m_KbHover = -1;
        m_KbHit = false;
        m_KbTrigPrev[0] = m_KbRawTrigger[0] && !m_KbMaskUntilRelease;
        m_KbTrigPrev[1] = m_KbRawTrigger[1] && !m_KbMaskUntilRelease;
        return;
    }
    // 扳機邊緣：按下的那一手變成目前的手；指著鍵、而且沒有別的鍵按著時按下那個鍵。放開看按下的那一手
    for (int hnd = 0; hnd < 2; hnd++) {
        const bool now = m_KbRawTrigger[hnd] && !m_KbMaskUntilRelease;
        if (now && !m_KbTrigPrev[hnd]) {
            m_KbHand = hnd;
            if (hit[hnd] && key[hnd] >= 0 && m_KbPressHand < 0) {
                m_KbPressHand = hnd;
                kbPress(key[hnd]);
                if (!m_KbOpen) {
                    return;  // 按到關閉鍵
                }
            }
            else if (barHit[hnd] && m_KbPressHand < 0) {
                // 鍵盤中心換到這一手的 aim 座標（共軛四元數＝反向旋轉）
                const XrQuaternionf inv = {-aim[hnd].orientation.x, -aim[hnd].orientation.y, -aim[hnd].orientation.z, aim[hnd].orientation.w};
                const XrRay::Vec3 rel = XrRay::rotate(inv, {pose.position.x - aim[hnd].position.x, pose.position.y - aim[hnd].position.y,
                                                             pose.position.z - aim[hnd].position.z});
                m_KbDragRel = {rel.x, rel.y, rel.z};
                m_KbDragHand = hnd;
                m_KbBarHover = true;
                m_KbHover = -1;
                m_KbTrigPrev[hnd] = true;
                return;
            }
        }
        else if (!now && m_KbTrigPrev[hnd] && m_KbPressHand == hnd) {
            kbRelease();
        }
        m_KbTrigPrev[hnd] = now;
    }
    // 沒有按著鍵時，目前的手沒指著鍵盤而另一手有，就換成另一手
    if (m_KbPressHand < 0 && !hit[m_KbHand] && hit[1 - m_KbHand]) {
        m_KbHand = 1 - m_KbHand;
    }
    if (m_KbPressHand < 0 && !hit[m_KbHand] && !barHit[m_KbHand] && barHit[1 - m_KbHand]) {
        m_KbHand = 1 - m_KbHand;
    }
    const int hnd = m_KbPressHand >= 0 ? m_KbPressHand : m_KbHand;
    m_KbBarHover = m_KbPressHand < 0 && barHit[hnd];
    m_KbHit = hit[hnd] || m_KbBarHover;
    m_KbHover = hit[hnd] ? key[hnd] : -1;
    if (m_KbBarHover) {
        const XrRay::Vec3 nrm = XrRay::rotate(bar.orientation, {0.0f, 0.0f, 0.004f});
        m_KbHitPose.orientation = bar.orientation;
        m_KbHitPose.position = {point[hnd].x + nrm.x, point[hnd].y + nrm.y, point[hnd].z + nrm.z};
    }
    if (hit[hnd]) {
        const XrRay::Vec3 nrm = XrRay::rotate(pose.orientation, {0.0f, 0.0f, 0.004f});
        m_KbHitPose.orientation = pose.orientation;
        m_KbHitPose.position = {point[hnd].x + nrm.x, point[hnd].y + nrm.y, point[hnd].z + nrm.z};
    }
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
        in.reserved = h == 0 ? m_Wheel.load(std::memory_order_relaxed) : 0;  // §VR-WHEEL：只放在左手
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
    uint32_t snapAvgUs = 0, snapMaxUs = 0, snapN = 0;  // §PERF-1113
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
        snapAvgUs = m_StatSnapN ? static_cast<uint32_t>(m_StatSnapSumUs / m_StatSnapN) : 0;
        snapMaxUs = m_StatSnapMaxUs;
        snapN = m_StatSnapN;
        m_StatSnapSumUs = 0;
        m_StatSnapN = 0;
        m_StatSnapMaxUs = 0;
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
    QString velLine = QStringLiteral(" | vel L %1 | vel R %2").arg(velText(vel[0]), velText(vel[1]));
    if (m_KbOpen || m_StatKbKeys != 0) {
        // §VR-KBD：只記數量，不記按了哪些鍵
        velLine += QStringLiteral(" | kbd %1 keys=%2 moves=%3").arg(m_KbOpen ? QStringLiteral("open") : QStringLiteral("closed")).arg(m_StatKbKeys).arg(m_StatKbDrags);
        m_StatKbKeys = 0;
    }
    if (snapN != 0) {
        // §PERF-1113：尾端加一欄，前段欄位順序不變
        velLine += QStringLiteral(" | sync2snap avg=%1 max=%2 us").arg(snapAvgUs).arg(snapMaxUs);
    }
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
