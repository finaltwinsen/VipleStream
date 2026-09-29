// pose_history.cpp - VipleStream §VR（M1b S2-06）：見 pose_history.h
#include "pose_history.h"

#include <cmath>

namespace vrdrv {

  namespace {
    constexpr double k_pi = 3.14159265358979323846;
  }

  math::vec3_t pose_history_t::rotate(const math::quat_t &q, const math::vec3_t &v) {
    // v' = q · (0, v) · q*
    const math::quat_t p {v.x, v.y, v.z, 0.0};
    const math::quat_t r = math::mul(math::mul(q, p), math::conj(q));
    return {r.x, r.y, r.z};
  }

  ph_pose_t pose_history_t::compose(const ph_pose_t &a, const ph_pose_t &b) {
    // (Ra, pa) · (Rb, pb) = (Ra Rb, pa + Ra pb)
    ph_pose_t o;
    o.rot = math::normalize(math::mul(a.rot, b.rot));
    const math::vec3_t rb = rotate(a.rot, b.pos);
    o.pos = {a.pos.x + rb.x, a.pos.y + rb.y, a.pos.z + rb.z};
    return o;
  }

  ph_pose_t pose_history_t::inverse(const ph_pose_t &a) {
    // (R, p)⁻¹ = (R*, −R* p)
    ph_pose_t o;
    o.rot = math::conj(math::normalize(a.rot));
    const math::vec3_t rp = rotate(o.rot, a.pos);
    o.pos = {-rp.x, -rp.y, -rp.z};
    return o;
  }

  double pose_history_t::pos_dist(const math::vec3_t &a, const math::vec3_t &b) {
    const double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
  }

  ph_pose_t pose_history_t::extrapolate(const ph_sample_t &s, double dt_s) {
    ph_pose_t o;
    o.pos = {s.pose.pos.x + s.lin_vel.x * dt_s, s.pose.pos.y + s.lin_vel.y * dt_s, s.pose.pos.z + s.lin_vel.z * dt_s};
    // world-space 角速度：q(t) = exp(ω dt) · q
    const double wx = s.ang_vel.x, wy = s.ang_vel.y, wz = s.ang_vel.z;
    const double mag = std::sqrt(wx * wx + wy * wy + wz * wz);
    const double theta = mag * dt_s;
    if (!std::isfinite(theta) || mag < 1e-9) {
      o.rot = math::normalize(s.pose.rot);
      return o;
    }
    const double h = 0.5 * theta;
    const double k = std::sin(h) / mag;
    const math::quat_t dq {wx * k, wy * k, wz * k, std::cos(h)};
    o.rot = math::normalize(math::mul(dq, s.pose.rot));
    return o;
  }

  void pose_history_t::to_euler_deg(const math::quat_t &qin, double &yaw, double &pitch, double &roll) {
    // OpenVR 座標（Y 上、−Z 前）：yaw 繞 Y、pitch 繞 X、roll 繞 Z
    const math::quat_t q = math::normalize(qin);
    const double sinp = 2.0 * (q.w * q.x - q.y * q.z);
    pitch = std::abs(sinp) >= 1.0 ? std::copysign(90.0, sinp) : std::asin(sinp) * 180.0 / k_pi;
    yaw = std::atan2(2.0 * (q.w * q.y + q.x * q.z), 1.0 - 2.0 * (q.x * q.x + q.y * q.y)) * 180.0 / k_pi;
    roll = std::atan2(2.0 * (q.w * q.z + q.x * q.y), 1.0 - 2.0 * (q.x * q.x + q.z * q.z)) * 180.0 / k_pi;
  }

  void pose_history_t::reset() {
    AcquireSRWLockExclusive(&lock_);
    for (auto &s : ring_) {
      s = ph_sample_t {};
    }
    head_ = 0;
    delta_cur_ = ph_pose_t {};
    delta_identity_ = true;
    pending_n_ = 0;
    pending_head_ = 0;
    last_event_qpc_ = 0;
    last_event_name_.clear();
    ReleaseSRWLockExclusive(&lock_);
  }

  void pose_history_t::record(const ph_sample_t &s) {
    AcquireSRWLockExclusive(&lock_);
    ring_[head_ % k_slots] = s;
    ++head_;
    ReleaseSRWLockExclusive(&lock_);
  }

  void pose_history_t::note_event(const char *name, int64_t qpc) {
    AcquireSRWLockExclusive(&lock_);
    last_event_qpc_ = qpc;
    last_event_name_ = name ? name : "?";
    ReleaseSRWLockExclusive(&lock_);
  }

