/**
 * @file src/platform/windows/steamvr_driver/hand_skeleton.cpp
 * @brief §VR-SKELETON：手部骨架（改寫自 Valve OpenVR SDK 範例 handskeletonsimulation，BSD 3-Clause；見 hand_skeleton.h）。
 */
#include "hand_skeleton.h"

#include <cmath>
#include <utility>

namespace vrdrv::hand {

  namespace {
    constexpr float k_pi = 3.14159265358979f;

    constexpr float deg(float d) {
      return d * k_pi / 180.0f;
    }

    struct quat_t {
      float w, x, y, z;
    };

    struct vec3_t {
      float x, y, z;
    };

    constexpr quat_t k_identity {1.0f, 0.0f, 0.0f, 0.0f};

    quat_t mul(const quat_t &a, const quat_t &b) {
      return {a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z, a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
              a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
    }

    /// v 依 q 旋轉（q·v·q*）
    vec3_t rotate(const vec3_t &v, const quat_t &q) {
      const quat_t r = mul(mul(q, quat_t {0.0f, v.x, v.y, v.z}), quat_t {q.w, -q.x, -q.y, -q.z});
      return {r.x, r.y, r.z};
    }

    quat_t swing_twist(float s0, float s1, float twist) {
      const float sq = s0 * s0 + s1 * s1;
      const float ct = std::cos(twist * 0.5f);
      const float st = std::sin(twist * 0.5f);
      if (sq > 0.0f) {
        const float th = std::sqrt(sq);
        const float cs = std::cos(th * 0.5f);
        const float k = std::sin(th * 0.5f) / th;
        return {cs * ct, cs * st, s1 * ct * k - s0 * st * k, s0 * ct * k + s1 * st * k};
      }
      return {ct, st, s1 * ct * 0.5f - s0 * st * 0.5f, s0 * ct * 0.5f + s1 * st * 0.5f};
    }

    /// 範例的 HmdQuaternion_FromEulerAngles(roll, 0, 0)：繞 z 軸
    quat_t bend(float roll) {
      return {std::cos(roll * 0.5f), 0.0f, 0.0f, std::sin(roll * 0.5f)};
    }

    struct joint_t {
      float s0 = 0.0f, s1 = 0.0f, twist = 0.0f;
    };

    struct finger_t {
      joint_t metacarpal, proximal;
      float intermediate = 0.0f, distal = 0.0f;
    };

    // 每節大約的長度（公尺）：拇指、食指、中指、無名指、小指
    constexpr float k_len[5][5] = {
      {0.05f, 0.05f, 0.035f, 0.025f, 0.0f},
      {0.03f, 0.073f, 0.045f, 0.025f, 0.02f},
      {0.01f, 0.091f, 0.049f, 0.03f, 0.02f},
      {0.02f, 0.073f, 0.045f, 0.03f, 0.03f},
      {0.03f, 0.067f, 0.03f, 0.025f, 0.02f},
    };

    void put(bool right, const quat_t &q, const vec3_t &p, vr::VRBoneTransform_t &out) {
      out.orientation.w = q.w;
      out.orientation.x = q.x;
      out.orientation.y = q.y;
      out.orientation.z = q.z;
      // 兩手的「上」軸相反：以左手為準算，右手把位移的 x 反過來
      out.position.v[0] = right ? -p.x : p.x;
      out.position.v[1] = p.y;
      out.position.v[2] = p.z;
      out.position.v[3] = 1.0f;
    }

    void put_len(bool right, const quat_t &q, float len, vr::VRBoneTransform_t &out) {
      put(right, q, {len, 0.0f, 0.0f}, out);
    }

    /// 掌骨：FBX 轉座標時根骨頭轉了、子骨頭反轉回來，程式產生的掌骨要多轉一個固定量（範例的說明）
    void put_metacarpal(bool right, const quat_t &q, float len, vr::VRBoneTransform_t &out) {
      quat_t o = mul(quat_t {0.5f, 0.5f, -0.5f, 0.5f}, q);
      const vec3_t p = rotate({len, 0.0f, 0.0f}, o);
      if (right) {
        std::swap(o.w, o.x);
        std::swap(o.y, o.z);
        o.x = -o.x;
        o.z = -o.z;
      }
      put(right, o, p, out);
    }
  }  // namespace

