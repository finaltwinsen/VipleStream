/**
 * @file src/vr/vr_multilink_logic.h
 * @brief VipleStream 2.0 §VR-MULTILINK：多連線的純邏輯（去重視窗、飽和偵測、故障注入、TLV 編解碼）。
 *
 * 只依賴標準函式庫與 VipleVr.h，單元測試直接 include（tests/unit/test_vr_multilink.cpp）。
 * socket、執行緒與加密在 vr_multilink.h／.cpp。
 *
 * 2026-10-06 的連線層實測（docs/steam_frame_client.md §8.7）決定了這裡的兩個規則：
 *   - 兩條連線共用頭盔的無線電，不是互相獨立：往一條送不動的鏈路硬送影像，會把另一條也拖垮
 *     （家用 Wi-Fi 送達率 99.8% → 91.5%）。所以鏈路送不動時要暫停在它上面送影像（saturation_t）。
 *   - Windows 對卡住的 Wi-Fi 網卡做阻塞式 UDP 送出，一次可以卡 10 秒。所以每條連線自己的佇列與
 *     執行緒、非阻塞送出、過期就丟。
 */
#pragma once

// standard includes
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// lib includes
#include <moonlight-common-c/src/VipleVr.h>

namespace vr::multilink {
  constexpr int kMaxLinks = VIPLE_VR_LINK_MAX;

  /**
   * @brief DATA 訊息的去重與防重放：64 格滑動視窗。同一則訊息在每條連線各送一份、seq 相同，
   *        先到的那份被接受，其餘（以及重放的舊封包）被擋掉。
   *
   * fresh() 不改狀態：必須在解密成功之後才 mark()，否則偽造的封包可以毒化視窗。
   */
  struct replay_window_t {
    uint32_t top = 0;
    uint64_t bits = 0;
    bool any = false;

    bool fresh(uint32_t seq) const {
      if (!any) {
        return true;
      }
      const int32_t d = (int32_t) (seq - top);
      if (d > 0) {
        return true;
      }
      if (d <= -64) {
        return false;
      }
      return !(bits & (1ull << (-d)));
    }

    void mark(uint32_t seq) {
      if (!any) {
        any = true;
        top = seq;
        bits = 1;
        return;
      }
      const int32_t d = (int32_t) (seq - top);
      if (d > 0) {
        bits = d >= 64 ? 0 : bits << d;
        bits |= 1;
        top = seq;
      } else if (d > -64) {
        bits |= 1ull << (-d);
      }
    }
  };

  /**
   * @brief 送出飽和偵測。每個影像批次回報一次「在期限內送完了沒」；1 s 視窗內至少 24 個批次、
   *        其中 40% 以上沒送完，就建議暫停在這條連線送影像（all：第一次 2 s，連續發生時加倍到 30 s，
   *        健康滿 10 s 後退避歸零；auto：退回探測，由量測決定何時恢復）。暫停期間連線仍保持
   *        （ping、追蹤、音訊照送）。
   *
   * 門檻來自 2026-10-07 在遊玩位置的實測：適配器那條每秒約有一次 50～100 ms（偶爾 150～200 ms）的停頓，
   * 停頓時送出會卡住、那段時間的批次會過期，但它整體送達 99.98%，是兩條裡比較好的一條——這種「短停頓」
   * 不能判成送不動（1 s 裡過期的不到 25%）。真的送不動的鏈路（另一個位置：PHY 只有 17～50 Mbps）
   * 幾乎每一批都送不完，1 s 內就會被判定。
   *
   * 只在這條連線的送出執行緒呼叫。是否真的暫停由呼叫端決定（最後一條可用的連線不暫停）。
   */
  struct saturation_t {
    using clock = std::chrono::steady_clock;
    static constexpr std::chrono::milliseconds kWindow {1000};
    static constexpr int kMinBatches = 24;
    static constexpr int kBadPct = 40;
    static constexpr std::chrono::milliseconds kHoldMin {2000};
    static constexpr std::chrono::milliseconds kHoldMax {30000};
    static constexpr std::chrono::milliseconds kHealthyReset {10000};

    clock::time_point win_start {};
    int ok = 0;
    int bad = 0;
    std::chrono::milliseconds hold {0};
    clock::time_point healthy_since {};
    bool healthy = false;
    int last_ok_pct = 100;  ///< 上一個結算視窗裡「期限內送完」的批次比例

