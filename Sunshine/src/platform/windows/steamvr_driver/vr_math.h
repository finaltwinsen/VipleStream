// vr_math.h - VipleStream §VR：driver 端（與 vr_probe unit 共用）的最小姿態數學（設計 §E.1、§E.2）。
//
// - 只用 double／float 純函式，不依賴 DirectXMath、OpenVR 或 Win32，vr_probe --mode unit 可以直接連。
// - fov_to_rect() 是 GetProjectionRaw、SetDisplayProjectionRaw 與 frame_compositor 四邊形投影的「唯一來源」
//   （drv-M-1；OpenVR 文件：GetProjectionRaw 的 bottom 與 top 是反的）。
// - 四元數一律 x, y, z, w（與 vr_ipc_abi.h 的 vripc_pose_t、VipleVr.h 的 Quat48 相同）。
#pragma once

#include <cmath>

namespace vrdrv::math {

  struct quat_t {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double w = 1.0;
  };

  struct vec3_t {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
  };

  // HmdRect2_t 的等價物（vTopLeft = (left, top)、vBottomRight = (right, bottom)）
  struct rect_t {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
  };

  // fov_tan[4] = { tan(left) 負, tan(right) 正, tan(up) 正, tan(down) 負 }（OpenXR 慣例，vr_ipc_abi.h config.fov_tan）
  // → GetProjectionRaw：left = fov[0]、right = fov[1]、top = fov[3]（tan(down)，負）、bottom = fov[2]（tan(up)，正）。
  inline rect_t fov_to_rect(const float fov[4]) {
    rect_t r;
    r.left = fov[0];
    r.right = fov[1];
    r.top = fov[3];
    r.bottom = fov[2];
    return r;
  }

  inline bool finite4(const float v[4]) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]) && std::isfinite(v[3]);
  }

  inline double dot(const quat_t &a, const quat_t &b) {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
  }

  inline quat_t normalize(const quat_t &q) {
    const double n = std::sqrt(dot(q, q));
    if (!(n > 1e-12) || !std::isfinite(n)) {
      return quat_t {};
    }
    return quat_t {q.x / n, q.y / n, q.z / n, q.w / n};
  }

  inline quat_t conj(const quat_t &q) {
    return quat_t {-q.x, -q.y, -q.z, q.w};
  }

  inline quat_t mul(const quat_t &a, const quat_t &b) {
    return quat_t {
      a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
      a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
      a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
      a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
  }

  // 兩個旋轉之間的角距離（度）；q 與 −q 視為相同
  inline double angle_deg(const quat_t &a, const quat_t &b) {
    double d = std::fabs(dot(normalize(a), normalize(b)));
    if (d > 1.0) {
      d = 1.0;
    }
    return 2.0 * std::acos(d) * 57.29577951308232;
  }

  // 3x4 列主序剛體矩陣（HmdMatrix34_t 的 m[3][4]）→ 旋轉四元數＋位置。不假設矩陣已正交化以外的任何事。
  inline void matrix34_to_pose(const float m[3][4], quat_t &q, vec3_t &p) {
    const double m00 = m[0][0], m01 = m[0][1], m02 = m[0][2];
    const double m10 = m[1][0], m11 = m[1][1], m12 = m[1][2];
    const double m20 = m[2][0], m21 = m[2][1], m22 = m[2][2];
    const double tr = m00 + m11 + m22;
    quat_t r;
    if (tr > 0.0) {
      const double s = std::sqrt(tr + 1.0) * 2.0;
      r.w = 0.25 * s;
      r.x = (m21 - m12) / s;
      r.y = (m02 - m20) / s;
      r.z = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
      const double s = std::sqrt(1.0 + m00 - m11 - m22) * 2.0;
      r.w = (m21 - m12) / s;
      r.x = 0.25 * s;
      r.y = (m01 + m10) / s;
      r.z = (m02 + m20) / s;
    } else if (m11 > m22) {
      const double s = std::sqrt(1.0 + m11 - m00 - m22) * 2.0;
      r.w = (m02 - m20) / s;
      r.x = (m01 + m10) / s;
      r.y = 0.25 * s;
      r.z = (m12 + m21) / s;
    } else {
      const double s = std::sqrt(1.0 + m22 - m00 - m11) * 2.0;
      r.w = (m10 - m01) / s;
      r.x = (m02 + m20) / s;
      r.y = (m12 + m21) / s;
      r.z = 0.25 * s;
    }
    q = normalize(r);
    p = vec3_t {m[0][3], m[1][3], m[2][3]};
  }

  // 旋轉四元數＋位置 → 3x4 列主序矩陣（SetDisplayEyeToHead 用）
  inline void pose_to_matrix34(const quat_t &qin, const vec3_t &p, float m[3][4]) {
    const quat_t q = normalize(qin);
    const double xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const double xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const double wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    m[0][0] = (float) (1.0 - 2.0 * (yy + zz));
    m[0][1] = (float) (2.0 * (xy - wz));
    m[0][2] = (float) (2.0 * (xz + wy));
    m[0][3] = (float) p.x;
    m[1][0] = (float) (2.0 * (xy + wz));
    m[1][1] = (float) (1.0 - 2.0 * (xx + zz));
    m[1][2] = (float) (2.0 * (yz - wx));
    m[1][3] = (float) p.y;
    m[2][0] = (float) (2.0 * (xz - wy));
    m[2][1] = (float) (2.0 * (yz + wx));
    m[2][2] = (float) (1.0 - 2.0 * (xx + yy));
    m[2][3] = (float) p.z;
  }

  // CRC-8（多項式 0x07，初值 0）：vr_probe scene 的角落位元圖案與 selftest T4 解碼共用
  inline unsigned char crc8(const unsigned char *data, unsigned len) {
    unsigned char c = 0;
    for (unsigned i = 0; i < len; ++i) {
      c ^= data[i];
      for (int b = 0; b < 8; ++b) {
        c = (unsigned char) ((c & 0x80) ? ((c << 1) ^ 0x07) : (c << 1));
      }
    }
    return c;
  }

}  // namespace vrdrv::math
