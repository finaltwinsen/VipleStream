/**
 * @file src/vr/vr_fec_spread.h
 * @brief VipleStream 2.0：client 回報的 FEC status（0x5502）的分類與 10 秒計數（純邏輯）。
 *
 * 只依賴標準函式庫，單元測試直接 include（tests/unit/test_vr_fec_spread.cpp）。
 *
 * 目前只有量測（§VR-REPAIR-RTT 的第一步）：每一份 FEC status 是「靠 FEC 還原成功」「收到的不夠、client 放棄了」還是
 * 「幀號跳號時 client 合成的那一份」，每 10 秒印成 [VIPLE-VR-FECSTAT]。位元率與 FEC 百分比的調整照舊只看回報裡的
 * 缺包數，不看這裡的分類。
 * §VR-FEC-SPREAD（小幀的 parity 補滿並延後送）要用到同一個分類，它的規則之後也放這個檔。
 *
 * FEC 升高期間的實效記帳（也只有量測）：FEC 跟著救不回的幀升到高於基準之後，多送的 parity 有沒有真的救到 block、
 * 放棄的 block 是不是再高一級就救得回（account_status、episode_t）。要回答的是「升高之後能不能提早降回來」。
 */
#pragma once

// standard includes
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <string>

namespace vr::fecspread {
  /// client 回報的 FEC status 屬於哪一種
  enum class status_e : uint8_t {
    none,  ///< data 全到（不該回報）或欄位不合理
    recovered,  ///< 缺了 data、靠 parity 還原了
    failed,  ///< 收到的 shard 不夠還原（client 放棄這個 block）
    /// client 依幀號跳號合成的回報（收到的欄位都是 0）。每次沒收齊就換幀都有一份：收到一部分才放棄的那一幀也有
    /// （它另外有一份 failed），而連續好幾幀沒到只有一份——所以它的份數不是「一個封包都沒到的幀數」
    gap,
  };

  /**
   * @param total_data,total_parity 這個 block 的 data／parity shard 數
   * @param recv_data,recv_parity   client 收到的 data／parity shard 數
   */
  inline status_e classify_status(unsigned total_data, unsigned total_parity, unsigned recv_data, unsigned recv_parity) {
    if (total_data == 0 || recv_data > total_data || recv_parity > total_parity) {
      return status_e::none;
    }
    if (recv_data == 0 && recv_parity == 0) {
      return status_e::gap;
    }
    if (recv_data >= total_data) {
      return status_e::none;
    }
    return recv_data + recv_parity >= total_data ? status_e::recovered : status_e::failed;
  }

  /**
   * @brief 打包端依 FEC 百分比給一個 block 的 parity shard 數：無條件進位，不到 min_parity 時補到 min_parity
   *        （data＋parity 不超過 255）。是 stream.cpp 的 fec::encode 那條公式的複本，只給記帳用；打包端改公式時要一起改。
   */
  inline unsigned parity_for(unsigned data, int pct, unsigned min_parity) {
    if (data == 0 || pct <= 0) {
      return 0;
    }
    unsigned parity = (data * (unsigned) pct + 99) / 100;
    if (parity < min_parity) {
      parity = data < 255 ? std::min(min_parity, 255u - data) : 0;
    }
    return parity;
  }

  /// 一份 FEC status 相對於「基準 FEC」與「再高一級」的位置（量測）
  struct status_use_t {
    status_e st = status_e::none;
    /// 還原成功，而且缺的 data 片數超過基準 FEC 會送的 parity 片數：基準一定救不回，升高的那一截確實用到了。
    /// 是下界——缺的片數沒超過時，也要基準的 parity 剛好都到才救得回
    bool elevated_use = false;
    /// client 放棄的 block，但 FEC 再高一級、多送的 parity 全到就夠還原。是上界——多送的也可能跟著掉
    bool near_miss = false;
  };

  /**
   * @param base_pct 設定的基準 FEC 百分比
   * @param next_pct 再升一級之後的百分比（已經在上限時傳目前的值：沒有下一級，near_miss 一律不成立）
   * @param min_parity client 要求的每個 block 最少 parity 數（minRequiredFecPackets）
   */
  inline status_use_t account_status(unsigned total_data, unsigned total_parity, unsigned recv_data, unsigned recv_parity,
                                     int base_pct, int next_pct, unsigned min_parity) {
    status_use_t use;
    use.st = classify_status(total_data, total_parity, recv_data, recv_parity);
    if (use.st == status_e::recovered) {
      use.elevated_use = total_data - recv_data > parity_for(total_data, base_pct, min_parity);
    } else if (use.st == status_e::failed) {
      const unsigned next_parity = parity_for(total_data, next_pct, min_parity);
      const unsigned short_by = total_data - recv_data - recv_parity;  // failed：一定 > 0
      use.near_miss = next_parity > total_parity && short_by <= next_parity - total_parity;
    }
    return use;
  }