    /// @return true＝這個視窗判定為飽和（建議暫停）。呼叫端決定暫停時再呼叫 next_hold()。
    bool report(bool sent_ok, clock::time_point now) {
      if (win_start == clock::time_point {}) {
        win_start = now;
      }
      (sent_ok ? ok : bad)++;
      if (now - win_start < kWindow) {
        return false;
      }
      const int total = ok + bad;
      last_ok_pct = total > 0 ? ok * 100 / total : 100;
      const bool saturated = total >= kMinBatches && bad * 100 >= total * kBadPct;
      if (saturated) {
        healthy = false;
      } else if (total >= kMinBatches) {
        if (!healthy) {
          healthy = true;
          healthy_since = win_start;
        } else if (now - healthy_since >= kHealthyReset) {
          hold = std::chrono::milliseconds {0};
        }
      }
      ok = bad = 0;
      win_start = now;
      return saturated;
    }

    /// 重新開始統計（連線剛從探測轉為送影像）：視窗與健康狀態歸零，退避時間保留。
    void restart() {
      win_start = clock::time_point {};
      ok = bad = 0;
      healthy = false;
      healthy_since = clock::time_point {};
    }

    /// 決定暫停時呼叫：回傳這次要暫停多久，並把下一次的時間加倍。
    std::chrono::milliseconds next_hold() {
      hold = hold.count() == 0 ? kHoldMin : std::min(hold * 2, kHoldMax);
      return hold;
    }
  };

  /**
   * @brief `vr_multilink = auto` 的恢復閘門。暫停送影像的連線每秒只送一批影像當探測；client 把這批封包到達的
   *        速率放在 PING 裡回報（burstSeq 每量到一次加一）。連續 kNeed 次新的量測都 ≥ 需求，才恢復在這條連線送影像。
   *
   * 只看「新的量測」：PING 每 250 ms 一個、探測每秒一批，同一個量測會被回報好幾次。
   * 只在 hub 的接收執行緒呼叫。
   */
  struct probe_gate_t {
    static constexpr int kNeed = 2;

    /// 上一次「夠快」之後又送了超過這麼多批探測、卻沒有新的夠快量測：那些探測沒量到（封包沒到齊），
    /// 之前的成績作廢。用「送了幾批探測」而不是時間來算，探測間隔怎麼變都成立；2＝容許回報晚一批。
    static constexpr uint32_t kMaxProbeGap = 2;

    /// 放行前探測封包至少要送到這個比例。到達速率只說明「到的那些封包來得多快」：鏈路在掉包時，到的那一半
    /// 照樣量得出很高的速率（2026-10-07 配戴實測：量到 151 Mbps 放行，一送影像就掉 46%）。
    static constexpr uint32_t kDelivPct = 85;
    static constexpr uint32_t kDelivMinPkts = 32;  ///< 送出不到這麼多就判不出比例，不擋

    uint8_t last_seq = 0;
    bool have_seq = false;
    int good = 0;
    uint32_t last_good_probe = 0;
    bool have_base = false;
    uint32_t tx0 = 0;
    uint32_t rx0 = 0;

    /// @param probes_sent server 到目前為止在這條連線送了幾批探測
    /// @param need_good 要連續幾次合格的量測（見 probe_need_good）
    /// @param tx_pkts／rx_pkts server 在這條連線累計送出的影像封包數、client 在 PING 回報的累計收到數（低 32 bit）
    /// @return true＝已經連續 need_good 次量到足夠的速率、成績還沒過期，而且這段探測期間的封包確實送到了
    bool on_report(uint8_t burst_seq, uint32_t burst_mbps, uint32_t need_mbps, uint32_t probes_sent = 0, int need_good = kNeed,
                   uint32_t tx_pkts = 0, uint32_t rx_pkts = 0) {
      if (!have_base) {
        have_base = true;
        tx0 = tx_pkts;
        rx0 = rx_pkts;
      }
      if (good > 0 && probes_sent - last_good_probe > kMaxProbeGap) {
        good = 0;  // 同一個 seq 被重複回報時也要檢查：不能拿很久以前的量測放行
      }
      if (have_seq && burst_seq == last_seq) {
        return good >= need_good;
      }
      have_seq = true;
      last_seq = burst_seq;
      if (burst_mbps >= need_mbps) {
        ++good;
        last_good_probe = probes_sent;
      } else {
        good = 0;
      }
      if (good >= need_good) {
        // 速率夠了，再看這段探測期間送出的封包有沒有到：沒到齊就從頭來（基準移到現在，下一輪只看之後的）
        const uint32_t dtx = tx_pkts - tx0;
        const uint32_t drx = rx_pkts - rx0;
        if (dtx >= kDelivMinPkts && (uint64_t) drx * 100 < (uint64_t) dtx * kDelivPct) {
          good = 0;
          tx0 = tx_pkts;
          rx0 = rx_pkts;
        }
      }
      return good >= need_good;
    }

