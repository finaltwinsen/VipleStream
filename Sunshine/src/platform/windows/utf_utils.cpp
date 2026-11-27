/**
 * @file src/platform/windows/utf_utils.cpp
 * @brief Minimal UTF conversion utilities for Windows tools
 */
#include "utf_utils.h"

#include "src/logging.h"

#include <limits>
#include <string>
#include <Windows.h>

using namespace std::literals;

namespace utf_utils {
  std::wstring from_utf8(const std::string &string) {
    // No conversion needed if the string is empty
    if (string.empty()) {
      return {};
    }

    if (string.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
      BOOST_LOG(error) << "UTF-8 string exceeds maximum supported size for conversion"sv;
      return {};
    }

    const auto input_len = static_cast<int>(string.size());

    // Get the output size required to store the string
    auto output_size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, string.data(), input_len, nullptr, 0);
    if (output_size == 0) {
      auto winerr = GetLastError();
      BOOST_LOG(error) << "Failed to get UTF-16 buffer size: "sv << winerr;
      return {};
    }

    // Perform the conversion
    std::wstring output(output_size, L'\0');
    output_size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, string.data(), input_len, output.data(), static_cast<int>(output.size()));
    if (output_size == 0) {
      auto winerr = GetLastError();
      BOOST_LOG(error) << "Failed to convert string to UTF-16: "sv << winerr;
      return {};
    }

    if (static_cast<std::size_t>(output_size) < output.size()) {
      output.resize(output_size);
    }

    return output;
  }

  std::string to_utf8(const std::wstring &string) {    // No conversion needed if the string is empty
    if (string.empty()) {
      return {};
    }

    if (string.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
      BOOST_LOG(error) << "UTF-16 string exceeds maximum supported size for conversion"sv;
      return {};
    }

    const auto input_len = static_cast<int>(string.size());

    // Get the output size required to store the string
    auto output_size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, string.data(), input_len, nullptr, 0, nullptr, nullptr);
    if (output_size == 0) {
      auto winerr = GetLastError();
      BOOST_LOG(error) << "Failed to get UTF-8 buffer size: "sv << winerr;
      return {};
    }

    // Perform the conversion
    std::string output(output_size, '\0');
    output_size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, string.data(), input_len, output.data(), static_cast<int>(output.size()), nullptr, nullptr);
    if (output_size == 0) {
      auto winerr = GetLastError();
      BOOST_LOG(error) << "Failed to convert string to UTF-8: "sv << winerr;
      return {};
    }

    if (static_cast<std::size_t>(output_size) < output.size()) {
      output.resize(output_size);
    }

    return output;
  }

  std::string from_acp(const std::string &string) {
    // §LOGDROP-1127：Windows 系統錯誤字串（FormatMessageA／boost system_category）是 ANSI（例如 cp950），
    // 直接寫進 UTF-8 日誌會變亂碼；先轉成寬字元再交給 to_utf8。
    if (string.empty()) {
      return {};
    }

    if (string.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
      return {};
    }

    const auto input_len = static_cast<int>(string.size());
    auto output_size = MultiByteToWideChar(CP_ACP, 0, string.data(), input_len, nullptr, 0);
    if (output_size <= 0) {
      return string;  // 轉不了就原樣回傳，至少不要讓呼叫端拿到空字串
    }

    std::wstring output(static_cast<std::size_t>(output_size), L'\0');
    output_size = MultiByteToWideChar(CP_ACP, 0, string.data(), input_len, output.data(), static_cast<int>(output.size()));
    if (output_size <= 0) {
      return string;
    }
    if (static_cast<std::size_t>(output_size) < output.size()) {
      output.resize(static_cast<std::size_t>(output_size));
    }

    return to_utf8(output);
  }
}  // namespace utf_utils