  void compute(bool right, const curls_t &c, vr::VRBoneTransform_t *out) {
    // 0 root：對齊 /pose/raw
    put(false, k_identity, {0.0f, 0.0f, 0.0f}, out[0]);
    // 1 wrist：範例取自 Index 控制器相對 /pose/raw 的位置
    out[1].position.v[0] = right ? 0.034038f : -0.034038f;
    out[1].position.v[1] = 0.036503f;
    out[1].position.v[2] = 0.164722f;
    out[1].position.v[3] = 1.0f;
    out[1].orientation.w = -0.055147f;
    out[1].orientation.x = -0.078608f;
    out[1].orientation.y = right ? 0.920279f : -0.920279f;
    out[1].orientation.z = right ? -0.379296f : 0.379296f;

    // 預設的張開手
    finger_t f[4];
    for (auto &x : f) {
      x.proximal.s1 = deg(10.0f);
      x.intermediate = deg(5.0f);
      x.distal = deg(5.0f);
    }
    f[0].metacarpal.s1 = deg(13.0f);
    f[1].metacarpal.s1 = deg(0.0f);
    f[2].metacarpal.s1 = deg(-15.0f);
    f[3].metacarpal.s1 = deg(-27.0f);
    f[0].proximal.s1 = deg(3.0f);
    f[1].proximal.s1 = deg(0.0f);
    f[2].proximal.s1 = deg(-1.0f);
    f[3].proximal.s1 = deg(-2.0f);

    // 拇指（跟其他手指不一樣）
    joint_t t_meta {deg(10.0f) + deg(c.thumb * 5.0f), deg(40.0f), 0.0f};
    joint_t t_prox {deg(c.thumb * 90.0f), 0.0f, 0.0f};
    const float t_distal = deg(c.thumb * 90.0f);

    const float fc[4] = {c.index, c.middle, c.ring, c.pinky};
    for (int i = 0; i < 4; ++i) {
      f[i].metacarpal.s0 += deg(fc[i] * 5.0f);
      f[i].proximal.s0 += deg(fc[i] * 90.0f);
      f[i].intermediate += deg(fc[i] * 80.0f);
      f[i].distal += deg(fc[i] * 80.0f);
    }

    // 2..5 拇指
    put_metacarpal(right, swing_twist(t_meta.s0, t_meta.s1, t_meta.twist), k_len[0][0], out[2]);
    put_len(right, swing_twist(t_prox.s0, t_prox.s1, t_meta.twist), k_len[0][1], out[3]);
    put_len(right, bend(t_distal), k_len[0][2], out[4]);
    put_len(right, k_identity, k_len[0][3], out[5]);

    // 6..25 食指、中指、無名指、小指（各 5 根）
    for (int i = 0; i < 4; ++i) {
      vr::VRBoneTransform_t *b = out + 6 + i * 5;
      put_metacarpal(right, swing_twist(f[i].metacarpal.s0, f[i].metacarpal.s1, f[i].metacarpal.twist), k_len[i + 1][0], b[0]);
      put_len(right, swing_twist(f[i].proximal.s0, f[i].proximal.s1, f[i].proximal.twist), k_len[i + 1][1], b[1]);
      put_len(right, bend(f[i].intermediate), k_len[i + 1][2], b[2]);
      put_len(right, bend(f[i].distal), k_len[i + 1][3], b[3]);
      put_len(right, k_identity, k_len[i + 1][4], b[4]);
    }

    // 26..30 aux（範例沒填）：放原點、不旋轉。只有少數 app 用它們做指尖的方向，給一個合法值即可
    for (uint32_t i = 26; i < k_bone_count; ++i) {
      put(false, k_identity, {0.0f, 0.0f, 0.0f}, out[i]);
    }
  }

}  // namespace vrdrv::hand