    void reset() {
      good = 0;
      have_base = false;
    }

    /// 退回探測時用：歸零，並把「已看過的量測」對齊到當下，退回之前（還在送影像時）量到的那一筆不算
    void reset(uint8_t current_seq, uint32_t tx_pkts = 0, uint32_t rx_pkts = 0) {
      good = 0;
      have_seq = true;
      last_seq = current_seq;
      have_base = true;
      tx0 = tx_pkts;
      rx0 = rx_pkts;
    }
  };

  /// 一條連線剛放行不久（kQuickFail 內）就又被判送不動，算一次「放行失敗」：下一次要更多次連續合格的量測
  /// （2、4、8 次；探測每秒一批，最多多等約 8 秒），免得每隔幾秒就把畫面拖下去一次。撐過 kQuickFail 就歸零。
  constexpr int64_t kProbeQuickFailNs = 10'000'000'000;
  constexpr int kProbeMaxStrikes = 2;

  inline int probe_need_good(int strikes) {
    return probe_gate_t::kNeed << std::clamp(strikes, 0, kProbeMaxStrikes);
  }

  /**
   * @brief 影像位元率的近期峰值（保持 kHold）。恢復門檻要用它算：斷訊時 ABR 會把位元率砍到很低，
   *        拿那一刻的位元率當門檻（1.5 倍、最低 50 Mbps）等於沒有門檻，而鏈路一恢復 ABR 就把位元率拉回去。
   */
  struct peak_rate_t {
    static constexpr int64_t kHoldNs = 30'000'000'000;
    uint32_t peak = 0;
    int64_t at_ns = 0;

    uint32_t update(uint32_t mbps, int64_t now_ns) {
      if (mbps >= peak || now_ns - at_ns > kHoldNs) {
        peak = mbps;
        at_ns = now_ns;
      }
      return peak;
    }
  };

  /**
   * @brief 一條連線的實際送達率：client 在 PING 回報的累計收到數 ÷ server 的累計送出數，約每秒結算一次。
   *        經路由器的那條連線，host 的網卡永遠送得出去，丟包發生在後面的 Wi-Fi——只有這個數字看得出來。
   *        pct＝-1 表示還不知道（剛建立、剛恢復，或這一秒幾乎沒送）。只在 hub 的接收執行緒呼叫。
   */
  struct delivery_t {
    uint64_t tx0 = 0;
    uint32_t rx0 = 0;
    int64_t t0 = 0;
    bool init = false;
    int pct = -1;

    void on_ping(uint64_t tx_pkts, uint32_t client_rx_pkts, int64_t now_ns) {
      if (!init) {
        init = true;
        tx0 = tx_pkts;
        rx0 = client_rx_pkts;
        t0 = now_ns;
        return;
      }
      if (now_ns - t0 < 1'000'000'000) {
        return;
      }
      const uint64_t dtx = tx_pkts - tx0;
      const uint32_t drx = client_rx_pkts - rx0;  // 32 bit 繞回也正確
      pct = dtx >= 500 ? (int) std::min<uint64_t>(100, (uint64_t) drx * 100 / dtx) : -1;
      tx0 = tx_pkts;
      rx0 = client_rx_pkts;
      t0 = now_ns;
    }
  };

  /// 恢復送影像需要量到的速率：目前影像位元率的 1.5 倍，至少 50 Mbps（位元率還沒量到時也適用）
  inline uint32_t probe_need_mbps(uint32_t video_mbps) {
    return std::max<uint32_t>(50, video_mbps + video_mbps / 2);
  }

