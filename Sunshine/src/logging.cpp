/**
 * @file src/logging.cpp
 * @brief Definitions for logging related functions.
 */
// standard includes
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>

// lib includes
#include <boost/core/null_deleter.hpp>
#include <boost/format.hpp>
#include <boost/log/attributes/clock.hpp>
#include <boost/log/common.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sinks.hpp>
#include <boost/log/sources/severity_logger.hpp>
#include <boost/log/utility/exception_handler.hpp>

// local includes
#include "logging.h"

// conditional includes
#ifdef __ANDROID__
  #include <android/log.h>
#else
  #include <display_device/logging.h>
#endif
#ifndef _WIN32
  // log 輪替前確認沒有別的行程在寫同一個檔（flock）
  #include <fcntl.h>
  #include <sys/file.h>
  #include <unistd.h>
#endif

extern "C" {
#include <libavutil/log.h>
}

using namespace std::literals;

namespace bl = boost::log;

boost::shared_ptr<boost::log::sinks::asynchronous_sink<boost::log::sinks::text_ostream_backend>> sink;

bl::sources::severity_logger<int> verbose(0);  // Dominating output
bl::sources::severity_logger<int> debug(1);  // Follow what is happening
bl::sources::severity_logger<int> info(2);  // Should be informed about
bl::sources::severity_logger<int> warning(3);  // Strange events
bl::sources::severity_logger<int> error(4);  // Recoverable errors
bl::sources::severity_logger<int> fatal(5);  // Unrecoverable errors
#ifdef SUNSHINE_TESTS
bl::sources::severity_logger<int> tests(10);  // Automatic tests output
#endif

BOOST_LOG_ATTRIBUTE_KEYWORD(severity, "Severity", int)

namespace logging {
  deinit_t::~deinit_t() {
    deinit();
  }

  void deinit() {
    log_flush();
    bl::core::get()->remove_sink(sink);
    sink.reset();
  }

  void formatter(const boost::log::record_view &view, boost::log::formatting_ostream &os) {
    constexpr const char *message = "Message";
    constexpr const char *severity = "Severity";

    auto log_level = view.attribute_values()[severity].extract<int>().get();

    std::string_view log_type;
    switch (log_level) {
      case 0:
        log_type = "Verbose: "sv;
        break;
      case 1:
        log_type = "Debug: "sv;
        break;
      case 2:
        log_type = "Info: "sv;
        break;
      case 3:
        log_type = "Warning: "sv;
        break;
      case 4:
        log_type = "Error: "sv;
        break;
      case 5:
        log_type = "Fatal: "sv;
        break;
#ifdef SUNSHINE_TESTS
      case 10:
        log_type = "Tests: "sv;
        break;
#endif
    };

    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      now - std::chrono::time_point_cast<std::chrono::seconds>(now)
    );

    auto t = std::chrono::system_clock::to_time_t(now);
    auto lt = *std::localtime(&t);

    os << "["sv << std::put_time(&lt, "%Y-%m-%d %H:%M:%S.") << boost::format("%03u") % ms.count() << "]: "sv
       << log_type << view.attribute_values()[message].extract<std::string>();
  }
#ifdef __ANDROID__
  namespace sinks = boost::log::sinks;
  namespace expr = boost::log::expressions;

  void android_log(const std::string &message, int severity) {
    android_LogPriority android_priority;
    switch (severity) {
      case 0:
        android_priority = ANDROID_LOG_VERBOSE;
        break;
      case 1:
        android_priority = ANDROID_LOG_DEBUG;
        break;
      case 2:
        android_priority = ANDROID_LOG_INFO;
        break;
      case 3:
        android_priority = ANDROID_LOG_WARN;
        break;
      case 4:
        android_priority = ANDROID_LOG_ERROR;
        break;
      case 5:
        android_priority = ANDROID_LOG_FATAL;
        break;
      default:
        android_priority = ANDROID_LOG_UNKNOWN;
        break;
    }
    __android_log_print(android_priority, "Sunshine", "%s", message.c_str());
  }

  // custom sink backend for android
  struct android_sink_backend: public sinks::basic_sink_backend<sinks::concurrent_feeding> {
    void consume(const bl::record_view &rec) {
      int log_sev = rec[severity].get();
      const std::string log_msg = rec[expr::smessage].get();
      // log to android
      android_log(log_msg, log_sev);
    }
  };
