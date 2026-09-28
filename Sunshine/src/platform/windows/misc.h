/**
 * @file src/platform/windows/misc.h
 * @brief Miscellaneous declarations for Windows.
 */
#pragma once

// standard includes
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

// platform includes
#include <Windows.h>
#include <winnt.h>

namespace platf {
  void print_status(const std::string_view &prefix, HRESULT status);
  HDESK syncThreadDesktop();

  int64_t qpc_counter();

  /**
   * @brief QueryPerformanceFrequency 的值（每秒 QPC tick 數）。
   * @details M1b S1-01：開機後固定不變，第一次呼叫時取值並快取（函式區域 static，初始化執行緒安全）。
   *          兩台實測機都是 10 MHz，但程式一律不假設這個值。
   * @return 每秒 tick 數；取不到時回 0（Windows XP 起不會發生）。
   */
  int64_t qpc_frequency();

  /**
   * @brief 把 QPC tick 數（通常是兩個 QPC 值的差）換成 ns，全程整數運算。
   * @details (t / f) * 1e9 + (t % f) * 1e9 / f：不經 double。|ticks| 約 ≤ 9.2e9 × f（10 MHz 時約 292 年）
   *          時結果精確；超出就對稱飽和到 ±INT64_MAX，任何輸入都不會有有號溢位的 UB。餘數項要求
   *          f < 9.2e9（實際的 QPF 遠小於此）。
   *          C++ 整數除法向 0 截斷、飽和也對稱，所以 qpc_ticks_to_ns(-t) == -qpc_ticks_to_ns(t)。
   *          頻率取不到（0）時回 0。
   * @param ticks QPC tick 數，可為負。
   * @return 對應的 ns 數。
   */
  int64_t qpc_ticks_to_ns(int64_t ticks);

  /**
   * @brief 把 100 ns 單位的系統相對時間換成 QPC tick。
   * @details M1b S1-01（ops-m3）：WGC 的 Direct3D11CaptureFrame::SystemRelativeTime() 是 TimeSpan
   *          （100 ns 單位、QPC 時基），不是 raw QPC tick；QPF 恰好是 10 MHz 時兩者數值相同，
   *          這裡明確換算，不再依賴這個巧合。整數公式同 qpc_ticks_to_ns()，超出範圍同樣對稱飽和到
   *          ±INT64_MAX（f == 1e7 時，未飽和的範圍內結果恆等於輸入；只有 |t100ns| 距 INT64_MAX 不到約 1e7
   *          的極端值才會飽和）。
   * @param t100ns 100 ns 單位的時間。
   * @return QPC tick。
   */
  int64_t qpc_from_100ns(int64_t t100ns);

  /**
   * @brief 兩個 QPC 值的時間差（performance_counter1 − performance_counter2）。
   * @details M1b S1-01（K9）：舊版公式寫成 Δtick × 頻率 / 1e9，方向反了（10 MHz 時小 10⁴ 倍，
   *          所有由它換算的 frame_timestamp 實際上都 ≈ 換算當下）。現在 = qpc_ticks_to_ns(Δtick)；
   *          簽章不變。差值以 unsigned 相減後轉回 int64，再經 qpc_ticks_to_ns() 的飽和，整條路徑對任何輸入
   *          都沒有有號溢位的 UB；極端輸入會得到 ±INT64_MAX ns（差值超出 int64 時則是回繞後、無意義的值）。
   *          呼叫端做 `steady_now - age` 之前仍要自己檢查範圍（例如 §B.8 第 5 條把 present_qpc 限制在
   *          now ± 1 s）。
   */
  std::chrono::nanoseconds qpc_time_difference(int64_t performance_counter1, int64_t performance_counter2);

  /**
   * @brief Get file version information from a Windows executable or driver file.
   * @param file_path Path to the file to query.
   * @param version_str Output parameter for version string in format "major.minor.build.revision".
   * @return true if version info was successfully extracted, false otherwise.
   */
  bool getFileVersionInfo(const std::filesystem::path &file_path, std::string &version_str);
}  // namespace platf
