#include "qxr_xr_input.h"

#include "qxr_log.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

namespace {

constexpr float kClickOn = 0.70f;
constexpr float kTouchOn = 0.05f;
constexpr uint64_t kComboNs = 1000000000ull;
constexpr int kHapticQueueMax = 64;

template <typename T>
T xrZero(XrStructureType type)
{
    T value{};
    value.type = type;
    return value;
}

uint64_t monoNowNs()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

uint16_t unorm16(float v)
{
    if (v < 0.0f) {
        v = 0.0f;
    }
    if (v > 1.0f) {
        v = 1.0f;
    }
    return static_cast<uint16_t>(std::lround(v * 65535.0f));
}

int16_t snorm16(float v)
{
    if (v < -1.0f) {
        v = -1.0f;
    }
    if (v > 1.0f) {
        v = 1.0f;
    }
    return static_cast<int16_t>(std::lround(v * 32767.0f));
}

const char* handName(int hand)
{
    return hand == 0 ? "L" : "R";
}

void logCtrl(XrInstance instance, const char* fn, XrResult result)
{
    char text[XR_MAX_RESULT_STRING_SIZE] = {};
    const char* extra = "";
    if (instance != XR_NULL_HANDLE && XR_SUCCEEDED(xrResultToString(instance, result, text))) {
        extra = text;
    }
    QXR_LOGE("CTRL", "%s failed: XrResult=%d %s", fn, static_cast<int>(result), extra);
}

void logCtrlLimited(XrInstance instance, const char* fn, XrResult result, std::atomic<uint32_t>* counter)
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
    QXR_LOGE("CTRL", "%s failed: XrResult=%d %s (occurrence %u)", fn, static_cast<int>(result), extra, n);
}

struct Bind {
    XrAction action;
    const char* path;
};

}  // namespace

bool QxrXrControllers::applySystemCombo(ComboState& st, uint64_t nowNs, uint32_t& buttons, uint32_t& touches, float& trigger)
{
    // §QUEST-PCVR：menu＋trigger 按滿 1 秒要變成 SYSTEM，放開後還按著的那一鍵繼續遮到它也放開。
    // 否則 host 會把遮罩結束看成一次新的按下。算法與 Frame 的 xrcombo::apply 相同。
    const uint32_t menuBit = VIPLE_VR_BTN_MENU;
    const uint32_t triggerBit = VIPLE_VR_BTN_TRIGGER;
    const uint32_t systemBit = VIPLE_VR_BTN_SYSTEM;
    const bool held = (buttons & menuBit) != 0 && (buttons & triggerBit) != 0;
    bool fired = false;
    if (!held) {
        if (st.active) {
            st.afterMask = menuBit | triggerBit;
        }
        st.startNs = 0;
        st.active = false;
    }
    else {
        if (st.startNs == 0) {
            st.startNs = nowNs;
        }
        if (!st.active && nowNs - st.startNs >= kComboNs) {
            st.active = true;
            fired = true;
        }
    }
    if (st.active) {
        buttons = (buttons & ~(menuBit | triggerBit)) | systemBit;
        touches &= ~triggerBit;
        trigger = 0.0f;
    }
    else if (st.afterMask != 0) {
        st.afterMask &= buttons;
        if (st.afterMask != 0) {
            buttons &= ~st.afterMask;
            if ((st.afterMask & triggerBit) != 0) {
                touches &= ~triggerBit;
                trigger = 0.0f;
            }
        }
    }
    return fired;
}

QxrXrControllers::QxrXrControllers() = default;

QxrXrControllers::~QxrXrControllers()
{
    destroy();
}

