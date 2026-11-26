/**
 * @file src/vr/viple_build_id.h
 * @brief §VRDUAL-7A：編譯期 build id（git HEAD 前 12 hex，工作樹有追蹤變更則標 dirty）。
 *
 * 放在既有的 hello.reserved[0]／welcome.reserved[0]（uint64_t）。0＝對方這欄沒填（舊版）。
 * 不改結構大小、不改 ABI 表的鍵、不加巨集到 vr_ipc_abi.h。不一致只記 log，握手不中止。
 *
 * 編碼：bit 63＝有填、bit 62＝dirty、bits 0–47＝HEAD 前 12 個十六進位數字。
 * 產生檔 viple_build_id_gen.h 由 cmake 或 Build-SteamVRDriver.ps1 寫入建置目錄。
 * 沒有產生檔（或 git 失敗）時本標頭自己定義 present＋hash 0，兩邊都這樣就視為相同。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

#if defined(__has_include)
#  if __has_include("viple_build_id_gen.h")
#    include "viple_build_id_gen.h"
#  endif
#endif

#ifndef VIPLE_BUILD_ID_PACKED
#  define VIPLE_BUILD_ID_PACKED 0x8000000000000000ull
#  define VIPLE_BUILD_ID_TEXT "unknown"
#endif

namespace viple_build_id {
  inline constexpr std::uint64_t k_packed = VIPLE_BUILD_ID_PACKED;
  inline constexpr std::uint64_t k_present_bit = 1ull << 63;
  inline constexpr std::uint64_t k_dirty_bit = 1ull << 62;
  inline constexpr std::uint64_t k_hash_mask = 0x0000FFFFFFFFFFFFull;

  /// 讓連結器留住字串（driver 有 /OPT:REF）。握手路徑會呼叫。
#if defined(_MSC_VER)
  __declspec(noinline)
#else
  __attribute__((noinline))
#endif
  inline const char *
  embedded_tag() {
    return "VIPLE_BID:" VIPLE_BUILD_ID_TEXT;
  }

  inline bool reported(std::uint64_t v) {
    return (v & k_present_bit) != 0;
  }

  /// 兩邊都是 0（舊版沒填）不算不一致。一邊有填、一邊是 0 才算。git 失敗的 unknown 對 unknown 算相同。
  inline bool same(std::uint64_t a, std::uint64_t b) {
    if (a == 0 && b == 0) {
      return true;
    }
    if (!reported(a) || !reported(b)) {
      return false;
    }
    const std::uint64_t mask = k_present_bit | k_dirty_bit | k_hash_mask;
    return (a & mask) == (b & mask);
  }

  /// unreported、或 12 hex、或 12 hex 接 -dirty。hash 0 且有填印 000000000000（git 讀不到）。
  inline void format(std::uint64_t v, char *out, std::size_t n) {
    if (out == nullptr || n == 0) {
      return;
    }
    if (!reported(v)) {
      std::snprintf(out, n, "unreported");
      return;
    }
    const unsigned long long hash = static_cast<unsigned long long>(v & k_hash_mask);
    std::snprintf(out, n, "%012llx%s", hash, (v & k_dirty_bit) != 0 ? "-dirty" : "");
  }
}  // namespace viple_build_id
