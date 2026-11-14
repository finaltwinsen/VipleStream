/**
 * @file src/logging.h
 * @brief Declarations for logging related functions.
 */
#pragma once

// standard includes
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

// lib includes
#include <boost/log/common.hpp>
#include <boost/log/sinks.hpp>

using text_sink = boost::log::sinks::asynchronous_sink<boost::log::sinks::text_ostream_backend>;

extern boost::log::sources::severity_logger<int> verbose;
extern boost::log::sources::severity_logger<int> debug;
extern boost::log::sources::severity_logger<int> info;
extern boost::log::sources::severity_logger<int> warning;
extern boost::log::sources::severity_logger<int> error;
extern boost::log::sources::severity_logger<int> fatal;
#ifdef SUNSHINE_TESTS
extern boost::log::sources::severity_logger<int> tests;
#endif

#include "config.h"
#include "stat_trackers.h"

/**
 * @brief Handles the initialization and deinitialization of the logging system.
 */
namespace logging {
  class deinit_t {
  public:
    /**
     * @brief A destructor that restores the initial state.
     */
    ~deinit_t();
  };

  /**
   * @brief Deinitialize the logging system.
   * @examples
   * deinit();
   * @examples_end
   */
  void deinit();

  void formatter(const boost::log::record_view &view, boost::log::formatting_ostream &os);

  /**
   * @brief §PERF-1113-R2（A14）：log 檔的輪替與 stdout 鏡像。全部用預設值時和原本的行為相同
   *        （啟動時截斷、執行中不設限、每一行也寫到 stdout）；server 模式由 main.cpp 填。
   */
  struct file_options_t {
    /// 單一 log 檔的大小上限（位元組），0＝不輪替。大於 0 時：啟動時把上一輪的檔留成 `<檔名>.1`
    /// （再前一輪是 `.2`），執行中寫到上限也照樣輪替一次。很短的一輪（小於 64 KB 與上限四分之一的較小者，
    /// 例如啟動就失敗、連續重啟）改收進 `<檔名>.0`（累積，最多 1 MB），不佔 `.1`／`.2`。
    /// POSIX 上另有 `<檔名>.lock`：別的行程正在寫同一個檔時不輪替。
    std::uintmax_t max_bytes = 0;
    /// 每次建立新的 log 檔之前呼叫（Windows 用它先以受保護的安全描述元建檔，否則新檔會繼承 Users 可讀）。
    /// 執行中輪替時是在 log sink 的執行緒上呼叫，裡面不可以寫 log。可為空。
    std::function<void(const std::string &path)> prepare_file;
    /// 執行中輪替之後新檔的第一行（版本資訊；部署流程會看 log 開頭的 version 行）。空字串＝不寫。
    std::string first_line;
    /// true＝log 檔開成功時不再把每一行也寫到 stdout。服務模式的 stdout 是 viplestream-svc.log：
    /// 每一行寫兩個檔、各 flush 一次，而且那個檔跨多次 server 重啟一直累積、沒有上限。
    bool no_stdout_mirror = false;
  };

  /**
   * @brief Initialize the logging system.
   * @param min_log_level The minimum log level to output.
   * @param log_file The log file to write to.
   * @param file_options 輪替與 stdout 鏡像的設定（見 file_options_t）。
   * @return An object that will deinitialize the logging system when it goes out of scope.
   * @examples
   * log_init(2, "sunshine.log");
   * @examples_end
   */
  [[nodiscard]] std::unique_ptr<deinit_t> init(int min_log_level, const std::string &log_file, const file_options_t &file_options = {});

  /**
   * @brief Setup AV logging.
   * @param min_log_level The log level.
   */
  void setup_av_logging(int min_log_level);

  /**
   * @brief Setup logging for libdisplaydevice.
   * @param min_log_level The log level.
   */
  void setup_libdisplaydevice_logging(int min_log_level);

  /**
   * @brief Flush the log.
   * @examples
   * log_flush();
   * @examples_end
   */
  void log_flush();

  /**
   * @brief Print help to stdout.
   * @param name The name of the program.
   * @examples
   * print_help("sunshine");
   * @examples_end
   */
  void print_help(const char *name);