#endif

  namespace {
    namespace fs = std::filesystem;

    // 和 formatter 相同格式的時間戳前綴。輪替後新檔的頭兩行不經過 sink，要自己寫。
    std::string timestamp_prefix() {
      const auto now = std::chrono::system_clock::now();
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - std::chrono::time_point_cast<std::chrono::seconds>(now)
      );
      const auto t = std::chrono::system_clock::to_time_t(now);
      const auto lt = *std::localtime(&t);

      char date[32] = {};
      std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", &lt);
      char out[48] = {};
      std::snprintf(out, sizeof(out), "[%s.%03d]: ", date, static_cast<int>(ms.count()));
      return out;
    }

    /**
     * §PERF-1113-R2（A14）：會自己輪替的 log 檔。原本 sunshine.log 每次 server 啟動就截斷（當機、Web UI 重啟、
     * session 切換之後前一輪的 log 就沒了），執行中又沒有大小上限（VR 中每小時 8～15 MB）。
     *   - 啟動時：上一輪的檔有內容就改名成 `<檔>.1`（原本的 `.1` 變 `.2`），再開新檔。
     *     上一輪很短（啟動就失敗、連續重啟）時改收進 `<檔>.0`，不佔 `.1`／`.2`——server 當掉後服務每 3 秒重啟一次，
     *     不這樣做的話三次啟動失敗就把當機前那一輪擠掉了。
     *   - 執行中：累計寫入超過上限時同樣輪替一次，新檔開頭補版本行與一行輪替標記。
     * 只給 text_ostream_backend 用：它在 sink 的鎖裡面寫，同一時間只有一條執行緒進來；
     * 每一筆 log 寫完會 flush 一次（auto_flush），輪替放在 sync() 裡就剛好落在行與行之間。
     */
    class rotating_log_buf_t: public std::streambuf {
    public:
      rotating_log_buf_t(const std::string &path, const file_options_t &options):
          path_ {path},
          options_ {options} {
        try {
          cur_ = fs::path {path};
          old0_ = fs::path {path + ".0"};
          old1_ = fs::path {path + ".1"};
          old2_ = fs::path {path + ".2"};
          tmp_ = fs::path {path + ".tmp"};
          have_paths_ = true;
        } catch (...) {
          // 路徑轉不成 filesystem::path：不輪替，其餘照舊
        }

        if (have_paths_) {
          other_writer_ = !lock_as_writer();
        }
        if (have_paths_ && !other_writer_) {
          recover_stale_tmp();
          std::error_code ec;
          const auto old_size = fs::file_size(cur_, ec);
          if (!ec && old_size > 0 && !park_short_run(old_size)) {
            // 改不了名（別的程式正開著）就照原本的做法，下面直接截斷
            shift_old_files();
          }
        }
        open_fresh(false);
      }

#ifndef _WIN32
      ~rotating_log_buf_t() override {
        if (lock_fd_ >= 0) {
          ::close(lock_fd_);
        }
      }
#endif

      bool is_open() const {
        return file_.is_open();
      }

    protected:
      // 寫入一律回報成功：text_ostream_backend 只寫 good() 的 stream，回報一次失敗這個檔就永遠不會再寫了
      int_type overflow(int_type ch) override {
        if (!traits_type::eq_int_type(ch, traits_type::eof())) {
          if (file_.is_open()) {
            file_.sputc(traits_type::to_char_type(ch));
          }
          ++written_;
        }
        return traits_type::not_eof(ch);
      }

      std::streamsize xsputn(const char *s, std::streamsize n) override {
        if (n <= 0) {
          return 0;
        }
        if (file_.is_open()) {
          file_.sputn(s, n);
        }
        written_ += static_cast<std::uintmax_t>(n);
        return n;
      }

      int sync() override {
        if (file_.is_open()) {
          file_.pubsync();
          if (written_ >= rotate_at_) {
            rotate();
          }
        } else if (written_ >= retry_open_at_) {
          // 新檔開不起來：每隔一段再試一次（附加模式，不動已經在那裡的內容）
          retry_open_at_ = written_ + retry_step;
          prepare();
          file_.open(path_, std::ios::out | std::ios::app);
        }
        return 0;
      }

    private:
      static constexpr std::uintmax_t retry_step = 64 * 1024;
      static constexpr std::uintmax_t defer_step = 1024 * 1024;

      static constexpr std::string_view rotated_tag = "[VIPLE-LOG] rotated after "sv;

      // 輪替之後上一輪的檔在哪裡
      enum class kept_e {
        none,  ///< 目前的檔改不了名，什麼都沒動
        old1,  ///< 留成 `<檔>.1`
        tmp,  ///< `.1` 的位置空不出來，留在 `<檔>.tmp`（下次輪替或下次啟動時補搬）
      };

      // 「很短的一輪」的門檻：啟動就失敗、或啟動後沒人串流就重啟的一輪約 13 KB，有一場串流的至少 100 KB
      std::uintmax_t short_run_bytes() const {
        return std::min<std::uintmax_t>(64 * 1024, options_.max_bytes / 4);
      }

      // `.0` 累積的上限
      std::uintmax_t parked_max_bytes() const {
        return std::min<std::uintmax_t>(1024 * 1024, options_.max_bytes);
      }

      static bool exists(const fs::path &p) {
        std::error_code ec;
        return fs::exists(p, ec);
      }

      // a 的最後寫入時間比 b 晚（任一個讀不到就回 false）
      static bool newer(const fs::path &a, const fs::path &b) {
        std::error_code ea;
        std::error_code eb;
        const auto ta = fs::last_write_time(a, ea);
        const auto tb = fs::last_write_time(b, eb);
        return !ea && !eb && ta > tb;
      }

      // 回 false＝別的行程正在寫同一個 log 檔。這時整個照原本的做法（啟動截斷、不輪替），不去動它正在寫的檔。
      // POSIX 的 rename 對開著的檔照樣成功：服務在跑時又手動啟動一次，會把運作中那個行程的 log 改名成 `.1`，
      // 再啟動兩次就被刪掉。所以輪替前先對 `<檔>.lock` 取獨佔鎖，持有到行程結束；取不到（或鎖檔開不了）就不輪替。
      // O_CLOEXEC：鎖不可以留給子行程，也不可以跨過「重啟」的 execv（新的映像要重新取得）。
      // Windows 不必：寫入中的檔改不了名（開檔的共享模式），輪替自己會退回截斷。
      bool lock_as_writer() {
#ifdef _WIN32
        return true;
#else
        lock_fd_ = ::open((path_ + ".lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (lock_fd_ >= 0 && ::flock(lock_fd_, LOCK_EX | LOCK_NB) == 0) {
          return true;
        }
        if (lock_fd_ >= 0) {
          ::close(lock_fd_);
          lock_fd_ = -1;
        }
        return false;
#endif
      }

      // 執行中輪替出來的檔（開頭有輪替標記）是上一輪的後半段，再短也不算「很短的一輪」
      bool is_continuation() const {
        std::ifstream in {cur_, std::ios::binary};
        char head[1024];
        in.read(head, sizeof(head));
        return std::string_view {head, static_cast<std::size_t>(in.gcount())}.find(rotated_tag) != std::string_view::npos;
      }

      // 上一輪很短：收進 `<檔>.0`，不佔 `.1`／`.2`。接得下去就接在後面（連續啟動失敗的各輪都留著），
      // 超過上限就整個換成這一輪。回 false＝不算短、或收不進去，照一般的輪替。
      // 還沒有 `.1` 時也照一般的輪替：不會擠掉誰，而且第一次重啟後 `.1` 一定看得到。
      // `.0` 只會由改名產生（安全描述元跟著檔案走），接在後面時不新建檔。
      bool park_short_run(std::uintmax_t size) {
        if (size >= short_run_bytes() || !exists(old1_) || is_continuation()) {
          return false;
        }
        std::error_code ec;
        const auto parked = fs::file_size(old0_, ec);
        if (!ec && parked + size <= parked_max_bytes()) {
          std::ifstream in {cur_, std::ios::binary};
          std::ofstream out {old0_, std::ios::binary | std::ios::app};
          if (in && out && (out << in.rdbuf()) && out.flush()) {
            return true;  // 目前的檔留在原地，接著由 open_fresh 截斷
          }
        }
        ec.clear();
        fs::rename(cur_, old0_, ec);
        return !ec;
      }

      // 把 `.1` 的位置空出來。回 false＝空不出來（被別的程式開著），這時 `.1` 留在原地、不刪
      bool free_old1() {
        if (!exists(old1_)) {
          return true;
        }
        std::error_code ec;
        if (newer(old2_, old1_)) {
          // 之前 `.1` 被開著時，較新的一輪只好放進 `.2`（見 recover_stale_tmp）：這時最舊的是 `.1`
          fs::remove(old1_, ec);
        } else {
          fs::rename(old1_, old2_, ec);  // 蓋掉原本的 `.2`
        }
        return !exists(old1_);
      }

      // 上次輪替時 `.1` 空不出來而留在暫時名的那一輪（比 `.1`、`.2` 都新）：補搬回去。`.1` 空得出來就放 `.1`；
      // 不然蓋掉 `.2`，再不然蓋掉 `.1`（哪一個沒被開著就蓋哪一個，丟的都是比它舊的）；
      // 兩個都動不了才丟——暫時名要讓給接下來這一輪。
      void recover_stale_tmp() {
        if (!exists(tmp_)) {
          return;
        }
        std::error_code ec;
        if (free_old1()) {
          fs::rename(tmp_, old1_, ec);
          if (!ec) {
            return;
          }
        }
        for (const auto *slot : {&old2_, &old1_}) {
          ec.clear();
          fs::rename(tmp_, *slot, ec);
          if (!ec) {
            return;
          }
        }
        fs::remove(tmp_, ec);
      }

      // `<檔>.1` → `<檔>.2`、`<檔>` → `<檔>.1`；改名時安全描述元跟著檔案走
      kept_e shift_old_files() {
        if (!have_paths_) {
          return kept_e::none;
        }
        recover_stale_tmp();
        std::error_code ec;
        // 先把目前的檔搬到暫時名：搬不動（別的程式正開著）就什麼都不動，舊的 .1／.2 不會白白少一份
        fs::rename(cur_, tmp_, ec);
        if (ec) {
          return kept_e::none;
        }
        if (free_old1()) {
          fs::rename(tmp_, old1_, ec);
          if (!ec) {
            return kept_e::old1;
          }
        }
        return kept_e::tmp;
      }

      void rotate() {
        file_.close();
        const auto kept = shift_old_files();
        if (kept == kept_e::none && written_ < options_.max_bytes * 2) {
          // 改不了名，多半是別的程式正開著讀（Web UI 的 log 頁、手動開檔）：接著寫原檔，再寫一段之後重試。
          // 到兩倍上限還是不行才原地截斷——不能讓它無限長下去。
          file_.open(path_, std::ios::out | std::ios::app);
          rotate_at_ = written_ + std::min<std::uintmax_t>(defer_step, std::max<std::uintmax_t>(options_.max_bytes / 8, 1));
          return;
        }
        open_fresh(true, kept);
      }

      // 檔案還不存在時先交給呼叫端建立（Windows：帶受保護的安全描述元）。失敗不擋開檔
      void prepare() {
        if (!options_.prepare_file) {
          return;
        }
        try {
          options_.prepare_file(path_);
        } catch (...) {
        }
      }

      // rotated＝執行中輪替（要在新檔開頭補兩行）；kept＝上一輪的檔留在哪裡
      void open_fresh(bool rotated, kept_e kept = kept_e::none) {
        prepare();
        file_.open(path_, std::ios::out | std::ios::trunc);
        written_ = 0;
        // 別的行程也在寫這個檔時不輪替（和原本一樣沒有上限）
        rotate_at_ = other_writer_ ? std::numeric_limits<std::uintmax_t>::max() : options_.max_bytes;
        retry_open_at_ = retry_step;
        if (!rotated || !file_.is_open()) {
          return;
        }

        // 新檔開頭：版本行（部署流程會看 log 開頭的 version 行）與輪替標記。不經過 sink，直接寫
        const auto ts = timestamp_prefix();
        std::string head;
        if (!options_.first_line.empty()) {
          head += ts + "Info: " + options_.first_line + '\n';
        }
        head += ts + "Info: ";
        head += rotated_tag;
        head += std::to_string(options_.max_bytes) + " bytes; ";
        switch (kept) {
          case kept_e::old1:
            head += "the previous log is now " + old1_.filename().string();
            break;
          case kept_e::tmp:
            // 寫實際的檔名：這時去看 .1 會看到更舊的一輪
            head += "the previous log is now " + tmp_.filename().string() + " (" + old1_.filename().string() + " is in use)";
            break;
          case kept_e::none:
            head += "the previous log could not be renamed and was truncated";
            break;
        }
        head += '\n';
        file_.sputn(head.data(), static_cast<std::streamsize>(head.size()));
        file_.pubsync();
        written_ = head.size();
      }

      std::string path_;
      file_options_t options_;
      fs::path cur_;
      fs::path old0_;
      fs::path old1_;
      fs::path old2_;
      fs::path tmp_;
      bool have_paths_ = false;
      bool other_writer_ = false;  // 別的行程正在寫同一個檔：不輪替
#ifndef _WIN32
      int lock_fd_ = -1;
#endif
      std::filebuf file_;
      std::uintmax_t written_ = 0;  // 目前這個檔累計寫了多少（文字模式下的換行轉換不算，夠用）
      std::uintmax_t rotate_at_ = 0;
      std::uintmax_t retry_open_at_ = retry_step;
    };

    class rotating_log_stream_t: public std::ostream {
    public:
      rotating_log_stream_t(const std::string &path, const file_options_t &options):
          std::ostream(static_cast<std::streambuf *>(nullptr)),
          buf_ {path, options} {
        rdbuf(&buf_);
      }

      bool is_open() const {
        return buf_.is_open();
      }

    private:
      rotating_log_buf_t buf_;
    };
  }  // namespace

  [[nodiscard]] std::unique_ptr<deinit_t> init(int min_log_level, const std::string &log_file, const file_options_t &file_options) {
    if (sink) {
      // Deinitialize the logging system before reinitializing it. This can probably only ever be hit in tests.
      deinit();
    }

#ifndef __ANDROID__
    setup_av_logging(min_log_level);
    setup_libdisplaydevice_logging(min_log_level);
#endif

    sink = boost::make_shared<text_sink>();

    // §PERF-1113-R2（A14）：沒有要求輪替時維持原本的 std::ofstream（CLI 模式、測試、工具）
    boost::shared_ptr<std::ostream> file_stream;
    [[maybe_unused]] bool file_ok = false;
    if (file_options.max_bytes > 0) {
      auto rotating = boost::make_shared<rotating_log_stream_t>(log_file, file_options);
      file_ok = rotating->is_open();
      file_stream = rotating;
    } else {
      auto plain = boost::make_shared<std::ofstream>(log_file);
      file_ok = plain->is_open();
      file_stream = plain;
    }

#ifndef SUNSHINE_TESTS
    // 服務模式的 stdout 是 viplestream-svc.log：log 檔開成功時就不再整份鏡像過去（啟動前的訊息、直接寫 stdout／stderr 的
    // 內容與 CRT 的錯誤仍會進那個檔）。log 檔開不起來時保留 stdout，至少還有一個地方看得到。
    if (!(file_options.no_stdout_mirror && file_ok)) {
      boost::shared_ptr<std::ostream> stream {&std::cout, boost::null_deleter()};
      sink->locked_backend()->add_stream(stream);
    }
#endif

    sink->locked_backend()->add_stream(file_stream);
    sink->set_filter(severity >= min_log_level);
    sink->set_formatter(&formatter);

    // Prevent the async sink's background thread from dying on backend exceptions.
    // Without this, a single I/O error (disk full, file locked, broken stdout, etc.)
    // kills the thread and all subsequent log records are silently lost.
    sink->set_exception_handler(bl::make_exception_suppressor());

    // Flush after each log record to ensure log file contents on disk isn't stale.
    // This is particularly important when running from a Windows service.
    sink->locked_backend()->auto_flush(true);

    bl::core::get()->add_sink(sink);

#ifdef __ANDROID__
    auto android_sink = boost::make_shared<sinks::synchronous_sink<android_sink_backend>>();
    bl::core::get()->add_sink(android_sink);
#endif
    return std::make_unique<deinit_t>();
  }