  /**
   * @brief 開發用故障注入：`vr_multilink_fault = <linkId>:<on_ms>:<off_ms>[:<phase_ms>][,…]`。
   *        每個週期（on＋off）裡的 off 期間，這條連線的收與送全部丟棄，用來在單一網卡上模擬鏈路空檔。
   *        另一種項目 `r<linkId>:<mbps>`：把這條連線的影像送出限制在這個速率（逐封包排隊），模擬送不動的慢鏈路。
   *        `c:<on_ms>:<off_ms>[:<phase_ms>]`：off 期間 ENet 控制通道上的 VR 即時訊息（LOSS、LATCH、REFRESH_START）
   *        全部丟掉，模擬 session 位址那條鏈路變弱（連線上的資料報不受影響）。
   */
  struct fault_t {
    struct rule_t {
      int on_ms = 0;
      int off_ms = 0;
      int phase_ms = 0;

      bool blocked(int64_t now_ms) const {
        const int64_t period = (int64_t) on_ms + off_ms;
        if (period <= 0 || off_ms <= 0) {
          return false;
        }
        const int64_t t = (((now_ms + phase_ms) % period) + period) % period;
        return t >= on_ms;
      }
    };

    std::array<std::optional<rule_t>, kMaxLinks + 1> rules {};
    std::optional<rule_t> ctrl;  ///< c:… 規則（ENet 上的 VR 即時訊息）

    /// b<linkId>:<period_ms>:<pkts>：這條連線每隔 period_ms 連續掉 pkts 個影像封包（只算第一次送出的，補包不受影響）。
    /// 模擬 Wi-Fi 一次聚合框的損失——比 FEC 補得起來的多、又不到整幀
    struct burst_t {
      int period_ms = 0;
      int pkts = 0;
    };
    std::array<burst_t, kMaxLinks + 1> burst {};
    std::array<int, kMaxLinks + 1> rate_mbps {};  ///< 0＝不限速
    std::array<int, kMaxLinks + 1> delay_ms {};  ///< 0＝不延遲；這條連線的每一批晚這麼久才送

    bool any() const {
      if (ctrl) {
        return true;
      }
      for (const auto &r : rules) {
        if (r) {
          return true;
        }
      }
      for (const int r : rate_mbps) {
        if (r > 0) {
          return true;
        }
      }
      for (const int d : delay_ms) {
        if (d > 0) {
          return true;
        }
      }
      for (const auto &b : burst) {
        if (b.pkts > 0) {
          return true;
        }
      }
      return false;
    }

    burst_t burst_rule(int link_id) const {
      return link_id >= 0 && link_id <= kMaxLinks ? burst[link_id] : burst_t {};
    }

    int delay(int link_id) const {
      return link_id >= 0 && link_id <= kMaxLinks ? delay_ms[link_id] : 0;
    }

    int rate_cap(int link_id) const {
      return link_id >= 0 && link_id <= kMaxLinks ? rate_mbps[link_id] : 0;
    }

    bool blocked(int link_id, int64_t now_ms) const {
      if (link_id < 0 || link_id > kMaxLinks || !rules[link_id]) {
        return false;
      }
      return rules[link_id]->blocked(now_ms);
    }

    bool ctrl_blocked(int64_t now_ms) const {
      return ctrl && ctrl->blocked(now_ms);
    }

