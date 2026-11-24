#pragma once

// §QUEST-VR：把 Qt vrsynthetic 的頭部軌跡移植過來。數學跟 Qt 逐式相同，host 兩邊的 log 才能對。
// 不帶 Qt 的 dev 軌跡（揮手、拋球、視線、選單、puppet）。

#include "qxr_pose_source.h"

#include <cstdint>
#include <string_view>

enum class QxrSyntheticMotion {
    Still = 0,
    Sine = 1,
    Yaw30 = 2,
    Tilt30Yaw = 3,
    Fast = 4,
};

class QxrSyntheticPose final : public QxrPoseSource {
public:
    // 認不得的字串用 Sine，並記一行 log。空字串也一樣。
    explicit QxrSyntheticPose(std::string_view motion);

    bool sample(VIPLE_VR_TRACKING* out, uint64_t nowUs) override;

    const char* motionName() const;

private:
    QxrSyntheticMotion motion = QxrSyntheticMotion::Sine;
    // 第一筆樣本才抓住 t＝0。nowUs 可能是 0，不能拿它當「還沒開始」。
    bool started = false;
    uint64_t startUs = 0;
};
