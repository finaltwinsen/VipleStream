/**
 * @file src/vr/vr_shortcuts.h
 * @brief §VR-SHORTCUT：把不在 steamapps.vrmanifest 裡的 Steam 遊戲當成 VR 遊戲，並帶自訂啟動參數。
 *
 * 設定 `vr_shortcuts`（預設空）是以 `;` 分隔的項目，每項 `<appid>[,<pad>] [<args>]`。
 * `<pad>`＝`auto`（預設，照既有 §VR-PADMODE）｜`vr`（一律 VR 手把）｜`xbox`（一律手把模式）。
 * 例：`593600,vr -pixarkvr`、`593600 -pixarkvr; 12345,xbox`。
 *
 * 純函式、不讀設定檔。語法警告只該在套用設定時印一次；清單重掃與手把輪詢要傳空的 warn，
 * 否則每 500 ms 會把同一行再印一次。
 */
#pragma once

#include <cstddef>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>


namespace vr::shortcuts {

  /// `auto` 是關鍵字，所以列舉叫 automatic；寫進 log／ctx 時仍是 "auto"
  enum class pad_e {
    automatic,
    vr,
    xbox,
  };

  struct entry_t {
    std::string app_id;
    pad_e pad = pad_e::automatic;
    std::string args;
  };

  /// 1～10 位數字。不去掉前導 0（`0593600` 與 `593600` 是不同的 id）
  inline bool app_id_ok(std::string_view id) {
    if (id.empty() || id.size() > 10) {
      return false;
    }
    for (const char c : id) {
      if (c < '0' || c > '9') {
        return false;
      }
    }
    return true;
  }

  /// 參數字元集 `[A-Za-z0-9 _.,:=+/\-]`，長度 ≤ 200。空字串合法（只把遊戲標成 VR）
  inline bool args_ok(std::string_view args) {
    if (args.size() > 200) {
      return false;
    }
    for (const unsigned char c : args) {
      const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' || c == '_' || c == '.' || c == ',' || c == ':' || c == '=' || c == '+' || c == '/' || c == '-';
      if (!ok) {
        return false;
      }
    }
    return true;
  }

  inline const char *pad_name(pad_e pad) {
    switch (pad) {
      case pad_e::vr:
        return "vr";
      case pad_e::xbox:
        return "xbox";
      case pad_e::automatic:
        return "auto";
    }
    return "auto";
  }

  /// warn 收到的是整行 log（含 tag）。空的 warn＝安靜略過。callback 不可把 string_view 留到呼叫結束之後
  inline std::vector<entry_t> parse_warned(std::string_view text, const std::function<void(std::string_view)> &warn) {
    const auto warn_entry = [&](std::string_view raw, std::string_view reason) {
      if (!warn) {
        return;
      }
      std::string snip;
      const std::size_t n = raw.size() < 40 ? raw.size() : 40;
      snip.reserve(n);
      for (std::size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(raw[i]);
        // 控制字元換成 '?'，警告才維持一行
        snip.push_back(c < 0x20 || c == 0x7f ? '?' : static_cast<char>(c));
      }
      warn(std::format("[VIPLE-VR-SHORTCUT] ignored entry \"{}\": {}", snip, reason));
    };

    const auto is_ws = [](char c) {
      return c == ' ' || c == '\t';
    };
    const auto trim = [&](std::string_view s) {
      while (!s.empty() && is_ws(s.front())) {
        s.remove_prefix(1);
      }
      while (!s.empty() && is_ws(s.back())) {
        s.remove_suffix(1);
      }
      return s;
    };

    std::vector<entry_t> out;
    std::size_t begin = 0;
    while (begin < text.size()) {
      const auto semi = text.find(';', begin);
      const auto raw = text.substr(begin, semi == std::string_view::npos ? std::string_view::npos : semi - begin);
      begin = semi == std::string_view::npos ? text.size() : semi + 1;

      const auto body = trim(raw);
      if (body.empty()) {
        continue;  // 多餘的分號、空白項：不警告
      }

      std::size_t i = 0;
      while (i < body.size() && body[i] >= '0' && body[i] <= '9') {
        ++i;
      }
      // 超過 10 位不算「10 位 id 加上參數」，整項是不合法的 app id
      if (i == 0 || i > 10) {
        warn_entry(raw, "invalid app id");
        continue;
      }
      const std::string app_id {body.substr(0, i)};
      auto rest = body.substr(i);
      while (!rest.empty() && is_ws(rest.front())) {
        rest.remove_prefix(1);
      }

      auto pad = pad_e::automatic;
      std::string args;
      if (!rest.empty() && rest.front() == ',') {
        rest.remove_prefix(1);
        while (!rest.empty() && is_ws(rest.front())) {
          rest.remove_prefix(1);
        }
        std::size_t j = 0;
        while (j < rest.size() && !is_ws(rest[j])) {
          ++j;
        }
        const auto token = rest.substr(0, j);
        rest.remove_prefix(j);
        if (token == "auto") {
          pad = pad_e::automatic;
        } else if (token == "vr") {
          pad = pad_e::vr;
        } else if (token == "xbox") {
          pad = pad_e::xbox;
        } else {
          warn_entry(raw, "invalid pad");
          continue;
        }
        if (!rest.empty() && !is_ws(rest.front())) {
          warn_entry(raw, "invalid pad");
          continue;
        }
        while (!rest.empty() && is_ws(rest.front())) {
          rest.remove_prefix(1);
        }
        args.assign(rest);
      } else if (!rest.empty()) {
        // `593600foo` 不是參數：app id 後面只能是結尾、空白或逗號
        if (body[i] != ' ' && body[i] != '\t') {
          warn_entry(raw, "invalid app id");
          continue;
        }
        args.assign(rest);
      }

      if (args.size() > 200) {
        warn_entry(raw, "args longer than 200");
        continue;
      }
      if (!args_ok(args)) {
        warn_entry(raw, "invalid characters in args");
        continue;
      }

      bool dup = false;
      for (const auto &e : out) {
        if (e.app_id == app_id) {
          dup = true;
          break;
        }
      }
      if (dup) {
        continue;  // 同一個 app id 取第一個，不警告
      }
      out.push_back(entry_t {app_id, pad, std::move(args)});
    }
    return out;
  }

  /**
   * @brief 這個 app id 的手把覆寫。true＝xbox、false＝VR 手把、nullopt＝auto、不在清單、或 id 是空的。
   *        不印警告（手把輪詢會一直呼叫）。
   */
  inline std::optional<bool> pad_for(std::string_view text, std::string_view app_id) {
    if (app_id.empty()) {
      return std::nullopt;
    }
    for (const auto &e : parse_warned(text, {})) {
      if (e.app_id == app_id) {
        if (e.pad == pad_e::vr) {
          return false;
        }
        if (e.pad == pad_e::xbox) {
          return true;
        }
        return std::nullopt;
      }
    }
    return std::nullopt;
  }

}  // namespace vr::shortcuts