#ifndef __ANDROID__
  void setup_av_logging(int min_log_level) {
    if (min_log_level >= 1) {
      av_log_set_level(AV_LOG_QUIET);
    } else {
      av_log_set_level(AV_LOG_DEBUG);
    }
    av_log_set_callback([](void *ptr, int level, const char *fmt, va_list vl) {
      static int print_prefix = 1;
      char buffer[1024];

      av_log_format_line(ptr, level, fmt, vl, buffer, sizeof(buffer), &print_prefix);
      if (level <= AV_LOG_ERROR) {
        // We print AV_LOG_FATAL at the error level. FFmpeg prints things as fatal that
        // are expected in some cases, such as lack of codec support or similar things.
        BOOST_LOG(error) << buffer;
      } else if (level <= AV_LOG_WARNING) {
        BOOST_LOG(warning) << buffer;
      } else if (level <= AV_LOG_INFO) {
        BOOST_LOG(info) << buffer;
      } else if (level <= AV_LOG_VERBOSE) {
        // AV_LOG_VERBOSE is less verbose than AV_LOG_DEBUG
        BOOST_LOG(debug) << buffer;
      } else {
        BOOST_LOG(verbose) << buffer;
      }
    });
  }

  void setup_libdisplaydevice_logging(int min_log_level) {
    constexpr int min_level {static_cast<int>(display_device::Logger::LogLevel::verbose)};
    constexpr int max_level {static_cast<int>(display_device::Logger::LogLevel::fatal)};
    const auto log_level {static_cast<display_device::Logger::LogLevel>(std::min(std::max(min_level, min_log_level), max_level))};

    display_device::Logger::get().setLogLevel(log_level);
    display_device::Logger::get().setCustomCallback([](const display_device::Logger::LogLevel level, const std::string &message) {
      switch (level) {
        case display_device::Logger::LogLevel::verbose:
          BOOST_LOG(verbose) << message;
          break;
        case display_device::Logger::LogLevel::debug:
          BOOST_LOG(debug) << message;
          break;
        case display_device::Logger::LogLevel::info:
          BOOST_LOG(info) << message;
          break;
        case display_device::Logger::LogLevel::warning:
          BOOST_LOG(warning) << message;
          break;
        case display_device::Logger::LogLevel::error:
          BOOST_LOG(error) << message;
          break;
        case display_device::Logger::LogLevel::fatal:
          BOOST_LOG(fatal) << message;
          break;
      }
    });
  }