  /**
   * @brief A helper class for tracking and logging numerical values across a period of time
   * @examples
   * min_max_avg_periodic_logger<int> logger(debug, "Test time value", "ms", 5s);
   * logger.collect_and_log(1);
   * // ...
   * logger.collect_and_log(2);
   * // after 5 seconds
   * logger.collect_and_log(3);
   * // In the log:
   * // [2024:01:01:12:00:00]: Debug: Test time value (min/max/avg): 1ms/3ms/2.00ms
   * @examples_end
   */
  template<typename T>
  class min_max_avg_periodic_logger {
  public:
    min_max_avg_periodic_logger(boost::log::sources::severity_logger<int> &severity, std::string_view message, std::string_view units, std::chrono::seconds interval_in_seconds = std::chrono::seconds(20)):
        severity(severity),
        message(message),
        units(units),
        interval(interval_in_seconds),
        enabled(config::sunshine.min_log_level <= severity.default_severity()) {
    }

    void collect_and_log(const T &value) {
      if (enabled) {
        auto print_info = [&](const T &min_value, const T &max_value, double avg_value) {
          auto f = stat_trackers::two_digits_after_decimal();
          if constexpr (std::is_floating_point_v<T>) {
            BOOST_LOG(severity.get()) << message << " (min/max/avg): " << f % min_value << units << "/" << f % max_value << units << "/" << f % avg_value << units;
          } else {
            BOOST_LOG(severity.get()) << message << " (min/max/avg): " << min_value << units << "/" << max_value << units << "/" << f % avg_value << units;
          }
        };
        tracker.collect_and_callback_on_interval(value, print_info, interval);
      }
    }

    void collect_and_log(std::function<T()> func) {
      if (enabled) {
        collect_and_log(func());
      }
    }

    void reset() {
      if (enabled) {
        tracker.reset();
      }
    }

    bool is_enabled() const {
      return enabled;
    }

  private:
    std::reference_wrapper<boost::log::sources::severity_logger<int>> severity;
    std::string message;
    std::string units;
    std::chrono::seconds interval;
    bool enabled;
    stat_trackers::min_max_avg_tracker<T> tracker;
  };

  /**
   * @brief A helper class for tracking and logging short time intervals across a period of time
   * @examples
   * time_delta_periodic_logger logger(debug, "Test duration", 5s);
   * logger.first_point_now();
   * // ...
   * logger.second_point_now_and_log();
   * // after 5 seconds
   * logger.first_point_now();
   * // ...
   * logger.second_point_now_and_log();
   * // In the log:
   * // [2024:01:01:12:00:00]: Debug: Test duration (min/max/avg): 1.23ms/3.21ms/2.31ms
   * @examples_end
   */
  class time_delta_periodic_logger {
  public:
    time_delta_periodic_logger(boost::log::sources::severity_logger<int> &severity, std::string_view message, std::chrono::seconds interval_in_seconds = std::chrono::seconds(20)):
        logger(severity, message, "ms", interval_in_seconds) {
    }

    void first_point(const std::chrono::steady_clock::time_point &point) {
      if (logger.is_enabled()) {
        point1 = point;
      }
    }

    void first_point_now() {
      if (logger.is_enabled()) {
        first_point(std::chrono::steady_clock::now());
      }
    }

    void second_point_and_log(const std::chrono::steady_clock::time_point &point) {
      if (logger.is_enabled()) {
        logger.collect_and_log(std::chrono::duration<double, std::milli>(point - point1).count());
      }
    }

    void second_point_now_and_log() {
      if (logger.is_enabled()) {
        second_point_and_log(std::chrono::steady_clock::now());
      }
    }

    void reset() {
      if (logger.is_enabled()) {
        logger.reset();
      }
    }

    bool is_enabled() const {
      return logger.is_enabled();
    }

  private:
    std::chrono::steady_clock::time_point point1 = std::chrono::steady_clock::now();
    min_max_avg_periodic_logger<double> logger;
  };

  /**
   * @brief Enclose string in square brackets.
   * @param input Input string.
   * @return Enclosed string.
   */
  std::string bracket(const std::string &input);

  /**
   * @brief Enclose string in square brackets.
   * @param input Input string.
   * @return Enclosed string.
   */
  std::wstring bracket(const std::wstring &input);

}  // namespace logging