  ph_pose_t pose_history_t::delta() const {
    AcquireSRWLockShared(&lock_);
    const ph_pose_t d = delta_cur_;
    ReleaseSRWLockShared(&lock_);
    return d;
  }

  ph_match_t pose_history_t::match(const ph_pose_t &app_pose, int64_t t_app_qpc, int64_t now_qpc) {
    ph_match_t m;
    AcquireSRWLockExclusive(&lock_);
    const double f = qpf_ > 0 ? (double) qpf_ : 1e7;
    const int64_t win = (int64_t) (k_candidate_window_s * f);

    int best_i = -1, near_i = -1;
    double best_err = 1e9, near_dt = 1e18;
    ph_pose_t best_delta {}, near_delta {};
    const uint32_t n = head_ < k_slots ? head_ : k_slots;
    for (uint32_t i = 0; i < n; ++i) {
      const ph_sample_t &c = ring_[i];
      if (c.sample_id == 0 || c.reported_qpc > now_qpc || now_qpc - c.reported_qpc > win) {
        continue;
      }
      // 外插起點＝SteamVR 實際採用的時間 reported + offset（clamp 後）
      const double t_start = (double) c.reported_qpc + c.offset_s * f;
      const double dt = ((double) t_app_qpc - t_start) / f;
      const ph_pose_t pc = extrapolate(c, dt);
      const ph_pose_t dc = compose(app_pose, inverse(pc));  // Δ_c = mHmdPose · P̂_c⁻¹
      const double err = math::angle_deg(dc.rot, delta_cur_.rot);
      if (err < best_err || (err == best_err && best_i >= 0 && (int32_t) (c.sample_id - ring_[best_i].sample_id) > 0)) {
        best_err = err;
        best_i = (int) i;
        best_delta = dc;
      }
      const double adt = std::abs(dt);
      if (adt < near_dt) {
        near_dt = adt;
        near_i = (int) i;
        near_delta = dc;
      }
    }

    if (best_i >= 0) {
      m.have_candidate = true;
      m.delta_err_deg = best_err;
      if (best_err < k_hit_deg) {
        m.hit = true;
        m.echo = ring_[best_i].sample_id;
        pending_n_ = 0;  // 目前 Δ 仍然正確：候選緩衝作廢
      } else {
        m.echo = ring_[near_i].sample_id;
        // 新 Δ 候選：時間最近候選的 Δ_c 與它的 HMD 角速度
        const auto &w = ring_[near_i].ang_vel;
        pending_[pending_head_ % k_adopt_window] = pending_t {near_delta, std::sqrt(w.x * w.x + w.y * w.y + w.z * w.z) * 180.0 / k_pi};
        ++pending_head_;
        if (pending_n_ < k_adopt_window) {
          ++pending_n_;
        }
        if (pending_n_ == k_adopt_window) {
          bool consistent = true;
          double max_speed = 0.0;
          for (uint32_t a = 0; a < k_adopt_window && consistent; ++a) {
            max_speed = pending_[a].ang_speed_deg > max_speed ? pending_[a].ang_speed_deg : max_speed;
            for (uint32_t b = a + 1; b < k_adopt_window; ++b) {
              if (math::angle_deg(pending_[a].delta.rot, pending_[b].delta.rot) >= k_consistent_deg ||
                  pos_dist(pending_[a].delta.pos, pending_[b].delta.pos) >= k_consistent_m) {
                consistent = false;
                break;
              }
            }
          }
          const bool recent_event = last_event_qpc_ != 0 && now_qpc - last_event_qpc_ <= (int64_t) (k_event_window_s * f);
          // 等速轉動＋app 晚交幀會變成穩定的假偏移（yaw30、延遲 44 ms ≈ 1.3°）：只在幾乎靜止或剛收到重置事件時採用
          if (consistent && (max_speed < k_still_deg_per_s || recent_event)) {
            delta_cur_ = pending_[(pending_head_ - 1) % k_adopt_window].delta;
            delta_identity_ = math::angle_deg(delta_cur_.rot, math::quat_t {}) < 1e-3 && pos_dist(delta_cur_.pos, math::vec3_t {}) < 1e-4;
            m.adopted = true;
            m.adopted_trigger = (max_speed < k_still_deg_per_s) ? std::string("still") : ("event:" + last_event_name_);
            m.adopted_delta = delta_cur_;
            pending_n_ = 0;
          }
        }
      }
    }

    // renderPose(client 空間) = Δ_cur⁻¹ · mHmdPose
    m.client_pose = compose(inverse(delta_cur_), app_pose);
    m.delta_nonidentity = !delta_identity_;
    ReleaseSRWLockExclusive(&lock_);
    return m;
  }

}  // namespace vrdrv