    /// 解析失敗回 nullopt（呼叫端警告後當成沒設）
    static std::optional<fault_t> parse(std::string_view spec) {
      fault_t f;
      while (!spec.empty()) {
        const auto comma = spec.find(',');
        std::string_view item = spec.substr(0, comma);
        spec = comma == std::string_view::npos ? std::string_view {} : spec.substr(comma + 1);
        if (!item.empty() && item.front() == 'b') {
          // b<linkId>:<period_ms>:<pkts>
          item.remove_prefix(1);
          int v[3] = {0, 0, 0};
          int n = 0;
          while (n < 3 && !item.empty()) {
            const auto colon = item.find(':');
            const std::string_view tok = item.substr(0, colon);
            item = colon == std::string_view::npos ? std::string_view {} : item.substr(colon + 1);
            const auto res = std::from_chars(tok.data(), tok.data() + tok.size(), v[n]);
            if (res.ec != std::errc {} || res.ptr != tok.data() + tok.size()) {
              return std::nullopt;
            }
            ++n;
          }
          if (n != 3 || !item.empty() || v[0] < 1 || v[0] > kMaxLinks || v[1] < 10 || v[1] > 60000 || v[2] < 1 || v[2] > 4000) {
            return std::nullopt;
          }
          f.burst[v[0]] = burst_t {v[1], v[2]};
          continue;
        }
        if (item.size() >= 2 && item[0] == 'c' && item[1] == ':') {
          // c:<on_ms>:<off_ms>[:<phase_ms>]
          item.remove_prefix(2);
          int v[3] = {0, 0, 0};
          int n = 0;
          while (n < 3 && !item.empty()) {
            const auto colon = item.find(':');
            const std::string_view tok = item.substr(0, colon);
            item = colon == std::string_view::npos ? std::string_view {} : item.substr(colon + 1);
            const auto res = std::from_chars(tok.data(), tok.data() + tok.size(), v[n]);
            if (res.ec != std::errc {} || res.ptr != tok.data() + tok.size()) {
              return std::nullopt;
            }
            ++n;
          }
          if (n < 2 || !item.empty() || v[0] < 0 || v[1] < 0 || v[0] + v[1] <= 0) {
            return std::nullopt;
          }
          f.ctrl = rule_t {v[0], v[1], v[2]};
          continue;
        }
        if (!item.empty() && (item.front() == 'r' || item.front() == 'd')) {
          // r<linkId>:<mbps>（限速）或 d<linkId>:<ms>（延遲，1～40：再久就超過每條連線 1 MB 的佇列上限，變成丟包）
          const char kind = item.front();
          item.remove_prefix(1);
          const auto colon = item.find(':');
          int id = 0;
          int mbps = 0;
          if (colon == std::string_view::npos) {
            return std::nullopt;
          }
          const auto a = std::from_chars(item.data(), item.data() + colon, id);
          const auto b = std::from_chars(item.data() + colon + 1, item.data() + item.size(), mbps);
          if (a.ec != std::errc {} || a.ptr != item.data() + colon || b.ec != std::errc {} || b.ptr != item.data() + item.size() ||
              id < 1 || id > kMaxLinks || mbps < 1 || mbps > (kind == 'd' ? 40 : 100000)) {
            return std::nullopt;
          }
          (kind == 'd' ? f.delay_ms : f.rate_mbps)[id] = mbps;
          continue;
        }
        int v[4] = {0, 0, 0, 0};
        int n = 0;
        while (n < 4 && !item.empty()) {
          const auto colon = item.find(':');
          const std::string_view tok = item.substr(0, colon);
          item = colon == std::string_view::npos ? std::string_view {} : item.substr(colon + 1);
          const auto res = std::from_chars(tok.data(), tok.data() + tok.size(), v[n]);
          if (res.ec != std::errc {} || res.ptr != tok.data() + tok.size()) {
            return std::nullopt;
          }
          ++n;
        }
        if (n < 3 || !item.empty() || v[0] < 1 || v[0] > kMaxLinks || v[1] < 0 || v[2] < 0 || v[1] + v[2] <= 0) {
          return std::nullopt;
        }
        f.rules[v[0]] = rule_t {v[1], v[2], v[3]};
      }
      return f;
    }
  };

  // ── §VR-LINK-REPAIR：補包 ─────────────────────────────────────────
  //
  // server 保留最近幾個 FEC block 已加密的封包。client 回報「正在等的那個 block 手上有哪些 shard」（NACK），
  // server 決定補哪些，把倉裡的封包原樣再送一次——位元組與第一次完全相同（同一個 IV、同一份密文），client 的接收路徑
  // 不必知道它是補的。絕不能改任何位元組後沿用原 IV 重新加密（AES-GCM 的 nonce 不可重複用在不同的明文）。

  /// 影像執行緒交給 hub 的一批封包屬於哪一幀、哪個 FEC block 的哪幾個 shard
  struct video_meta_t {
    uint32_t frame = 0;
    uint8_t block = 0;  ///< FEC block 序號（0 起）
    uint16_t first_shard = 0;  ///< 這一批的第一個封包是 block 裡的第幾個 shard
    uint16_t data_shards = 0;  ///< 這個 block 的 data shard 數
    uint16_t total_shards = 0;  ///< data＋parity
  };

  using shard_bits_t = std::array<uint8_t, 32>;  ///< 256 個 shard 的位元圖

  inline bool bit_get(const shard_bits_t &b, unsigned i) {
    return i < 256 && ((b[i >> 3] >> (i & 7)) & 1);
  }