  /// 一個視窗（10 秒）裡各種 FEC status 的數量。不是 thread-safe，由呼叫端加鎖。
  struct status_stats_t {
    uint32_t recovered = 0;  ///< 靠 FEC 還原成功的 block 數
    uint32_t recovered_shards = 0;  ///< 這些 block 一共還原了幾個 data shard
    uint32_t recovered_max = 0;  ///< 單一 block 最多還原幾個
    uint32_t failed = 0;  ///< 收到的不夠還原、client 放棄的 block 數
    uint32_t gap = 0;  ///< client 合成的跳號回報份數（log 的 wholeFrame；不是整幀沒到的幀數，見 status_e::gap）
    // 以下兩個由呼叫端依 account_status() 的結果累計（add() 與 line() 不管它們）
    uint32_t elevated_use = 0;  ///< recovered 之中 status_use_t::elevated_use 成立的
    uint32_t near_miss = 0;  ///< failed 之中 status_use_t::near_miss 成立的

    void add(unsigned total_data, unsigned total_parity, unsigned recv_data, unsigned recv_parity) {
      switch (classify_status(total_data, total_parity, recv_data, recv_parity)) {
        case status_e::recovered:
          {
            const uint32_t shards = total_data - recv_data;
            ++recovered;
            recovered_shards += shards;
            recovered_max = std::max(recovered_max, shards);
            break;
          }
        case status_e::failed:
          ++failed;
          break;
        case status_e::gap:
          ++gap;
          break;
        case status_e::none:
          break;
      }
    }

    bool any() const {
      return recovered != 0 || failed != 0 || gap != 0;
    }

    /// [VIPLE-VR-FECSTAT] 10s 行的前半段
    std::string line() const {
      return std::format("recovered={} (shards={} max={}) failed={} wholeFrame={}", recovered, recovered_shards, recovered_max, failed, gap);
    }
  };

  /**
   * @brief FEC 高於基準的一段期間（從第一次升高到降回基準）的實效記帳（量測）。不是 thread-safe，由呼叫端加鎖。
   *
   * 升高期間收到的 FEC status 才算；期間沒有任何 recovered／failed ＝ 多送的 parity 完全沒有派上用場。
   */
  struct episode_t {
    using clock = std::chrono::steady_clock;

    bool active = false;
    int peak_pct = 0;  ///< 這段期間到過的最高百分比
    clock::time_point start {};
    clock::time_point last_use {};  ///< 最後一次 elevated_use 的時刻（還沒有過＝預設值）
    uint32_t recovered = 0;  ///< 期間靠 FEC 還原成功的 block 數
    uint32_t elevated_use = 0;  ///< 其中基準 FEC 一定救不回的（見 status_use_t::elevated_use）
    uint32_t failed = 0;  ///< 期間 client 放棄的 block 數
    uint32_t fixable_failed = 0;  ///< 其中再高一級就救得回的（見 status_use_t::near_miss）
    uint32_t nack_holds = 0;  ///< 期間因為補包（NACK）而把回降往後延的次數

    void on_status(const status_use_t &use, clock::time_point now) {
      if (!active) {
        return;
      }
      if (use.st == status_e::recovered) {
        ++recovered;
        if (use.elevated_use) {
          ++elevated_use;
          last_use = now;
        }
      } else if (use.st == status_e::failed) {
        ++failed;
        if (use.near_miss) {
          ++fixable_failed;
        }
      }
    }

    void on_nack_hold() {
      if (active) {
        ++nack_holds;
      }
    }

    /// 到目前為止的累計（10 秒行與變動行共用）。active 才有意義
    std::string summary(clock::time_point now) const {
      const auto secs = [now](clock::time_point t) {
        return std::chrono::duration<double>(now - t).count();
      };
      const double dur_s = secs(start);
      const std::string last_use_str = last_use == clock::time_point {} ? std::string {"-"} : std::format("{:.1f}s", secs(last_use));
      return std::format("peak={}% dur={:.1f}s elevatedUse={}/{} fixableFailed={}/{} lastUse={} nackHolds={}", peak_pct, dur_s,
                         elevated_use, recovered, fixable_failed, failed, last_use_str, nack_holds);
    }

    /**
     * @brief FEC 百分比變動之後呼叫（升或降都要）。
     * @return 要印的一行；空字串＝不印（這段期間剛開始，還沒有東西可報）。降回基準時回傳的是結算（尾端有 ended），之後清空
     */
    std::string on_fec_changed(int pct, int base_pct, clock::time_point now) {
      if (!active) {
        if (pct > base_pct) {
          *this = {};
          active = true;
          peak_pct = pct;
          start = now;
        }
        return {};
      }
      peak_pct = std::max(peak_pct, pct);
      const bool ended = pct <= base_pct;
      const std::string sum = summary(now);
      const char *const tail = ended ? " (ended)" : "";
      std::string line = std::format("fec={}% {}{}", pct, sum, tail);
      if (ended) {
        *this = {};
      }
      return line;
    }
  };
}  // namespace vr::fecspread
