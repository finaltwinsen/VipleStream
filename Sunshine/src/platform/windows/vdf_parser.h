/**
 * @file src/platform/windows/vdf_parser.h
 * @brief Minimal Valve KeyValues (VDF) parser for Steam config files.
 *
 * Only supports the subset actually used by Steam's on-disk configs:
 *   "key"   "string_value"
 *   "parent" { nested ... }
 *   // line comments (rare but show up in loginusers.vdf)
 *   escape sequences: \n  \t  \\  \"
 *
 * Does NOT handle:
 *   - unquoted tokens (ACF / libraryfolders all use quoted keys/values)
 *   - conditional blocks [$OSNAME]
 *   - #base / #include preprocessor directives
 *
 * Header-only so we don't add a .cpp file to the Windows build just for this.
 *
 * Intentionally hand-rolled rather than pulling in a dependency — this is
 * 120 lines of pure logic, reviewable at once, and we control it.
 */
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace viple::vdf {

  // A VDF value is either a leaf string or a map of child nodes.
  // `ordered_map` is not available in the standard; Steam's format is
  // technically order-preserving (appmanifest keys have a documented
  // order) but nothing we read depends on order, so std::map is fine.
  class Node;
  using NodeMap = std::map<std::string, Node>;

  class Node {
  public:
    Node() = default;

    Node(std::string leaf):
        _val(std::move(leaf)) {
    }

    Node(NodeMap children):
        _val(std::move(children)) {
    }

    bool is_leaf() const {
      return std::holds_alternative<std::string>(_val);
    }

    bool is_map() const {
      return std::holds_alternative<NodeMap>(_val);
    }

    const std::string &as_string() const {
      return std::get<std::string>(_val);
    }

    const NodeMap &as_map() const {
      return std::get<NodeMap>(_val);
    }

    NodeMap &as_map() {
      return std::get<NodeMap>(_val);
    }

    /**
     * Case-insensitive lookup of a child node. Steam files mix casing
     * ("appid" vs "AppID" between Steam versions); callers shouldn't
     * care.
     * @return pointer to child Node, or nullptr if absent / this is a leaf.
     */
    const Node *child(std::string_view key) const {
      if (!is_map()) {
        return nullptr;
      }
      for (const auto &[k, v] : as_map()) {
        if (k.size() == key.size() &&
            std::equal(k.begin(), k.end(), key.begin(), [](char a, char b) {
              return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
            })) {
          return &v;
        }
      }
      return nullptr;
    }

    /**
     * Chain child() lookups. Returns nullptr on first miss.
     * Example: `node.path({"UserLocalConfigStore","Software","Valve","Steam","apps"})`
     */
    const Node *path(std::initializer_list<std::string_view> segs) const {
      const Node *cur = this;
      for (auto seg : segs) {
        if (!cur) {
          return nullptr;
        }
        cur = cur->child(seg);
      }
      return cur;
    }

    /**
     * Leaf value as string; empty if absent or not a leaf.
     */
    std::string leaf_or(std::string_view key, std::string fallback = {}) const {
      const auto *c = child(key);
      if (c && c->is_leaf()) {
        return c->as_string();
      }
      return fallback;
    }

  private:
    std::variant<std::string, NodeMap> _val;
  };

  namespace detail {
    struct Lexer {
      std::string_view text;
      size_t pos = 0;

      void skip_ws() {
        while (pos < text.size()) {
          char c = text[pos];
          if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            pos++;
          } else if (c == '/' && pos + 1 < text.size() && text[pos + 1] == '/') {
            // line comment
            while (pos < text.size() && text[pos] != '\n') {
              pos++;
            }
          } else {
            break;
          }
        }
      }

      /** Read "quoted string", returning just the content. Advances pos
       *  past the closing quote. Returns nullopt on malformed input. */
      std::optional<std::string> read_quoted() {
        if (pos >= text.size() || text[pos] != '"') {
          return std::nullopt;
        }
        pos++;  // skip opening "
        std::string out;
        while (pos < text.size() && text[pos] != '"') {
          char c = text[pos];
          if (c == '\\' && pos + 1 < text.size()) {
            char next = text[pos + 1];
            switch (next) {
              case 'n':
                out.push_back('\n');
                break;
              case 't':
                out.push_back('\t');
                break;
              case '\\':
                out.push_back('\\');
                break;
              case '"':
                out.push_back('"');
                break;
              default:
                out.push_back(next);
                break;
            }
            pos += 2;
          } else {
            out.push_back(c);
            pos++;
          }
        }
        if (pos >= text.size()) {
          return std::nullopt;
        }
        pos++;  // skip closing "
        return out;
      }
    };

    /** Parse a sequence of "key" {value|{block}} pairs until EOF or a
     *  closing '}'. */
    inline std::optional<NodeMap> parse_body(Lexer &lex) {
      NodeMap out;
      while (true) {
        lex.skip_ws();
        if (lex.pos >= lex.text.size() || lex.text[lex.pos] == '}') {
          break;
        }
        auto key = lex.read_quoted();
        if (!key) {
          return std::nullopt;
        }
        lex.skip_ws();
        if (lex.pos >= lex.text.size()) {
          return std::nullopt;
        }
        if (lex.text[lex.pos] == '{') {
          lex.pos++;
          auto children = parse_body(lex);
          if (!children) {
            return std::nullopt;
          }
          lex.skip_ws();
          if (lex.pos >= lex.text.size() || lex.text[lex.pos] != '}') {
            return std::nullopt;
          }
          lex.pos++;  // closing }
          out.emplace(std::move(*key), Node {std::move(*children)});
        } else if (lex.text[lex.pos] == '"') {
          auto val = lex.read_quoted();
          if (!val) {
            return std::nullopt;
          }
          out.emplace(std::move(*key), Node {std::move(*val)});
        } else {
          // Unexpected token
          return std::nullopt;
        }
      }
      return out;
    }
  }  // namespace detail

  /**
   * Parse a full VDF document. Returns root as a map-Node.
   * Top-level is treated like `{ ... }` body (zero-or-more top-level
   * key/value pairs — Steam's loginusers/libraryfolders/localconfig etc
   * all have exactly ONE top-level key but this parser doesn't enforce
   * that).
   */
  inline std::optional<Node> parse(std::string_view text) {
    detail::Lexer lex {text};
    auto body = detail::parse_body(lex);
    if (!body) {
      return std::nullopt;
    }
    return Node {std::move(*body)};
  }

}  // namespace viple::vdf