  inline void bit_set(shard_bits_t &b, unsigned i) {
    if (i < 256) {
      b[i >> 3] |= (uint8_t) (1u << (i & 7));
    }
  }

  /**
   * @brief 封包倉：最近 kBlocks 個 FEC block 的封包，環狀覆寫。只存 total ≤ 255 的 block（不做 FEC 的超大幀
   *        一個 block 可以到上千個 shard，位元圖描述不了，也不該補）。不是 thread-safe，由呼叫端加鎖。
   */
  class packet_store_t {
  public:
    static constexpr size_t kBlocks = 24;  ///< 一般幀 1～2 個 block：至少約 12 幀

    struct block_t {
      bool used = false;
      uint32_t frame = 0;
      uint8_t block = 0;
      uint16_t data = 0;
      uint16_t total = 0;
      uint32_t pkt_size = 0;
      int64_t first_ns = 0;  ///< 第一批進來的時間
      uint8_t rounds = 0;  ///< 已經補過幾輪
      int64_t last_round_ns = 0;  ///< 上一輪補包的時間（0＝還沒補過）
      shard_bits_t resent {};  ///< 上一輪補了哪些（可能還在路上）
      shard_bits_t stored {};  ///< 倉裡有的 shard
      std::vector<uint8_t> bytes;  ///< total × pkt_size

      const uint8_t *pkt(unsigned shard) const {
        return bytes.data() + (size_t) shard * pkt_size;
      }
    };

    /// @return 有沒有存進去（meta 不合理、或是不該補的 block 時回 false）
    bool put(const video_meta_t &m, const uint8_t *pkts, size_t pkt_size, size_t count, int64_t now_ns) {
      if (pkt_size == 0 || count == 0 || m.total_shards == 0 || m.total_shards > 255 || m.data_shards == 0 || m.data_shards > m.total_shards ||
          (size_t) m.first_shard + count > m.total_shards) {
        return false;
      }
      block_t *b = find(m.frame, m.block);
      if (b && (b->total != m.total_shards || b->data != m.data_shards || b->pkt_size != pkt_size)) {
        b = nullptr;  // 同一個（幀, block）卻對不上：當成新的（幀號繞回一圈才可能）
      }
      if (!b) {
        b = &blocks_[next_];
        next_ = (next_ + 1) % kBlocks;
        b->used = true;
        b->frame = m.frame;
        b->block = m.block;
        b->data = m.data_shards;
        b->total = m.total_shards;
        b->pkt_size = (uint32_t) pkt_size;
        b->first_ns = now_ns;
        b->rounds = 0;
        b->last_round_ns = 0;
        b->resent.fill(0);
        b->stored.fill(0);
        b->bytes.resize((size_t) m.total_shards * pkt_size);  // 容量會留著重複使用
      }
      std::memcpy(b->bytes.data() + (size_t) m.first_shard * pkt_size, pkts, count * pkt_size);
      for (size_t i = 0; i < count; ++i) {
        bit_set(b->stored, (unsigned) (m.first_shard + i));
      }
      last_ = b;
      return true;
    }

    block_t *find(uint32_t frame, uint8_t block) {
      if (last_ && last_->used && last_->frame == frame && last_->block == block) {
        return last_;  // 同一個 block 的下一批：最常見
      }
      for (auto &b : blocks_) {
        if (b.used && b.frame == frame && b.block == block) {
          return &b;
        }
      }
      return nullptr;
    }

  private:
    std::array<block_t, kBlocks> blocks_ {};
    size_t next_ = 0;
    block_t *last_ = nullptr;
  };

  /// 解析後的 NACK
  struct nack_t {
    uint32_t frame = 0;
    uint8_t block = 0;
    uint8_t need = 0;
    uint8_t total = 0;  ///< 0＝client 一個封包都沒收到（沒有位元圖）
    uint8_t attempt = 0;
    shard_bits_t have {};
  };

