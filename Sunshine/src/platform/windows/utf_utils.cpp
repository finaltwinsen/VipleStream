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

  std::string to_utf8(const std::wstring &string) {
    // No conversion needed if the string is empty
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
}  // namespace utf_utils