bool QxrXrControllers::create(XrInstance instance, XrSession session, bool touchPlus, std::string* error)
{
    instance_ = instance;
    session_ = session;
    auto failCreate = [&](const char* fn, XrResult result) {
        logCtrl(instance_, fn, result);
        if (error != nullptr) {
            char buf[160];
            std::snprintf(buf, sizeof(buf), "%s failed: XrResult=%d", fn, static_cast<int>(result));
            *error = buf;
        }
        return false;
    };

    XrResult result = xrStringToPath(instance_, "/user/hand/left", &hands_[0]);
    if (!XR_SUCCEEDED(result)) {
        return failCreate("xrStringToPath(/user/hand/left)", result);
    }
    result = xrStringToPath(instance_, "/user/hand/right", &hands_[1]);
    if (!XR_SUCCEEDED(result)) {
        return failCreate("xrStringToPath(/user/hand/right)", result);
    }

    XrActionSetCreateInfo setInfo = xrZero<XrActionSetCreateInfo>(XR_TYPE_ACTION_SET_CREATE_INFO);
    std::snprintf(setInfo.actionSetName, sizeof(setInfo.actionSetName), "viple_pcvr");
    std::snprintf(setInfo.localizedActionSetName, sizeof(setInfo.localizedActionSetName), "VipleStream PCVR");
    result = xrCreateActionSet(instance_, &setInfo, &set_);
    if (!XR_SUCCEEDED(result)) {
        set_ = XR_NULL_HANDLE;
        return failCreate("xrCreateActionSet", result);
    }

    auto makeAction = [&](const char* name, const char* localized, XrActionType type, XrAction* out) {
        XrActionCreateInfo info = xrZero<XrActionCreateInfo>(XR_TYPE_ACTION_CREATE_INFO);
        std::snprintf(info.actionName, sizeof(info.actionName), "%s", name);
        std::snprintf(info.localizedActionName, sizeof(info.localizedActionName), "%s", localized);
        info.actionType = type;
        info.countSubactionPaths = 2;
        info.subactionPaths = hands_;
        const XrResult created = xrCreateAction(set_, &info, out);
        if (!XR_SUCCEEDED(created)) {
            *out = XR_NULL_HANDLE;
            char fn[96];
            std::snprintf(fn, sizeof(fn), "xrCreateAction(%s)", name);
            failCreate(fn, created);
            return false;
        }
        return true;
    };

    if (!makeAction("grip_pose", "Hand pose", XR_ACTION_TYPE_POSE_INPUT, &gripPose_) ||
        !makeAction("aim_pose", "Aim pose", XR_ACTION_TYPE_POSE_INPUT, &aimPose_) ||
        !makeAction("trigger_value", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT, &triggerValue_) ||
        !makeAction("trigger_click", "Trigger click", XR_ACTION_TYPE_BOOLEAN_INPUT, &triggerClick_) ||
        !makeAction("trigger_touch", "Trigger touch", XR_ACTION_TYPE_BOOLEAN_INPUT, &triggerTouch_) ||
        !makeAction("squeeze_value", "Grip", XR_ACTION_TYPE_FLOAT_INPUT, &squeezeValue_) ||
        !makeAction("squeeze_click", "Grip click", XR_ACTION_TYPE_BOOLEAN_INPUT, &squeezeClick_) ||
        !makeAction("thumbstick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT, &stick_) ||
        !makeAction("thumbstick_click", "Thumbstick click", XR_ACTION_TYPE_BOOLEAN_INPUT, &stickClick_) ||
        !makeAction("thumbstick_touch", "Thumbstick touch", XR_ACTION_TYPE_BOOLEAN_INPUT, &stickTouch_) ||
        !makeAction("button_a", "A / X", XR_ACTION_TYPE_BOOLEAN_INPUT, &btnLo_) ||
        !makeAction("button_a_touch", "A / X touch", XR_ACTION_TYPE_BOOLEAN_INPUT, &btnLoTouch_) ||
        !makeAction("button_b", "B / Y", XR_ACTION_TYPE_BOOLEAN_INPUT, &btnHi_) ||
        !makeAction("button_b_touch", "B / Y touch", XR_ACTION_TYPE_BOOLEAN_INPUT, &btnHiTouch_) ||
        !makeAction("menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT, &menu_) ||
        !makeAction("system", "System", XR_ACTION_TYPE_BOOLEAN_INPUT, &system_) ||
        !makeAction("thumbrest_touch", "Thumbrest touch", XR_ACTION_TYPE_BOOLEAN_INPUT, &thumbrestTouch_) ||
        !makeAction("haptic", "Haptics", XR_ACTION_TYPE_VIBRATION_OUTPUT, &haptic_)) {
        return false;
    }

    auto suggestOnce = [&](const char* profile, const std::vector<Bind>& list, bool quiet) {
        XrPath profilePath = XR_NULL_PATH;
        const XrResult pathResult = xrStringToPath(instance_, profile, &profilePath);
        if (!XR_SUCCEEDED(pathResult)) {
            if (!quiet) {
                logCtrl(instance_, "xrStringToPath(interaction profile)", pathResult);
                QXR_LOGE("CTRL", "interaction profile path rejected: %s", profile);
            }
            return false;
        }
        std::vector<XrActionSuggestedBinding> bindings;
        bindings.reserve(list.size());
        for (const Bind& bind : list) {
            XrPath path = XR_NULL_PATH;
            const XrResult pathResult = xrStringToPath(instance_, bind.path, &path);
            if (!XR_SUCCEEDED(pathResult)) {
                logCtrl(instance_, "xrStringToPath(binding)", pathResult);
                QXR_LOGW("CTRL", "skip binding path %s", bind.path);
                continue;
            }
            bindings.push_back({bind.action, path});
        }
        if (bindings.empty()) {
            if (!quiet) {
                QXR_LOGE("CTRL", "bindings empty: %s", profile);
            }
            return false;
        }
        XrInteractionProfileSuggestedBinding suggested =
            xrZero<XrInteractionProfileSuggestedBinding>(XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING);
        suggested.interactionProfile = profilePath;
        suggested.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
        suggested.suggestedBindings = bindings.data();
        const XrResult suggestedResult = xrSuggestInteractionProfileBindings(instance_, &suggested);
        if (XR_SUCCEEDED(suggestedResult)) {
            QXR_LOGI("CTRL", "bindings accepted: %s (%u)", profile, static_cast<unsigned>(bindings.size()));
            return true;
        }
        if (!quiet) {
            logCtrl(instance_, "xrSuggestInteractionProfileBindings", suggestedResult);
            QXR_LOGE("CTRL", "bindings rejected: %s", profile);
        }
        return false;
    };

    // §QUEST-PCVR：同一 profile 整份接受或整份拒絕。完整清單被拒就再送較小的那份，後一次取代前一次。
    auto suggest = [&](const char* profile, const std::vector<Bind>& full, const std::vector<Bind>& core) {
        // 完整清單被拒時也要留下 XrResult。退回的較小清單會取代前一次建議。
        if (suggestOnce(profile, full, false)) {
            return;
        }
        QXR_LOGW("CTRL", "%s: full binding list rejected, retrying core set", profile);
        suggestOnce(profile, core, false);
    };

    const std::vector<Bind> aim = {
        {aimPose_, "/user/hand/left/input/aim/pose"},
        {aimPose_, "/user/hand/right/input/aim/pose"},
    };

    {
        std::vector<Bind> core = {
            {gripPose_, "/user/hand/left/input/grip/pose"},
            {gripPose_, "/user/hand/right/input/grip/pose"},
            {triggerValue_, "/user/hand/left/input/select/click"},
            {triggerValue_, "/user/hand/right/input/select/click"},
            {triggerClick_, "/user/hand/left/input/select/click"},
            {triggerClick_, "/user/hand/right/input/select/click"},
            {menu_, "/user/hand/left/input/menu/click"},
            {menu_, "/user/hand/right/input/menu/click"},
            {haptic_, "/user/hand/left/output/haptic"},
            {haptic_, "/user/hand/right/output/haptic"},
        };
        std::vector<Bind> full = core;
        full.insert(full.end(), aim.begin(), aim.end());
        suggest("/interaction_profiles/khr/simple_controller", full, core);
    }

    auto suggestTouchLike = [&](const char* profile) {
        // Touch 沒有 trigger/click、squeeze/click。路徑寫進去的話整份建議會被拒，click 改由類比值門檻推出。
        std::vector<Bind> core = {
            {gripPose_, "/user/hand/left/input/grip/pose"},
            {gripPose_, "/user/hand/right/input/grip/pose"},
            {triggerValue_, "/user/hand/left/input/trigger/value"},
            {triggerValue_, "/user/hand/right/input/trigger/value"},
            {squeezeValue_, "/user/hand/left/input/squeeze/value"},
            {squeezeValue_, "/user/hand/right/input/squeeze/value"},
            {stick_, "/user/hand/left/input/thumbstick"},
            {stick_, "/user/hand/right/input/thumbstick"},
            {stickClick_, "/user/hand/left/input/thumbstick/click"},
            {stickClick_, "/user/hand/right/input/thumbstick/click"},
            {btnLo_, "/user/hand/left/input/x/click"},
            {btnLo_, "/user/hand/right/input/a/click"},
            {btnHi_, "/user/hand/left/input/y/click"},
            {btnHi_, "/user/hand/right/input/b/click"},
            {menu_, "/user/hand/left/input/menu/click"},
            {haptic_, "/user/hand/left/output/haptic"},
            {haptic_, "/user/hand/right/output/haptic"},
        };
        std::vector<Bind> full = core;
        const std::vector<Bind> extra = {
            {triggerTouch_, "/user/hand/left/input/trigger/touch"},
            {triggerTouch_, "/user/hand/right/input/trigger/touch"},
            {stickTouch_, "/user/hand/left/input/thumbstick/touch"},
            {stickTouch_, "/user/hand/right/input/thumbstick/touch"},
            {btnLoTouch_, "/user/hand/left/input/x/touch"},
            {btnLoTouch_, "/user/hand/right/input/a/touch"},
            {btnHiTouch_, "/user/hand/left/input/y/touch"},
            {btnHiTouch_, "/user/hand/right/input/b/touch"},
            {thumbrestTouch_, "/user/hand/left/input/thumbrest/touch"},
            {thumbrestTouch_, "/user/hand/right/input/thumbrest/touch"},
            {system_, "/user/hand/right/input/system/click"},
        };
        full.insert(full.end(), extra.begin(), extra.end());
        full.insert(full.end(), aim.begin(), aim.end());
        suggest(profile, full, core);
    };

    suggestTouchLike("/interaction_profiles/oculus/touch_controller");
    if (touchPlus) {
        // §QUEST-PCVR：plus 多出來的 proximity 路徑沒有把握，綁進去會讓整份被拒。只綁與 Touch 相同的元件。
        suggestTouchLike("/interaction_profiles/meta/touch_controller_plus");
    }

    XrSessionActionSetsAttachInfo attach = xrZero<XrSessionActionSetsAttachInfo>(XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO);
    attach.countActionSets = 1;
    attach.actionSets = &set_;
    result = xrAttachSessionActionSets(session_, &attach);
    if (!XR_SUCCEEDED(result)) {
        return failCreate("xrAttachSessionActionSets", result);
    }

    for (int hand = 0; hand < 2; ++hand) {
        XrActionSpaceCreateInfo spaceInfo = xrZero<XrActionSpaceCreateInfo>(XR_TYPE_ACTION_SPACE_CREATE_INFO);
        spaceInfo.action = gripPose_;
        spaceInfo.subactionPath = hands_[hand];
        spaceInfo.poseInActionSpace.orientation.w = 1.0f;
        result = xrCreateActionSpace(session_, &spaceInfo, &gripSpace_[hand]);
        if (!XR_SUCCEEDED(result)) {
            gripSpace_[hand] = XR_NULL_HANDLE;
            char fn[64];
            std::snprintf(fn, sizeof(fn), "xrCreateActionSpace(grip %s)", handName(hand));
            logCtrl(instance_, fn, result);
        }
        spaceInfo.action = aimPose_;
        result = xrCreateActionSpace(session_, &spaceInfo, &aimSpace_[hand]);
        if (!XR_SUCCEEDED(result)) {
            aimSpace_[hand] = XR_NULL_HANDLE;
            char fn[64];
            std::snprintf(fn, sizeof(fn), "xrCreateActionSpace(aim %s)", handName(hand));
            logCtrl(instance_, fn, result);
        }
    }
    if (error != nullptr) {
        error->clear();
    }
    return true;
}

void QxrXrControllers::destroy()
{
    for (XrSpace& space : gripSpace_) {
        if (space != XR_NULL_HANDLE) {
            const XrResult result = xrDestroySpace(space);
            space = XR_NULL_HANDLE;
            if (!XR_SUCCEEDED(result)) {
                logCtrl(instance_, "xrDestroySpace(grip)", result);
            }
        }
    }
    for (XrSpace& space : aimSpace_) {
        if (space != XR_NULL_HANDLE) {
            const XrResult result = xrDestroySpace(space);
            space = XR_NULL_HANDLE;
            if (!XR_SUCCEEDED(result)) {
                logCtrl(instance_, "xrDestroySpace(aim)", result);
            }
        }
    }
    XrAction* actions[] = {&gripPose_,     &aimPose_,        &triggerValue_, &triggerClick_, &triggerTouch_,
                           &squeezeValue_, &squeezeClick_,   &stick_,        &stickClick_,   &stickTouch_,
                           &btnLo_,        &btnLoTouch_,     &btnHi_,        &btnHiTouch_,   &menu_,
                           &system_,       &thumbrestTouch_, &haptic_};
    for (XrAction* action : actions) {
        if (*action != XR_NULL_HANDLE) {
            const XrResult result = xrDestroyAction(*action);
            *action = XR_NULL_HANDLE;
            if (!XR_SUCCEEDED(result)) {
                logCtrl(instance_, "xrDestroyAction", result);
            }
        }
    }
    if (set_ != XR_NULL_HANDLE) {
        const XrResult result = xrDestroyActionSet(set_);
        set_ = XR_NULL_HANDLE;
        if (!XR_SUCCEEDED(result)) {
            logCtrl(instance_, "xrDestroyActionSet", result);
        }
    }
    session_ = XR_NULL_HANDLE;
}

bool QxrXrControllers::stateOk(XrResult result, const char* fn) const
{
    if (XR_SUCCEEDED(result)) {
        return true;
    }
    logCtrlLimited(instance_, fn, result, &stateFails_);
    return false;
}

bool QxrXrControllers::boolState(XrAction action, int hand) const
{
    if (action == XR_NULL_HANDLE || session_ == XR_NULL_HANDLE) {
        return false;
    }
    XrActionStateGetInfo info = xrZero<XrActionStateGetInfo>(XR_TYPE_ACTION_STATE_GET_INFO);
    info.action = action;
    info.subactionPath = hands_[hand];
    XrActionStateBoolean state = xrZero<XrActionStateBoolean>(XR_TYPE_ACTION_STATE_BOOLEAN);
    const XrResult result = xrGetActionStateBoolean(session_, &info, &state);
    return stateOk(result, "xrGetActionStateBoolean") && state.isActive && state.currentState;
}

float QxrXrControllers::floatState(XrAction action, int hand) const
{
    if (action == XR_NULL_HANDLE || session_ == XR_NULL_HANDLE) {
        return 0.0f;
    }
    XrActionStateGetInfo info = xrZero<XrActionStateGetInfo>(XR_TYPE_ACTION_STATE_GET_INFO);
    info.action = action;
    info.subactionPath = hands_[hand];
    XrActionStateFloat state = xrZero<XrActionStateFloat>(XR_TYPE_ACTION_STATE_FLOAT);
    const XrResult result = xrGetActionStateFloat(session_, &info, &state);
    if (stateOk(result, "xrGetActionStateFloat") && state.isActive) {
        return state.currentState;
    }
    return 0.0f;
}

XrVector2f QxrXrControllers::vecState(XrAction action, int hand) const
{
    XrVector2f zero{0.0f, 0.0f};
    if (action == XR_NULL_HANDLE || session_ == XR_NULL_HANDLE) {
        return zero;
    }
    XrActionStateGetInfo info = xrZero<XrActionStateGetInfo>(XR_TYPE_ACTION_STATE_GET_INFO);
    info.action = action;
    info.subactionPath = hands_[hand];
    XrActionStateVector2f state = xrZero<XrActionStateVector2f>(XR_TYPE_ACTION_STATE_VECTOR2F);
    const XrResult result = xrGetActionStateVector2f(session_, &info, &state);
    if (stateOk(result, "xrGetActionStateVector2f") && state.isActive) {
        return state.currentState;
    }
    return zero;
}

bool QxrXrControllers::poseIsActive(int hand) const
{
    if (gripPose_ == XR_NULL_HANDLE || session_ == XR_NULL_HANDLE) {
        return false;
    }
    XrActionStateGetInfo info = xrZero<XrActionStateGetInfo>(XR_TYPE_ACTION_STATE_GET_INFO);
    info.action = gripPose_;
    info.subactionPath = hands_[hand];
    XrActionStatePose state = xrZero<XrActionStatePose>(XR_TYPE_ACTION_STATE_POSE);
    const XrResult result = xrGetActionStatePose(session_, &info, &state);
    return stateOk(result, "xrGetActionStatePose") && state.isActive;
}

bool QxrXrControllers::locateGrip(int hand, XrSpace base, XrTime t, Hand* out)
{
    if (gripSpace_[hand] == XR_NULL_HANDLE || base == XR_NULL_HANDLE || t == 0 || out == nullptr) {
        return false;
    }
    XrSpaceVelocity velocity = xrZero<XrSpaceVelocity>(XR_TYPE_SPACE_VELOCITY);
    XrSpaceLocation location = xrZero<XrSpaceLocation>(XR_TYPE_SPACE_LOCATION);
    location.next = &velocity;
    const XrResult result = xrLocateSpace(gripSpace_[hand], base, t, &location);
    if (!XR_SUCCEEDED(result)) {
        logCtrlLimited(instance_, "xrLocateSpace(grip)", result, &locateFails_);
        return false;
    }
    // 手把要位置與方向都有效才標 LEFT／RIGHT 與 CTRL_ACTIVE。缺一就整筆姿態不用。
    const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    if ((location.locationFlags & need) != need) {
        return false;
    }
    out->pos[0] = location.pose.position.x;
    out->pos[1] = location.pose.position.y;
    out->pos[2] = location.pose.position.z;
    out->rot[0] = location.pose.orientation.x;
    out->rot[1] = location.pose.orientation.y;
    out->rot[2] = location.pose.orientation.z;
    out->rot[3] = location.pose.orientation.w;
    // §QUEST-PCVR：速度沒有獨立的線上旗標。bit 沒設就送 0，host 才不會拿未定義的速度去外插。
    const bool linear = (velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0;
    const bool angular = (velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) != 0;
    out->linVel[0] = linear ? velocity.linearVelocity.x : 0.0f;
    out->linVel[1] = linear ? velocity.linearVelocity.y : 0.0f;
    out->linVel[2] = linear ? velocity.linearVelocity.z : 0.0f;
    out->angVel[0] = angular ? velocity.angularVelocity.x : 0.0f;
    out->angVel[1] = angular ? velocity.angularVelocity.y : 0.0f;
    out->angVel[2] = angular ? velocity.angularVelocity.z : 0.0f;
    out->linValid = linear;
    out->angValid = angular;
    out->poseValid = true;
    return true;
}

void QxrXrControllers::updateButtons(bool focused)
{
    const uint64_t nowNs = monoNowNs();
    Hand next[2];
    {
        std::lock_guard<std::mutex> lock(mu_);
        next[0] = hand_[0];
        next[1] = hand_[1];
        focused_ = focused;
    }

    for (int hand = 0; hand < 2; ++hand) {
        Hand& hd = next[hand];
        uint32_t buttons = 0;
        uint32_t touches = 0;
        float trigger = 0.0f;
        float grip = 0.0f;
        float stickX = 0.0f;
        float stickY = 0.0f;
        if (focused) {
            trigger = floatState(triggerValue_, hand);
            grip = floatState(squeezeValue_, hand);
            const XrVector2f stick = vecState(stick_, hand);
            stickX = stick.x;
            stickY = stick.y;
            if (boolState(system_, hand)) {
                buttons |= VIPLE_VR_BTN_SYSTEM;
            }
            if (boolState(menu_, hand)) {
                buttons |= VIPLE_VR_BTN_MENU;
            }
            if (boolState(btnLo_, hand)) {
                buttons |= VIPLE_VR_BTN_A;
            }
            if (boolState(btnHi_, hand)) {
                buttons |= VIPLE_VR_BTN_B;
            }
            if (boolState(stickClick_, hand)) {
                buttons |= VIPLE_VR_BTN_THUMBSTICK;
            }
            if (boolState(triggerClick_, hand)) {
                buttons |= VIPLE_VR_BTN_TRIGGER;
            }
            if (boolState(squeezeClick_, hand)) {
                buttons |= VIPLE_VR_BTN_GRIP;
            }
            if (boolState(btnLoTouch_, hand)) {
                touches |= VIPLE_VR_BTN_A;
            }
            if (boolState(btnHiTouch_, hand)) {
                touches |= VIPLE_VR_BTN_B;
            }
            if (boolState(stickTouch_, hand)) {
                touches |= VIPLE_VR_BTN_THUMBSTICK;
            }
            if (boolState(triggerTouch_, hand)) {
                touches |= VIPLE_VR_BTN_TRIGGER;
            }
            if (boolState(thumbrestTouch_, hand)) {
                touches |= VIPLE_VR_BTN_THUMBREST;
            }
        }
        if (trigger >= kClickOn) {
            buttons |= VIPLE_VR_BTN_TRIGGER;
        }
        if (grip >= kClickOn) {
            buttons |= VIPLE_VR_BTN_GRIP;
        }
        if (trigger > kTouchOn) {
            touches |= VIPLE_VR_BTN_TRIGGER;
        }
        if (grip > kTouchOn) {
            touches |= VIPLE_VR_BTN_GRIP;
        }
        touches |= buttons & (VIPLE_VR_BTN_A | VIPLE_VR_BTN_B | VIPLE_VR_BTN_C | VIPLE_VR_BTN_D | VIPLE_VR_BTN_BUMPER |
                               VIPLE_VR_BTN_THUMBSTICK | VIPLE_VR_BTN_TRIGGER);
        if (!focused) {
            buttons = 0;
            touches = 0;
            trigger = 0.0f;
            grip = 0.0f;
            stickX = 0.0f;
            stickY = 0.0f;
        }
        if (applySystemCombo(hd.combo, nowNs, buttons, touches, trigger)) {
            QXR_LOGI("CTRL", "menu+trigger held 1 s -> SYSTEM (%s)", handName(hand));
        }

        const uint32_t rising = buttons & ~hd.buttons & 0xFFFFu;
        if (rising != 0) {
            for (int bit = 0; bit < 16; ++bit) {
                if ((rising & (1u << bit)) == 0) {
                    continue;
                }
                const uint32_t shift = 2u * static_cast<uint32_t>(bit);
                const uint32_t ctr = ((hd.pressCtr >> shift) + 1u) & 3u;
                hd.pressCtr = (hd.pressCtr & ~(3u << shift)) | (ctr << shift);
                hd.edges++;
            }
        }
        hd.buttons = buttons;
        hd.touches = touches;
        hd.trigger = unorm16(trigger);
        hd.grip = unorm16(grip);
        hd.stickX = snorm16(stickX);
        hd.stickY = snorm16(stickY);
    }

    std::lock_guard<std::mutex> lock(mu_);
    hand_[0] = next[0];
    hand_[1] = next[1];
    statSnapshots_++;
}

void QxrXrControllers::updateGripPose(XrTime predictedDisplayTime, XrSpace base)
{
    bool active[2] = {false, false};
    bool located[2] = {false, false};
    Hand fresh[2];
    for (int hand = 0; hand < 2; ++hand) {
        active[hand] = poseIsActive(hand);
        located[hand] = locateGrip(hand, base, predictedDisplayTime, &fresh[hand]);
    }
    std::lock_guard<std::mutex> lock(mu_);
    for (int hand = 0; hand < 2; ++hand) {
        Hand& hd = hand_[hand];
        hd.poseActive = active[hand];
        if (located[hand]) {
            hd.poseValid = true;
            std::memcpy(hd.pos, fresh[hand].pos, sizeof(hd.pos));
            std::memcpy(hd.rot, fresh[hand].rot, sizeof(hd.rot));
            std::memcpy(hd.linVel, fresh[hand].linVel, sizeof(hd.linVel));
            std::memcpy(hd.angVel, fresh[hand].angVel, sizeof(hd.angVel));
            hd.linValid = fresh[hand].linValid;
            hd.angValid = fresh[hand].angValid;
        }
        else {
            hd.poseValid = false;
            hd.linValid = false;
            hd.angValid = false;
        }
    }
}

void QxrXrControllers::fill(VIPLE_VR_TRACKING* sample, XrTime t, XrSpace base)
{
    if (sample == nullptr) {
        return;
    }
    Hand snap[2];
    bool focused = false;
    uint8_t wire[2] = {};
    {
        std::lock_guard<std::mutex> lock(mu_);
        snap[0] = hand_[0];
        snap[1] = hand_[1];
        focused = focused_;
        wire[0] = profileWire_[0];
        wire[1] = profileWire_[1];
    }
    uint32_t poseOkAdd[2] = {};
    for (int hand = 0; hand < 2; ++hand) {
        Hand& hd = snap[hand];
        if (t != 0) {
            Hand located = hd;
            if (locateGrip(hand, base, t, &located)) {
                hd = located;
            }
        }
        const bool active = hd.poseActive && hd.poseValid;
        if (active) {
            VIPLE_VR_POSE& pose = sample->pose[hand == 0 ? VIPLE_VR_POSE_LEFT : VIPLE_VR_POSE_RIGHT];
            std::memcpy(pose.pos, hd.pos, sizeof(pose.pos));
            std::memcpy(pose.rot, hd.rot, sizeof(pose.rot));
            std::memcpy(pose.linVel, hd.linVel, sizeof(pose.linVel));
            std::memcpy(pose.angVel, hd.angVel, sizeof(pose.angVel));
            sample->flags = static_cast<uint8_t>(sample->flags | (hand == 0 ? VIPLE_VR_TRK_LEFT : VIPLE_VR_TRK_RIGHT));
        }
        VIPLE_VR_CONTROLLER_INPUT& input = sample->input[hand];
        input.buttons = hd.buttons;
        input.touches = hd.touches;
        input.pressCtr = hd.pressCtr;
        input.trigger = hd.trigger;
        input.grip = hd.grip;
        input.stickX = hd.stickX;
        input.stickY = hd.stickY;
        input.battery = 255;
        input.flags = static_cast<uint8_t>((active ? VIPLE_VR_CTRL_ACTIVE : 0) | (focused ? VIPLE_VR_CTRL_FOCUSED : 0));
        input.profile = wire[hand];
        input.reserved = 0;
        poseOkAdd[hand] = active ? 1u : 0u;
    }
    std::lock_guard<std::mutex> lock(mu_);
    for (int hand = 0; hand < 2; ++hand) {
        statPoseN_[hand]++;
        statPoseOk_[hand] += poseOkAdd[hand];
    }
}

void QxrXrControllers::queueHaptic(int hand, int64_t durationNs, float frequencyHz, float amplitude)
{
    if (hand != 0 && hand != 1) {
        return;
    }
    std::lock_guard<std::mutex> lock(hapticMu_);
    if (session_ == XR_NULL_HANDLE || haptic_ == XR_NULL_HANDLE) {
        return;
    }
    if (static_cast<int>(haptics_.size()) >= kHapticQueueMax) {
        haptics_.pop_front();
        if (!hapticDropLogged_) {
            hapticDropLogged_ = true;
            QXR_LOGW("CTRL", "haptic queue full; dropping the oldest");
        }
    }
    haptics_.push_back({hand, durationNs, frequencyHz, amplitude});
}

void QxrXrControllers::applyPendingHaptics()
{
    std::deque<HapticReq> todo;
    {
        std::lock_guard<std::mutex> lock(hapticMu_);
        todo.swap(haptics_);
    }
    for (const HapticReq& req : todo) {
        if (req.hand != 0 && req.hand != 1) {
            statHapticFailed_++;
            continue;
        }
        if (session_ == XR_NULL_HANDLE || haptic_ == XR_NULL_HANDLE) {
            statHapticFailed_++;
            continue;
        }
        XrHapticActionInfo info = xrZero<XrHapticActionInfo>(XR_TYPE_HAPTIC_ACTION_INFO);
        info.action = haptic_;
        info.subactionPath = hands_[req.hand];
        XrResult result;
        const char* fn = "xrApplyHapticFeedback";
        // 停止只看振幅。XR_MIN_HAPTIC_DURATION（-1）是最短脈衝，不能拿 duration <= 0 去停。
        float amplitude = req.amplitude;
        if (!std::isfinite(amplitude) || amplitude <= 0.0f) {
            fn = "xrStopHapticFeedback";
            result = xrStopHapticFeedback(session_, &info);
        }
        else {
            XrHapticVibration vibration = xrZero<XrHapticVibration>(XR_TYPE_HAPTIC_VIBRATION);
            vibration.duration = req.durationNs;
            vibration.frequency = (std::isfinite(req.frequencyHz) && req.frequencyHz > 0.0f) ? req.frequencyHz
                                                                                             : XR_FREQUENCY_UNSPECIFIED;
            if (amplitude > 1.0f) {
                amplitude = 1.0f;
            }
            vibration.amplitude = amplitude;
            result = xrApplyHapticFeedback(session_, &info, reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
        }
        if (XR_SUCCEEDED(result)) {
            statHapticApplied_++;
        }
        else {
            statHapticFailed_++;
            if (statHapticFailed_ <= 3) {
                logCtrl(instance_, fn, result);
            }
        }
    }
    logStatsIfDue();
}

void QxrXrControllers::onProfileChanged()
{
    if (session_ == XR_NULL_HANDLE || instance_ == XR_NULL_HANDLE) {
        return;
    }
    for (int hand = 0; hand < 2; ++hand) {
        XrInteractionProfileState state = xrZero<XrInteractionProfileState>(XR_TYPE_INTERACTION_PROFILE_STATE);
        const XrResult result = xrGetCurrentInteractionProfile(session_, hands_[hand], &state);
        if (!XR_SUCCEEDED(result)) {
            logCtrl(instance_, "xrGetCurrentInteractionProfile", result);
            continue;
        }
        char path[XR_MAX_PATH_LENGTH] = "(none)";
        uint8_t wire = VIPLE_VR_CTRL_PROFILE_UNKNOWN;
        if (state.interactionProfile != XR_NULL_PATH) {
            uint32_t length = 0;
            const XrResult pathResult = xrPathToString(instance_, state.interactionProfile, sizeof(path), &length, path);
            if (!XR_SUCCEEDED(pathResult)) {
                logCtrl(instance_, "xrPathToString(interaction profile)", pathResult);
                std::snprintf(path, sizeof(path), "(none)");
            }
            else {
                // §QUEST-PCVR：子字串順序與 Frame 相同。touch_controller_plus 含 /touch_controller，結果是 TOUCH。
                if (std::strstr(path, "/touch_controller") != nullptr) {
                    wire = VIPLE_VR_CTRL_PROFILE_TOUCH;
                }
                else if (std::strstr(path, "/index_controller") != nullptr) {
                    wire = VIPLE_VR_CTRL_PROFILE_INDEX;
                }
                else if (std::strstr(path, "/frame_controller") != nullptr) {
                    wire = VIPLE_VR_CTRL_PROFILE_FRAME;
                }
                else {
                    wire = VIPLE_VR_CTRL_PROFILE_OTHER;
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            profileWire_[hand] = wire;
            std::snprintf(profilePath_[hand], sizeof(profilePath_[hand]), "%s", path);
        }
        QXR_LOGI("CTRL", "interaction profile %s: %s (wire profile %u)", handName(hand), path, static_cast<unsigned>(wire));
    }
}

void QxrXrControllers::logStatsIfDue()
{
    const uint64_t nowNs = monoNowNs();
    uint32_t poseN[2];
    uint32_t poseOk[2];
    uint32_t snapshots = 0;
    uint32_t haptic = 0;
    uint8_t wire[2];
    char path[2][XR_MAX_PATH_LENGTH];
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!statClockStarted_) {
            statClockStarted_ = true;
            statStartNs_ = nowNs;
            return;
        }
        if (nowNs - statStartNs_ < 10000000000ull) {
            return;
        }
        statStartNs_ = nowNs;
        for (int hand = 0; hand < 2; ++hand) {
            poseN[hand] = statPoseN_[hand];
            poseOk[hand] = statPoseOk_[hand];
            statPoseN_[hand] = 0;
            statPoseOk_[hand] = 0;
            wire[hand] = profileWire_[hand];
            std::snprintf(path[hand], sizeof(path[hand]), "%s", profilePath_[hand]);
        }
        snapshots = statSnapshots_;
        statSnapshots_ = 0;
        haptic = statHapticApplied_;
        statHapticApplied_ = 0;
        statHapticFailed_ = 0;
    }
    auto percent = [](uint32_t ok, uint32_t n) { return n == 0 ? 0.0 : 100.0 * static_cast<double>(ok) / static_cast<double>(n); };
    QXR_LOGI("CTRL",
             "profile L=%s wire=%u R=%s wire=%u poseValid L=%.1f%% (%u/%u) R=%.1f%% (%u/%u) snapshots=%u haptic=%u",
             path[0], static_cast<unsigned>(wire[0]), path[1], static_cast<unsigned>(wire[1]), percent(poseOk[0], poseN[0]),
             poseOk[0], poseN[0], percent(poseOk[1], poseN[1]), poseOk[1], poseN[1], snapshots, haptic);
}