  /// 解析 0x5507/0B 的 TLV 本體：VIPLE_VR_TLV_NACK＋have[(total+7)/8]。長度不夠回 false。
  inline bool parse_nack(const uint8_t *body, size_t len, nack_t &out) {
    VIPLE_VR_TLV_NACK n;
    if (body == nullptr || len < sizeof(n)) {
      return false;
    }
    std::memcpy(&n, body, sizeof(n));
    const size_t bytes = ((size_t) n.total + 7) / 8;
    if (len < sizeof(n) + bytes) {
      return false;
    }
    out = nack_t {};
    out.frame = n.frame;
    out.block = n.block;
    out.need = n.need;
    out.total = n.total;
    out.attempt = n.attempt;
    std::memcpy(out.have.data(), body + sizeof(n), bytes);
    // 最後一個 byte 裡超出 total 的位元不算
    if (n.total % 8 != 0 && bytes > 0) {
      out.have[bytes - 1] &= (uint8_t) ((1u << (n.total % 8)) - 1);
    }
    return true;
  }

  /// 同一個 block 最多補幾輪。client 一般回報 3 次；鏈路整個斷一小段時，空檔裡的回報不算次數、恢復後再報（最多多 2 次）
  inline constexpr unsigned kRepairMaxRounds = 5;

  /**
   * @brief 決定補哪些 shard（回傳 shard 序號，依送出順序）。空＝不用補（client 其實已經夠還原，或倉裡沒有它缺的）。
   *
   * 補到「夠還原」再多一點點餘裕就好（補包自己也可能掉）：client 一湊滿 data 個 shard 就做 Reed-Solomon 還原並往下走，
   * 之後才到的全被當成過期丟掉——多送的只是白佔空中時間與補包額度。先挑缺的 data shard（序號小的在前）：每補回一個
   * data，頭盔要還原的就少一個。只會挑倉裡有的 shard（還沒送出的不算缺）。
   *
   * @param data,total server 記的這個 block 的 data／全部 shard 數
   * @param stored     倉裡有的 shard
   * @param in_flight  剛補過、可能還在路上的 shard（上一輪補包距今不到一個往返時間時才給）：當成 client 會收到，
   *                   不再補一次——client 等不及而提早再報的那一份，位元圖還沒反映出路上的補包
   * @param needed     （輸出，可為 nullptr）補之前還差幾個 shard 才夠還原＝這份回報代表的缺包數（呼叫端記 ABR 的帳用）
   */
  inline std::vector<uint8_t> plan_repair(const nack_t &n, unsigned data, unsigned total, const shard_bits_t &stored, const shard_bits_t *in_flight = nullptr, unsigned *needed = nullptr) {
    std::vector<uint8_t> out;
    if (needed) {
      *needed = 0;
    }
    if (total == 0 || total > 255 || data == 0 || data > total) {
      return out;
    }
    // client 的 total 和我們的對不上（包含 0＝它什麼都沒收到）：位元圖不可信，當成全部都缺
    const bool trust = n.total == total;
    const auto has = [&](unsigned i) {
      return (trust && bit_get(n.have, i)) || (in_flight && bit_get(*in_flight, i));
    };
    const auto missing = [&](unsigned i) {
      return !has(i) && bit_get(stored, i);
    };
    unsigned have = 0;
    for (unsigned i = 0; i < total; ++i) {
      have += has(i) ? 1 : 0;
    }
    if (have >= data) {
      return out;  // 已經夠還原（這份回報過期了）
    }
    const unsigned need = data - have;
    if (needed) {
      *needed = need;
    }
    // data 的序號在前、parity 在後：照序號挑就是 data 優先，缺的 data 不在倉裡（還沒送出）時自然用 parity 湊
    const unsigned want = need + std::max<unsigned>(2, need / 8);
    for (unsigned i = 0; i < total && out.size() < want; ++i) {
      if (missing(i)) {
        out.push_back((uint8_t) i);
      }
    }
    return out;
  }

  /**
   * @brief 補包額度（token bucket，以封包計）：每秒補進「影像封包率 × kPct%」，最多存 kBurst 個。
   *        額度用完代表鏈路已經在大量掉包，這時加碼重送只會更擠——改由 FEC 與 refresh 處理，位元率交給 ABR。
   */
  struct repair_budget_t {
    static constexpr unsigned kPct = 15;
    /// 兩個最大的單一 block 各補一次再留一點（FEC 10% 時一個 block 最多 231 個 data → 一次 259 個、兩次 518）：
    /// 鏈路斷一小段、連續兩整幀沒到時兩幀都補得起。400 的時候 72／80 Hz 或高位元率（每幀超過約 185 個 data）只補得起
    /// 第一幀，第二幀拿不到額度——多送了一整幀補包、refresh 還是照走。長期上限仍是 kPct
    static constexpr double kBurst = 600;

