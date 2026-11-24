/**
 * @file src/logdrop.cpp
 * @brief §LOGDROP：遮罩與上傳。遮罩用單向掃描，不用巢狀量詞的 regex（log 可能數 MB）。
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio/ip/host_name.hpp>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include "config.h"
#include "crypto.h"
#include "logdrop.h"
#include "logging.h"
#include "nvhttp.h"
#include "platform/common.h"

#ifndef _WIN32
  #include <sys/utsname.h>
#endif

namespace logdrop {

  namespace {

    constexpr std::uint64_t kCurrentTailBytes = 4ull * 1024ull * 1024ull;
    constexpr std::uint64_t kPreviousTailBytes = 2ull * 1024ull * 1024ull;
    // §LOGDROP：重啟後新檔常常只剩開頭。門檻用檔案大小，不是裁過的內容長度。
    constexpr std::uint64_t kSmallCurrentBytes = 256ull * 1024ull;
    constexpr std::size_t kNoteChars = 2000;
    constexpr std::size_t kDeviceChars = 200;
    constexpr std::size_t kResponseCap = 1024ull * 1024ull;

    constexpr const char *kUserAgent = "VipleStream-Server-LogDrop (" PROJECT_VERSION ")";

#ifdef _WIN32
    constexpr const char *kSource = "server-windows";
#else
    constexpr const char *kSource = "server-linux";
#endif

    std::mutex g_upload_mutex;

    bool is_digit(char c) {
      return c >= '0' && c <= '9';
    }

    bool is_alpha(char c) {
      return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    }

    bool is_hex(char c) {
      return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }

    char fold_ascii(char c) {
      if (c >= 'A' && c <= 'Z') {
        return static_cast<char>(c - 'A' + 'a');
      }
      return c;
    }

    bool ieq_at(std::string_view text, std::size_t i, std::string_view secret) {
      if (i + secret.size() > text.size()) {
        return false;
      }
      for (std::size_t k = 0; k < secret.size(); ++k) {
        if (fold_ascii(text[i + k]) != fold_ascii(secret[k])) {
          return false;
        }
      }
      return true;
    }

    // 回傳四段的結尾；失敗是 npos。third 是第三段的起點（「a.b.」的下一個字元）。
    std::size_t parse_dotted_quad(std::string_view s, std::size_t begin, int oct[4], std::size_t &third) {
      std::size_t i = begin;
      third = std::string_view::npos;
      for (int k = 0; k < 4; ++k) {
        if (k > 0) {
          if (i >= s.size() || s[i] != '.') {
            return std::string_view::npos;
          }
          ++i;
        }
        if (k == 2) {
          third = i;
        }
        if (i >= s.size() || !is_digit(s[i])) {
          return std::string_view::npos;
        }
        int val = 0;
        int digits = 0;
        while (i < s.size() && digits < 3 && is_digit(s[i])) {
          val = val * 10 + (s[i] - '0');
          ++i;
          ++digits;
        }
        // 第四個數字代表這段超過三位數（版號 26300 這種），不是 IPv4。
        if (digits == 0 || val > 255 || (i < s.size() && is_digit(s[i]))) {
          return std::string_view::npos;
        }
        oct[k] = val;
      }
      return i;
    }

    bool is_email_local(char c) {
      return is_digit(c) || is_alpha(c) || c == '.' || c == '_' || c == '%' || c == '+' || c == '-';
    }

    bool is_domain_char(char c) {
      return is_digit(c) || is_alpha(c) || c == '-';
    }

    std::size_t match_email(std::string_view s, std::size_t begin) {
      if (begin >= s.size() || !is_email_local(s[begin])) {
        return 0;
      }
      // 從信箱中間開始會切出另一個假位址；前面還是信箱字元就略過，讓起點那次吃整段。
      if (begin > 0 && is_email_local(s[begin - 1])) {
        return 0;
      }
      std::size_t i = begin;
      std::size_t local = 0;
      while (i < s.size() && local < 64 && s[i] != '@' && is_email_local(s[i])) {
        ++i;
        ++local;
      }
      if (local == 0 || i >= s.size() || s[i] != '@') {
        return 0;
      }
      ++i;

      int labels = 0;
      std::size_t label_len = 0;
      bool label_alpha = true;
      char label_first = 0;
      char label_last = 0;
      bool last_alpha = false;
      std::size_t last_len = 0;
      auto close_label = [&]() -> bool {
        if (label_len == 0 || label_len > 63) {
          return false;
        }
        if (label_first == '-' || label_last == '-') {
          return false;
        }
        last_alpha = label_alpha;
        last_len = label_len;
        ++labels;
        label_len = 0;
        label_alpha = true;
        return true;
      };

      std::size_t domain = 0;
      while (i < s.size() && domain < 253) {
        const char c = s[i];
        if (c == '.') {
          // 句尾的點不是網域的一段，留給後面的文字。
          if (i + 1 >= s.size() || !is_domain_char(s[i + 1])) {
            break;
          }
          if (!close_label()) {
            return 0;
          }
          ++i;
          ++domain;
          continue;
        }
        if (!is_domain_char(c)) {
          break;
        }
        if (label_len == 0) {
          label_first = c;
        }
        if (label_len >= 63) {
          return 0;
        }
        if (!is_alpha(c)) {
          label_alpha = false;
        }
        label_last = c;
        ++label_len;
        ++i;
        ++domain;
      }
      if (!close_label()) {
        return 0;
      }
      if (labels < 2 || !last_alpha || last_len < 2 || last_len > 24) {
        return 0;
      }
      return i;
    }

    bool consume_h16(std::string_view s, std::size_t &p, bool &hex_letter) {
      if (p >= s.size() || !is_hex(s[p])) {
        return false;
      }
      const std::size_t start = p;
      int digits = 0;
      bool letter = false;
      while (p < s.size() && digits < 4 && is_hex(s[p])) {
        if (!is_digit(s[p])) {
          letter = true;
        }
        ++p;
        ++digits;
      }
      // 第五個十六進位代表這組太長，整段不是 IPv6；位置退回去。
      if (p < s.size() && is_hex(s[p])) {
        p = start;
        return false;
      }
      if (letter) {
        hex_letter = true;
      }
      return digits > 0;
    }

    std::size_t match_ipv6(std::string_view s, std::size_t begin, bool &keep) {
      keep = false;
      const auto n = s.size();
      if (begin >= n) {
        return 0;
      }
      // §LOGDROP：前面若是數字、字母或冒號，代表還在同一個識別字裡。只擋十六進位時，word::1 會從最後一個十六進位被切開。
      if (begin > 0 && (is_digit(s[begin - 1]) || is_alpha(s[begin - 1]) || s[begin - 1] == ':')) {
        return 0;
      }
      if (!(is_hex(s[begin]) || s[begin] == ':')) {
        return 0;
      }

      std::size_t i = begin;
      int groups = 0;
      bool compressed = false;
      bool hex_letter = false;

      while (i < n && groups < 8) {
        if (s[i] != ':' && !is_hex(s[i])) {
          break;
        }
        if (s[i] == ':') {
          if (!(i + 1 < n && s[i + 1] == ':') || compressed) {
            return 0;
          }
          compressed = true;
          i += 2;
          if (i < n && s[i] == ':') {
            return 0;
          }
          continue;
        }

        if (is_digit(s[i])) {
          bool dotted = false;
          for (std::size_t k = i; k < n && s[k] != ':'; ++k) {
            if (s[k] == '.') {
              dotted = true;
              break;
            }
            if (!is_digit(s[k])) {
              break;
            }
          }
          if (dotted) {
            int oct[4];
            std::size_t third = 0;
            const auto after = parse_dotted_quad(s, i, oct, third);
            // 四段只用來確認長度並算成兩組；整段會變成 <ipv6>，數字不再另遮。
            (void) oct;
            (void) third;
            if (after == std::string_view::npos) {
              return 0;
            }
            groups += 2;
            i = after;
            break;
          }
        }

        if (!consume_h16(s, i, hex_letter)) {
          return 0;
        }
        ++groups;
        if (i < n && s[i] == ':') {
          if (i + 1 < n && s[i + 1] == ':') {
            continue;
          }
          if (i + 1 < n && is_hex(s[i + 1])) {
            ++i;
            continue;
          }
          break;
        }
        break;
      }

      if (groups > 8) {
        return 0;
      }
      if (compressed) {
        if (groups >= 8) {
          return 0;
        }
      } else if (groups != 8 || !hex_letter) {
        // 沒有 a–f、也沒有 :: 的（12:34:56、八組全數字）不要當 IPv6。
        return 0;
      }
      if (i == begin) {
        return 0;
      }

      std::size_t end = i;
      if (end < n && s[end] == '%') {
        std::size_t p = end + 1;
        std::size_t len = 0;
        while (p < n && len < 32) {
          const char c = s[p];
          if (!(is_digit(c) || is_alpha(c) || c == '.' || c == '_' || c == '-')) {
            break;
          }
          ++p;
          ++len;
        }
        if (len > 0) {
          end = p;
        }
      }
      // §LOGDROP：後面還接著字母（::string、std::vector）代表這組寫不完，不是獨立的 IPv6。整段都是十六進位的 fade::beef 仍要遮。
      if (end < n && (is_digit(s[end]) || is_alpha(s[end]) || s[end] == ':')) {
        return 0;
      }

      std::size_t core_end = end;
      for (std::size_t k = begin; k < end; ++k) {
        if (s[k] == '%') {
          core_end = k;
          break;
        }
      }
      // ::1 與帶 zone 的 ::1%eth0 都是本機迴圈，留著。
      if (s.substr(begin, core_end - begin) == "::1") {
        keep = true;
      }
      return end;
    }

    std::size_t match_mac(std::string_view s, std::size_t begin, std::string &repl) {
      if (begin + 17 > s.size() || !is_hex(s[begin])) {
        return 0;
      }
      if (begin > 0 && is_hex(s[begin - 1])) {
        return 0;
      }
      const char sep = s[begin + 2];
      if (sep != ':' && sep != '-') {
        return 0;
      }
      for (int g = 0; g < 6; ++g) {
        const auto p = begin + static_cast<std::size_t>(g) * 3;
        if (!is_hex(s[p]) || !is_hex(s[p + 1])) {
          return 0;
        }
        if (g < 5 && s[p + 2] != sep) {
          return 0;
        }
      }
      const auto end = begin + 17;
      // 後面還接著同一種分隔，代表超過六組，不要切前六組。
      if (end < s.size() && (is_hex(s[end]) || s[end] == sep)) {
        return 0;
      }
      repl.assign(s.data() + begin, 8);
      repl.push_back(sep);
      repl += "xx";
      repl.push_back(sep);
      repl += "xx";
      repl.push_back(sep);
      repl += "xx";
      return end;
    }

    std::size_t match_ipv4(std::string_view s, std::size_t begin, std::string &repl) {
      if (begin >= s.size() || !is_digit(s[begin])) {
        return 0;
      }
      // 前後緊接數字或點的不是獨立的 IPv4（版號、多一段的位址）。
      if (begin > 0 && (is_digit(s[begin - 1]) || s[begin - 1] == '.')) {
        return 0;
      }
      int oct[4];
      std::size_t third = 0;
      const auto end = parse_dotted_quad(s, begin, oct, third);
      if (end == std::string_view::npos || third == std::string_view::npos) {
        return 0;
      }
      if (end < s.size() && (is_digit(s[end]) || s[end] == '.')) {
        return 0;
      }
      if (oct[0] == 127 || (oct[0] == 0 && oct[1] == 0 && oct[2] == 0 && oct[3] == 0)) {
        repl.assign(s.data() + begin, end - begin);
        return end;
      }
      const bool mask = oct[0] == 10 || (oct[0] == 172 && oct[1] >= 16 && oct[1] <= 31) || (oct[0] == 192 && oct[1] == 168) || (oct[0] == 100 && oct[1] >= 64 && oct[1] <= 127) || (oct[0] == 169 && oct[1] == 254);
      if (mask) {
        repl.assign(s.data() + begin, third - begin);
        repl += "x.x";
        return end;
      }
      repl = "<ip>";
      return end;
    }

    std::string redact_structural(std::string_view text) {
      std::string out;
      out.reserve(text.size());
      for (std::size_t i = 0; i < text.size();) {
        // §LOGDROP：長度算錯時若停在同一位元組，上傳會轉不完；這時留原字。
        const auto copy_one = [&]() {
          out.push_back(text[i]);
          ++i;
        };
        // 信箱先吃掉，避免裡面的數字被當成 IP。IPv6 在 IPv4 之前，映射位址才會整段遮掉。
        if (const auto end = match_email(text, i)) {
          if (end <= i) {
            copy_one();
            continue;
          }
          out += "<email>";
          i = end;
          continue;
        }
        bool keep = false;
        if (const auto end = match_ipv6(text, i, keep)) {
          if (end <= i) {
            copy_one();
            continue;
          }
          if (keep) {
            out.append(text.data() + i, end - i);
          } else {
            out += "<ipv6>";
          }
          i = end;
          continue;
        }
        std::string repl;
        if (const auto end = match_mac(text, i, repl)) {
          if (end <= i) {
            copy_one();
            continue;
          }
          out += repl;
          i = end;
          continue;
        }
        if (const auto end = match_ipv4(text, i, repl)) {
          if (end <= i) {
            copy_one();
            continue;
          }
          out += repl;
          i = end;
          continue;
        }
        copy_one();
      }
      return out;
    }

    // 位址先遮完再遮名稱：名稱若是位址的一段，先遮名稱會把 IP 拆開，剩下的就不再像位址。
    // 置換後的 <redacted> 不再掃描，避免祕密字本身出現在這個佔位字裡時一直長下去。
    std::string apply_secrets(std::string text, const std::vector<std::string> &extra_secrets) {
      bool any = false;
      for (const auto &secret : extra_secrets) {
        if (secret.size() >= 3) {
          any = true;
          break;
        }
      }
      if (!any || text.empty()) {
        return text;
      }
      std::string out;
      out.reserve(text.size());
      const std::string_view view = text;
      for (std::size_t i = 0; i < view.size();) {
        std::size_t best = 0;
        for (const auto &secret : extra_secrets) {
          if (secret.size() < 3 || secret.size() <= best) {
            continue;
          }
          if (ieq_at(view, i, secret)) {
            best = secret.size();
          }
        }
        if (best > 0) {
          out += "<redacted>";
          i += best;
        } else {
          out.push_back(view[i]);
          ++i;
        }
      }
      return out;
    }

    // 協定與網頁 maxlength 的「字」都是 UTF-16 code unit（Apps Script 的 String.length）。
    std::string clip_utf16_units(std::string_view text, std::size_t max_units) {
      std::size_t i = 0;
      std::size_t units = 0;
      while (i < text.size() && units < max_units) {
        const auto c = static_cast<unsigned char>(text[i]);
        std::size_t len = 1;
        std::size_t add = 1;
        if ((c & 0x80) == 0) {
          len = 1;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < text.size()) {
          len = 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < text.size()) {
          len = 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < text.size()) {
          len = 4;
          add = 2;
        }
        if (units + add > max_units) {
          break;
        }
        i += len;
        units += add;
      }
      return std::string(text.substr(0, i));
    }

    void crlf_to_lf(std::string &s) {
      // 空字串的 data() 可能是空指標，memchr 不能收。
      if (s.empty()) {
        return;
      }
      const char *const base = s.data();
      const char *const end = base + s.size();
      const char *r = static_cast<const char *>(std::memchr(base, '\r', s.size()));
      if (!r) {
        return;
      }
      char *w = s.data() + (r - base);
      while (r < end) {
        const char *cr = static_cast<const char *>(std::memchr(r, '\r', static_cast<std::size_t>(end - r)));
        const char *const chunk_end = cr ? cr : end;
        const auto chunk = static_cast<std::size_t>(chunk_end - r);
        if (chunk && w != r) {
          std::memmove(w, r, chunk);
        }
        w += chunk;
        if (!cr) {
          break;
        }
        if (cr + 1 >= end || cr[1] != '\n') {
          *w++ = '\r';
        }
        r = cr + 1;
      }
      s.resize(static_cast<std::size_t>(w - s.data()));
    }

    struct tail_t {
      std::string text;
      std::uint64_t size = 0;
    };

    // 跟 getLogs 一樣：路徑是 config::sunshine.log_file，二進位讀檔尾，起點落在半行就丟到下一個換行。
    tail_t read_tail(const std::string &path, std::uint64_t max_bytes) {
      tail_t out;
      std::ifstream in {path, std::ios::binary};
      if (!in) {
        return out;
      }
      in.seekg(0, std::ios::end);
      if (!in) {
        return out;
      }
      const auto end_off = in.tellg();
      if (end_off <= 0) {
        return out;
      }
      out.size = static_cast<std::uint64_t>(end_off);
      std::uint64_t start = 0;
      if (out.size > max_bytes) {
        start = out.size - max_bytes;
      }
      out.text.resize(static_cast<std::size_t>(out.size - start));
      in.seekg(static_cast<std::streamoff>(start), std::ios::beg);
      in.read(out.text.data(), static_cast<std::streamsize>(out.text.size()));
      const auto got = in.gcount();
      out.text.resize(static_cast<std::size_t>(got > 0 ? got : 0));
      if (start > 0) {
        if (const auto nl = out.text.find('\n'); nl != std::string::npos) {
          out.text.erase(0, nl + 1);
        }
      }
      // Windows 的 log 在磁碟上是 CRLF；併成 LF 才跟網頁讀到的同一份，也不會被換行灌大。
      crlf_to_lf(out.text);
      return out;
    }

    std::string build_payload(const std::string &log_path, const std::vector<std::string> &secrets, bool prefer_previous) {
      // 空路徑再接 ".1" 會去開工作目錄裡名為 .1 的檔，那不是上一輪 log。
      if (log_path.empty()) {
        return {};
      }
      auto current = read_tail(log_path, kCurrentTailBytes);
      if (prefer_previous) {
        // 上次沒正常結束時，這次啟動已經把上一輪轉成 .1。問題在那一份，不要只送這次開頭的幾行。
        auto prev = read_tail(log_path + ".1", kCurrentTailBytes);
        if (!prev.text.empty()) {
          return redact(prev.text, secrets);
        }
        BOOST_LOG(info) << "[VIPLE-LOGDROP] previous log unavailable, using current";
        return redact(current.text, secrets);
      }
      std::string previous;
      if (current.size < kSmallCurrentBytes) {
        auto prev = read_tail(log_path + ".1", kPreviousTailBytes);
        if (!prev.text.empty()) {
          previous = redact(prev.text, secrets);
        }
      }
      std::string cur = redact(current.text, secrets);
      if (previous.empty()) {
        return cur;
      }
      if (previous.back() != '\n') {
        previous.push_back('\n');
      }
      previous += "----- [VIPLE-LOGDROP] previous log above / current log below -----\n";
      previous += cur;
      return previous;
    }

    void add_secret(std::vector<std::string> &out, std::string value) {
      if (value.size() < 3) {
        return;
      }
      for (const auto &existing : out) {
        if (existing.size() == value.size() && ieq_at(existing, 0, value)) {
          return;
        }
      }
      out.push_back(std::move(value));
    }

    std::vector<std::string> collect_secrets() {
      std::vector<std::string> secrets;
      add_secret(secrets, config::nvhttp.sunshine_name);
      // platf::get_host_name() 失敗會回 Sunshine／VipleStream，拿去當祕密字會把產品名稱全部遮掉。
      try {
        add_secret(secrets, boost::asio::ip::host_name());
      } catch (...) {
      }
#ifdef _WIN32
      if (const char *user = std::getenv("USERNAME"); user != nullptr && user[0] != '\0') {
        add_secret(secrets, user);
      }
#else
      if (const char *user = std::getenv("USER"); user != nullptr && user[0] != '\0') {
        add_secret(secrets, user);
      }
#endif
      // 公開的 get_all_clients() 已有名稱；讀不到就略過，不要為了這個改 nvhttp 的介面。
      try {
        const auto clients = nvhttp::get_all_clients();
        if (clients.is_array()) {
          for (const auto &client : clients) {
            if (!client.is_object()) {
              continue;
            }
            const auto it = client.find("name");
            if (it != client.end() && it->is_string()) {
              add_secret(secrets, it->get<std::string>());
            }
          }
        }
      } catch (...) {
      }
      return secrets;
    }

    std::string device_string() {
#ifdef _WIN32
      // 專案裡沒有現成的 RtlGetVersion 包裝；這裡不加新的 Windows API。
      return "Windows";
#else
      utsname info {};
      if (uname(&info) != 0) {
        return "Linux";
      }
      return clip_utf16_units(std::string(info.sysname) + " " + info.release, kDeviceChars);
#endif
    }

    std::string app_version_string() {
      // 協定上限 64。正常版本很短；裁掉是為了自訂版本字串也不會被收件端拒掉。
      std::string ver {PROJECT_VERSION};
      if (ver.size() > 64) {
        ver.resize(64);
      }
      return ver;
    }

    struct response_buf {
      std::string body;
      bool overflow = false;
    };

    std::size_t curl_write(char *ptr, std::size_t size, std::size_t nmemb, void *userdata) {
      auto *buf = static_cast<response_buf *>(userdata);
      if (size != 0 && nmemb > (std::numeric_limits<std::size_t>::max)() / size) {
        buf->overflow = true;
        return 0;
      }
      const auto n = size * nmemb;
      if (buf->body.size() + n > kResponseCap) {
        buf->overflow = true;
        return 0;
      }
      buf->body.append(ptr, n);
      return n;
    }

    result_t parse_response(const std::string &body) {
      nlohmann::json parsed;
      try {
        parsed = nlohmann::json::parse(body);
      } catch (...) {
        return {false, {}, "bad_response"};
      }
      if (!parsed.is_object() || !parsed.contains("ok") || !parsed["ok"].is_boolean()) {
        return {false, {}, "bad_response"};
      }
      if (parsed["ok"].get<bool>()) {
        if (!parsed.contains("code") || !parsed["code"].is_string()) {
          return {false, {}, "bad_response"};
        }
        auto code = parsed["code"].get<std::string>();
        if (code.empty()) {
          return {false, {}, "bad_response"};
        }
        return {true, std::move(code), {}};
      }
      if (!parsed.contains("error") || !parsed["error"].is_string()) {
        return {false, {}, "bad_response"};
      }
      auto error = parsed["error"].get<std::string>();
      if (error.empty()) {
        return {false, {}, "bad_response"};
      }
      return {false, {}, std::move(error)};
    }

    std::string clip_log_text(std::string_view text) {
      constexpr std::size_t kMax = 200;
      if (text.size() <= kMax) {
        return std::string {text};
      }
      return std::string {text.substr(0, kMax)};
    }

    bool valid_trigger(std::string_view trigger) {
      if (trigger.empty() || trigger.size() > 32) {
        return false;
      }
      for (const char c : trigger) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok) {
          return false;
        }
      }
      return true;
    }

    std::string log_trigger(std::string_view trigger) {
      if (valid_trigger(trigger)) {
        return std::string {trigger};
      }
      return "invalid";
    }

    void log_result(const result_t &result, std::size_t sent, bool have_curl, CURLcode rc, std::string_view trigger) {
      if (result.ok) {
        BOOST_LOG(info) << "[VIPLE-LOGDROP] uploaded code=" << result.code << " bytes=" << sent << " trigger=" << log_trigger(trigger);
        return;
      }
      if (have_curl && rc != CURLE_OK) {
        BOOST_LOG(warning) << "[VIPLE-LOGDROP] upload failed: " << clip_log_text(result.error) << " (" << curl_easy_strerror(rc) << ")";
        return;
      }
      BOOST_LOG(warning) << "[VIPLE-LOGDROP] upload failed: " << clip_log_text(result.error);
    }

    struct curl_cleanup {
      CURL *curl = nullptr;
      curl_slist *headers = nullptr;

      curl_cleanup() = default;
      curl_cleanup(const curl_cleanup &) = delete;
      curl_cleanup &operator=(const curl_cleanup &) = delete;

      ~curl_cleanup() {
        if (headers) {
          curl_slist_free_all(headers);
        }
        if (curl) {
          curl_easy_cleanup(curl);
        }
      }
    };

    // 跟 self_update 的 curl_common 一樣：TLS 1.2；Windows 的 MSYS2 curl 沒有 CA bundle，改用系統憑證存放區。
    void set_tls(CURL *curl) {
      curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
      curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
      // 這個行程還有別的執行緒，不能讓 curl 用訊號做逾時。
      curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
#ifdef _WIN32
      curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_NATIVE_CA));
#endif
    }

    constexpr std::string_view kInstallAlphabet = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
    constexpr int kSessionEndDelaySec = 5;
    constexpr int kCrashDelaySec = 30;
    constexpr int kMaxAutoPerDay = 3;

    std::atomic<bool> g_shutdown {false};
    std::atomic<bool> g_unclean {false};

    std::mutex g_id_mu;
    std::string g_install_id;

    std::mutex g_state_mu;
    std::string g_last_code;
    std::string g_last_time;

    std::mutex g_throttle_mu;
    std::vector<std::chrono::steady_clock::time_point> g_attempts;
    std::map<std::string, std::chrono::steady_clock::time_point> g_last_trigger;

    std::mutex g_mode_mu;
    std::string g_mode;
    bool g_mode_loaded = false;

    std::filesystem::path config_dir() {
      // sunshine_state.json 可能被指到別的目錄；匿名 ID 與執行中標記要跟它放一起。
      const std::filesystem::path state {config::nvhttp.file_state};
      if (!state.empty()) {
        const auto dir = state.parent_path();
        if (!dir.empty()) {
          return dir;
        }
      }
      return platf::appdata();
    }

    bool valid_install_id(std::string_view id) {
      return id.size() == 8 && id.find_first_not_of(kInstallAlphabet) == std::string_view::npos;
    }

    std::string trim_line(std::string s) {
      while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) {
        s.pop_back();
      }
      std::size_t i = 0;
      while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
        ++i;
      }
      if (i > 0) {
        s.erase(0, i);
      }
      return s;
    }

    std::string utc_now() {
      const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
      std::tm tm {};
#ifdef _WIN32
      if (gmtime_s(&tm, &now) != 0) {
        return {};
      }
#else
      if (gmtime_r(&now, &tm) == nullptr) {
        return {};
      }
#endif
      char buf[32] {};
      if (std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm) == 0) {
        return {};
      }
      return buf;
    }

    void remember_success(const std::string &code) {
      std::lock_guard lock {g_state_mu};
      g_last_code = code;
      g_last_time = utc_now();
    }

    std::string current_mode() {
      std::lock_guard lock {g_mode_mu};
      if (!g_mode_loaded) {
        const auto &raw = config::sunshine.log_upload;
        if (raw == "enabled" || raw == "disabled" || raw == "ask") {
          g_mode = raw;
        } else {
          g_mode = "ask";
        }
        g_mode_loaded = true;
      }
      return g_mode;
    }

    bool known_mode(std::string_view mode) {
      return mode == "ask" || mode == "enabled" || mode == "disabled";
    }

    bool is_log_upload_line(std::string_view line) {
      std::size_t i = 0;
      while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
        ++i;
      }
      constexpr std::string_view kKey = "log_upload";
      if (line.substr(i).size() < kKey.size() || line.substr(i, kKey.size()) != kKey) {
        return false;
      }
      i += kKey.size();
      while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
        ++i;
      }
      return i < line.size() && line[i] == '=';
    }

    std::string rewrite_mode(std::string_view content, std::string_view mode) {
      std::string out;
      out.reserve(content.size() + 32);
      bool replaced = false;
      std::size_t i = 0;
      while (i < content.size()) {
        const auto eol = content.find('\n', i);
        const auto line_end = eol == std::string_view::npos ? content.size() : eol + 1;
        const auto line = content.substr(i, line_end - i);
        auto core = line;
        if (!core.empty() && core.back() == '\n') {
          core.remove_suffix(1);
        }
        if (!core.empty() && core.back() == '\r') {
          core.remove_suffix(1);
        }
        if (is_log_upload_line(core)) {
          // 同一鍵寫了兩次時，解析器只留第一次。後面的重複要拿掉，不然下次啟動又讀回舊值。
          if (!replaced) {
            out += "log_upload = ";
            out.append(mode);
            out.push_back('\n');
            replaced = true;
          }
        } else {
          out.append(line);
        }
        i = line_end;
      }
      if (!replaced) {
        if (!out.empty() && out.back() != '\n') {
          out.push_back('\n');
        }
        out += "log_upload = ";
        out.append(mode);
        out.push_back('\n');
      }
      return out;
    }

    bool write_mode_file(std::string_view mode) {
      const std::filesystem::path path {config::sunshine.config_file};
      if (path.empty()) {
        return false;
      }
      std::error_code ec;
      if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), ec);
      }
      std::string content;
      {
        std::ifstream in {path, std::ios::binary};
        if (in) {
          in.seekg(0, std::ios::end);
          const auto n = in.tellg();
          if (n > 0) {
            content.resize(static_cast<std::size_t>(n));
            in.seekg(0, std::ios::beg);
            in.read(content.data(), static_cast<std::streamsize>(content.size()));
            const auto got = in.gcount();
            content.resize(static_cast<std::size_t>(got > 0 ? got : 0));
          }
        }
      }
      const auto rewritten = rewrite_mode(content, mode);
      // 先寫暫存再覆蓋：寫到一半當掉不能把整份 sunshine.conf 截掉。
      const auto tmp = path.parent_path() / (path.filename().string() + ".logdrop.tmp");
      {
        std::ofstream out {tmp, std::ios::binary | std::ios::trunc};
        if (!out) {
          std::filesystem::remove(tmp, ec);
          return false;
        }
        out << rewritten;
        out.close();
        if (!out) {
          std::filesystem::remove(tmp, ec);
          return false;
        }
      }
      // create_directories 可能留下錯誤碼。覆蓋成功時要以這次的結果為準。
      ec.clear();
      std::filesystem::copy_file(tmp, path, std::filesystem::copy_options::overwrite_existing, ec);
      std::error_code ignore;
      std::filesystem::remove(tmp, ignore);
      return !ec;
    }

    bool reserve_slot(const std::string &trigger) {
      using clock = std::chrono::steady_clock;
      const auto now = clock::now();
      std::lock_guard lock {g_throttle_mu};
      std::erase_if(g_attempts, [&](const clock::time_point t) {
        return now - t >= std::chrono::hours {24};
      });
      if (static_cast<int>(g_attempts.size()) >= kMaxAutoPerDay) {
        return false;
      }
      const auto it = g_last_trigger.find(trigger);
      if (it != g_last_trigger.end() && now - it->second < std::chrono::hours {6}) {
        return false;
      }
      g_attempts.push_back(now);
      g_last_trigger[trigger] = now;
      return true;
    }

    bool wait_or_shutdown(int seconds) {
      for (int i = 0; i < seconds; ++i) {
        if (g_shutdown.load()) {
          return false;
        }
        std::this_thread::sleep_for(std::chrono::seconds {1});
      }
      return !g_shutdown.load();
    }

  }  // namespace

  std::string redact(std::string_view text, const std::vector<std::string> &extra_secrets) {
    return apply_secrets(redact_structural(text), extra_secrets);
  }

  result_t upload(const std::string &note, const std::string &trigger, bool prefer_previous) {
    if (ENDPOINT.empty()) {
      return {false, {}, "not_configured"};
    }
    std::unique_lock<std::mutex> lock(g_upload_mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
      const result_t busy {false, {}, "busy"};
      log_result(busy, 0, false, CURLE_OK, trigger);
      return busy;
    }

    try {
      const auto secrets = collect_secrets();
      const auto payload = build_payload(config::sunshine.log_file, secrets, prefer_previous);
      // 先裁再遮：說明很長時不必掃整段。遮完短名稱會變長，所以再裁一次，才不會超過協定的 2000。
      const auto note_out = clip_utf16_units(redact(clip_utf16_units(note, kNoteChars), secrets), kNoteChars);

      nlohmann::json body = {
        {"v", 1},
        {"source", kSource},
        {"app_version", app_version_string()},
        {"device", device_string()},
        {"encoding", "text"},
        {"payload", payload},
        {"install_id", install_id()},
      };
      if (!note_out.empty()) {
        body["note"] = note_out;
      }
      if (valid_trigger(trigger)) {
        body["trigger"] = trigger;
      }
      // log 裡可能有非 UTF-8 的位元組；strict dump 會丟例外，整次上傳就失敗。
      const std::string dumped = body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
      const std::string url {ENDPOINT};

      curl_cleanup session;
      session.curl = curl_easy_init();
      if (!session.curl) {
        const result_t failed {false, {}, "network"};
        log_result(failed, dumped.size(), false, CURLE_OK, trigger);
        return failed;
      }
      session.headers = curl_slist_append(session.headers, "Content-Type: application/json");
      // 本文超過 1 KB 時 libcurl 會先等 100-continue。Apps Script 的 302 不該被這一步拖住。
      session.headers = curl_slist_append(session.headers, "Expect:");

      set_tls(session.curl);
      curl_easy_setopt(session.curl, CURLOPT_URL, url.c_str());
      curl_easy_setopt(session.curl, CURLOPT_POST, 1L);
      curl_easy_setopt(session.curl, CURLOPT_POSTFIELDS, dumped.c_str());
      curl_easy_setopt(session.curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(dumped.size()));
      curl_easy_setopt(session.curl, CURLOPT_HTTPHEADER, session.headers);
      curl_easy_setopt(session.curl, CURLOPT_FOLLOWLOCATION, 1L);
      curl_easy_setopt(session.curl, CURLOPT_MAXREDIRS, 5L);
      // 不設 CURLOPT_POSTREDIR：Apps Script 用 302 把 POST 改成 GET 才拿得到 JSON。設了會維持 POST。
      curl_easy_setopt(session.curl, CURLOPT_CONNECTTIMEOUT, 20L);
      curl_easy_setopt(session.curl, CURLOPT_TIMEOUT, 120L);
      curl_easy_setopt(session.curl, CURLOPT_WRITEFUNCTION, curl_write);
      response_buf response;
      curl_easy_setopt(session.curl, CURLOPT_WRITEDATA, &response);
#if LIBCURL_VERSION_NUM >= 0x075500
      curl_easy_setopt(session.curl, CURLOPT_PROTOCOLS_STR, "https");
      curl_easy_setopt(session.curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
      curl_easy_setopt(session.curl, CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS));
      curl_easy_setopt(session.curl, CURLOPT_REDIR_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS));
#endif

      const CURLcode rc = curl_easy_perform(session.curl);
      long http_code = 0;
      curl_easy_getinfo(session.curl, CURLINFO_RESPONSE_CODE, &http_code);

      result_t result;
      if (response.overflow) {
        result = {false, {}, "bad_response"};
      } else if (rc != CURLE_OK) {
        result = {false, {}, "network"};
      } else if (http_code != 200) {
        result = {false, {}, "http_" + std::to_string(http_code)};
      } else {
        result = parse_response(response.body);
      }
      log_result(result, dumped.size(), true, rc, trigger);
      return result;
    } catch (const std::exception &e) {
      BOOST_LOG(warning) << "[VIPLE-LOGDROP] upload failed: internal (" << clip_log_text(e.what()) << ")";
      return {false, {}, "internal"};
    } catch (...) {
      const result_t failed {false, {}, "internal"};
      log_result(failed, 0, false, CURLE_OK, trigger);
      return failed;
    }
  }

  std::string install_id() {
    std::lock_guard lock {g_id_mu};
    if (!g_install_id.empty()) {
      return g_install_id;
    }
    const auto path = config_dir() / "logdrop_id";
    {
      std::ifstream in {path, std::ios::binary};
      if (in) {
        std::string line;
        std::getline(in, line);
        line = trim_line(std::move(line));
        if (valid_install_id(line)) {
          g_install_id = std::move(line);
          return g_install_id;
        }
      }
    }
    g_install_id = crypto::rand_alphabet(8, kInstallAlphabet);
    std::error_code ec;
    if (!path.parent_path().empty()) {
      std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream out {path, std::ios::binary | std::ios::trunc};
    if (!out) {
      // 這次行程還是用記憶體裡的 ID。路徑可能含使用者名稱，不要寫進 log。
      BOOST_LOG(warning) << "[VIPLE-LOGDROP] could not store install id";
      return g_install_id;
    }
    out << g_install_id << '\n';
    if (!out) {
      BOOST_LOG(warning) << "[VIPLE-LOGDROP] could not store install id";
      return g_install_id;
    }
    BOOST_LOG(info) << "[VIPLE-LOGDROP] created install id " << g_install_id;
    return g_install_id;
  }

  bool auto_enabled() {
    return !ENDPOINT.empty() && current_mode() == "enabled";
  }

  void auto_upload_async(std::string trigger) {
    // 沒同意、或正在關機，連節流額度都不要先扣。
    if (ENDPOINT.empty() || g_shutdown.load() || !auto_enabled()) {
      return;
    }
    if (!valid_trigger(trigger)) {
      BOOST_LOG(warning) << "[VIPLE-LOGDROP] auto upload skipped: bad trigger";
      return;
    }
    if (!reserve_slot(trigger)) {
      BOOST_LOG(info) << "[VIPLE-LOGDROP] auto upload skipped: throttle trigger=" << trigger;
      return;
    }
    const bool prefer_previous = trigger == "crash";
    const int delay = trigger == "session-end" ? kSessionEndDelaySec : 0;
    try {
      std::thread([trigger = std::move(trigger), prefer_previous, delay]() {
        try {
          if (!wait_or_shutdown(delay)) {
            BOOST_LOG(info) << "[VIPLE-LOGDROP] auto upload cancelled: shutdown trigger=" << trigger;
            return;
          }
          if (!auto_enabled()) {
            BOOST_LOG(info) << "[VIPLE-LOGDROP] auto upload skipped: disabled trigger=" << trigger;
            return;
          }
          const auto result = upload("", trigger, prefer_previous);
          if (result.ok) {
            remember_success(result.code);
          }
        } catch (const std::exception &e) {
          BOOST_LOG(warning) << "[VIPLE-LOGDROP] auto upload failed: " << clip_log_text(e.what());
        } catch (...) {
          BOOST_LOG(warning) << "[VIPLE-LOGDROP] auto upload failed: internal";
        }
      }).detach();
    } catch (const std::exception &e) {
      BOOST_LOG(warning) << "[VIPLE-LOGDROP] auto upload failed: " << clip_log_text(e.what());
    }
  }

  status_t status() {
    status_t out;
    out.configured = !ENDPOINT.empty();
    out.mode = current_mode();
    out.install_id = install_id();
    std::lock_guard lock {g_state_mu};
    out.last_code = g_last_code;
    out.last_time = g_last_time;
    return out;
  }

  bool persist_mode(std::string_view mode) {
    if (!known_mode(mode)) {
      return false;
    }
    // 檔案與記憶體一起改。只鎖模式，不跟上傳鎖互相等。
    std::lock_guard lock {g_mode_mu};
    if (!write_mode_file(mode)) {
      BOOST_LOG(warning) << "[VIPLE-LOGDROP] could not store log_upload";
      return false;
    }
    g_mode = std::string {mode};
    g_mode_loaded = true;
    config::sunshine.log_upload = g_mode;
    BOOST_LOG(info) << "[VIPLE-LOGDROP] log_upload=" << g_mode;
    return true;
  }

  void mark_process_start() {
    const auto path = config_dir() / "logdrop_running";
    std::error_code ec;
    const bool existed = std::filesystem::exists(path, ec);
    g_unclean.store(existed && !ec);
    if (!path.parent_path().empty()) {
      std::filesystem::create_directories(path.parent_path(), ec);
    }
    std::ofstream out {path, std::ios::binary | std::ios::trunc};
    if (!out) {
      BOOST_LOG(warning) << "[VIPLE-LOGDROP] could not write running marker";
    }
  }

  void startup_complete() {
    if (!g_unclean.load()) {
      return;
    }
    if (ENDPOINT.empty()) {
      BOOST_LOG(info) << "[VIPLE-LOGDROP] previous exit was unclean; upload disabled: endpoint empty";
      return;
    }
    BOOST_LOG(info) << "[VIPLE-LOGDROP] previous exit was unclean; uploading in 30s";
    try {
      std::thread([]() {
        try {
          // 等啟動 log 寫完，也讓使用者若在這段時間按同意，送的時候看得到。
          if (!wait_or_shutdown(kCrashDelaySec)) {
            return;
          }
          auto_upload_async("crash");
        } catch (const std::exception &e) {
          BOOST_LOG(warning) << "[VIPLE-LOGDROP] auto upload failed: " << clip_log_text(e.what());
        } catch (...) {
          BOOST_LOG(warning) << "[VIPLE-LOGDROP] auto upload failed: internal";
        }
      }).detach();
    } catch (const std::exception &e) {
      BOOST_LOG(warning) << "[VIPLE-LOGDROP] auto upload failed: " << clip_log_text(e.what());
    }
  }

  void mark_clean_exit() {
    // 先讓還在等的自動上傳停下來，再拿掉標記。順序反了的話，關機途中的串流結束仍會送出。
    g_shutdown.store(true);
    std::error_code ec;
    std::filesystem::remove(config_dir() / "logdrop_running", ec);
    if (ec) {
      BOOST_LOG(warning) << "[VIPLE-LOGDROP] could not remove running marker";
    }
  }

}  // namespace logdrop
