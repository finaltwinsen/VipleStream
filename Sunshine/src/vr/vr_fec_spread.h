/**
 * @file src/vr/vr_fec_spread.h
 * @brief VipleStream 2.0：client 回報的 FEC status（0x5502）的分類與 10 秒計數（純邏輯）。
 *
 * 只依賴標準函式庫，單元測試直接 include（tests/unit/test_vr_fec_spread.cpp）。
 *
 * 目前只有量測（§VR-REPAIR-RTT 的第一步）：每一份 FEC status 是「靠 FEC 還原成功」「收到的不夠、client 放棄了」還是
 * 「整幀沒到」，每 10 秒印成 [VIPLE-VR-FECSTAT]。位元率與 FEC 百分比的調整照舊只看回報裡的缺包數，不看這裡的分類。
 * §VR-FEC-SPREAD（小幀的 parity 補滿並延後送）要用到同一個分類，它的規則之後也放這個檔。
 */
#pragma once

// standard includes
#include <algorithm>
#include <cstdint>
#include <format>
#include <string>

namespace vr::fecspread {
  /// client 回報的 FEC status 屬於哪一種
  enum class status_e : uint8_t {
    none,  ///< data 全到（不該回報）或欄位不合理
    recovered,  ///< 缺了 data、靠 parity 還原了
    failed,  ///< 收到的 shard 不夠還原（client 放棄這個 block）
    gap,  ///< 一個 shard 都沒收到（client 依幀號跳號合成的回報）
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

  /// 一個視窗（10 秒）裡各種 FEC status 的數量。不是 thread-safe，由呼叫端加鎖。
  struct status_stats_t {
    uint32_t recovered = 0;  ///< 靠 FEC 還原成功的 block 數
    uint32_t recovered_shards = 0;  ///< 這些 block 一共還原了幾個 data shard
    uint32_t recovered_max = 0;  ///< 單一 block 最多還原幾個
    uint32_t failed = 0;  ///< 收到的不夠還原、client 放棄的 block 數
    uint32_t gap = 0;  ///< 整幀沒到（client 合成的回報）的份數

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
}  // namespace vr::fecspread