    double tokens = kBurst;
    int64_t last_ns = 0;

    bool take(unsigned pkts, double video_pps, int64_t now_ns) {
      if (last_ns != 0 && now_ns > last_ns) {
        tokens = std::min(kBurst, tokens + video_pps * kPct / 100.0 * (double) (now_ns - last_ns) / 1e9);
      }
      last_ns = now_ns;
      if (tokens < (double) pkts) {
        return false;
      }
      tokens -= (double) pkts;
      return true;
    }
  };

  // ── LINK_HELLO／LINK_READY ─────────────────────────────────────────

  struct hello_link_t {
    uint8_t id = 0;
    std::array<uint8_t, 4> client_addr {};
    std::array<uint8_t, 4> server_addr {};
    uint16_t client_video_port = 0;
    uint16_t client_audio_port = 0;
    uint8_t flags = 0;  ///< VIPLE_VR_LINK_F_*：client 支援的連線層功能

    bool operator==(const hello_link_t &) const = default;
  };

  struct ready_link_t {
    uint8_t id = 0;
    uint8_t status = VIPLE_VR_LINK_READY_REFUSED;
    uint16_t server_video_port = 0;
    uint16_t server_audio_port = 0;
  };

  /// 解析 LINK_HELLO 的 TLV 本體：{u8 count, VIPLE_VR_LINK_DESC[count]}。
  /// 長度不符、count 超過上限、linkId 超出範圍或重複、埠為 0 都回 false。
  inline bool parse_hello(const uint8_t *body, size_t len, std::vector<hello_link_t> &out) {
    out.clear();
    if (len < 1) {
      return false;
    }
    const size_t count = body[0];
    if (count == 0 || count > (size_t) kMaxLinks || len < 1 + count * sizeof(VIPLE_VR_LINK_DESC)) {
      return false;
    }
    bool seen[kMaxLinks + 1] = {};
    for (size_t i = 0; i < count; ++i) {
      VIPLE_VR_LINK_DESC d;
      std::memcpy(&d, body + 1 + i * sizeof(d), sizeof(d));
      if (d.linkId < 1 || d.linkId > kMaxLinks || seen[d.linkId] || d.clientVideoPort == 0 || d.clientAudioPort == 0) {
        out.clear();
        return false;
      }
      seen[d.linkId] = true;
      hello_link_t h;
      h.id = d.linkId;
      std::memcpy(h.client_addr.data(), d.clientAddr, 4);
      std::memcpy(h.server_addr.data(), d.serverAddr, 4);
      h.client_video_port = d.clientVideoPort;  // 兩端都是 little-endian 平台（VipleVr.h 的前提）
      h.client_audio_port = d.clientAudioPort;
      h.flags = d.flags;
      out.push_back(h);
    }
    return true;
  }

  /// 組 LINK_READY 的 TLV（含 subtype 與 len）。
  /// @param hub_caps server 同意啟用的連線層功能（VIPLE_VR_LINK_F_*）；0＝不附（位元組與舊版完全相同）
  inline std::vector<uint8_t> format_ready_tlv(const std::vector<ready_link_t> &links, uint8_t hub_caps = 0) {
    std::vector<uint8_t> tlv;
    const size_t n = std::min<size_t>(links.size(), kMaxLinks);
    const size_t extra = hub_caps != 0 ? 1 : 0;
    tlv.resize(2 + 1 + n * sizeof(VIPLE_VR_LINK_PORTS) + extra);
    tlv[0] = VIPLE_VR_S2C_LINK_READY;
    tlv[1] = (uint8_t) (1 + n * sizeof(VIPLE_VR_LINK_PORTS) + extra);
    if (extra) {
      tlv.back() = hub_caps;
    }
    tlv[2] = (uint8_t) n;
    for (size_t i = 0; i < n; ++i) {
      VIPLE_VR_LINK_PORTS p {};
      p.linkId = links[i].id;
      p.status = links[i].status;
      p.serverVideoPort = links[i].server_video_port;
      p.serverAudioPort = links[i].server_audio_port;
      std::memcpy(tlv.data() + 3 + i * sizeof(p), &p, sizeof(p));
    }
    return tlv;
  }
}  // namespace vr::multilink