#endif

  void log_flush() {
    if (sink) {
      sink->flush();
    }
  }

  void print_help(const char *name) {
    std::cout
      << "Usage: "sv << name << " [options] [/path/to/configuration_file] [--cmd]"sv << std::endl
      << "    Any configurable option can be overwritten with: \"name=value\""sv << std::endl
      << std::endl
      << "    Note: The configuration will be created if it doesn't exist."sv << std::endl
      << std::endl
      << "    --help                    | print help"sv << std::endl
      << "    --creds username password | set user credentials for the Web manager"sv << std::endl
      << "    --version                 | print the version of sunshine"sv << std::endl
      << "    --check-update            | query the latest GitHub release (exit 2 = update available)"sv << std::endl
      << "    --self-update [--force] [package] | download and install the latest release (or a local zip/deb)"sv << std::endl
#ifdef _WIN32
      // VipleStream 2.0 §VR（M1b §F.5）：經 admin pipe 交給執行中的 server；需要提升的管理員
      << "    --vr-selftest [--only T0,T2,T6] [--detach] [--probe-path exe] | run the VR self-test in the running server (elevated admin)"sv << std::endl
      << "    --vr-status               | show the progress or result of the last VR self-test"sv << std::endl
      << "    --vr-abort                | abort the running VR self-test"sv << std::endl
      << "    --steamvr-driver status   | show the VR bridge state of the running server"sv << std::endl
#endif
      << std::endl
      << "    flags"sv << std::endl
      << "        -0 | Read PIN from stdin"sv << std::endl
      << "        -1 | Do not load previously saved state and do retain any state after shutdown"sv << std::endl
      << "           | Effectively starting as if for the first time without overwriting any pairings with your devices"sv << std::endl
      << "        -2 | Force replacement of headers in video stream"sv << std::endl
      << "        -p | Enable/Disable UPnP"sv << std::endl
      << std::endl;
  }

  std::string bracket(const std::string &input) {
    return "["s + input + "]"s;
  }

  std::wstring bracket(const std::wstring &input) {
    return L"["s + input + L"]"s;
  }

}  // namespace logging
