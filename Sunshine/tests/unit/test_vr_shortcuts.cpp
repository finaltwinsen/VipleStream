/**
 * @file tests/unit/test_vr_shortcuts.cpp
 * @brief §VR-SHORTCUT：vr_shortcuts 字串的解析。
 */
#include "../tests_common.h"

#include <string>
#include <vector>

#include <src/vr/vr_shortcuts.h>

using vr::shortcuts::entry_t;
using vr::shortcuts::pad_e;

namespace {
  struct parsed_t {
    std::vector<entry_t> entries;
    std::vector<std::string> warnings;
  };

  parsed_t parse(std::string_view text) {
    parsed_t out;
    out.entries = vr::shortcuts::parse_warned(text, [&](std::string_view message) {
      out.warnings.emplace_back(message);
    });
    return out;
  }
}  // namespace

TEST(VrShortcuts, AppAndArgsDefaultPad) {
  const auto p = parse("593600 -pixarkvr");
  ASSERT_EQ(p.entries.size(), 1u);
  EXPECT_TRUE(p.warnings.empty());
  EXPECT_EQ(p.entries[0].app_id, "593600");
  EXPECT_EQ(p.entries[0].pad, pad_e::automatic);
  EXPECT_EQ(p.entries[0].args, "-pixarkvr");
}

TEST(VrShortcuts, PadAndArgs) {
  const auto p = parse("593600,vr -pixarkvr");
  ASSERT_EQ(p.entries.size(), 1u);
  EXPECT_TRUE(p.warnings.empty());
  EXPECT_EQ(p.entries[0].pad, pad_e::vr);
  EXPECT_EQ(p.entries[0].args, "-pixarkvr");
  EXPECT_STREQ(vr::shortcuts::pad_name(p.entries[0].pad), "vr");
}

TEST(VrShortcuts, MultipleEntriesAndXboxWithoutArgs) {
  const auto p = parse("593600 -pixarkvr; 12345,xbox");
  ASSERT_EQ(p.entries.size(), 2u);
  EXPECT_TRUE(p.warnings.empty());
  EXPECT_EQ(p.entries[0].app_id, "593600");
  EXPECT_EQ(p.entries[0].pad, pad_e::automatic);
  EXPECT_EQ(p.entries[0].args, "-pixarkvr");
  EXPECT_EQ(p.entries[1].app_id, "12345");
  EXPECT_EQ(p.entries[1].pad, pad_e::xbox);
  EXPECT_TRUE(p.entries[1].args.empty());
}

TEST(VrShortcuts, WhitespaceAndEmptySegments) {
  EXPECT_TRUE(parse("").entries.empty());
  EXPECT_TRUE(parse("").warnings.empty());
  EXPECT_TRUE(parse("   ").entries.empty());
  EXPECT_TRUE(parse(" ; ; ").entries.empty());
  EXPECT_TRUE(parse(" ; ; ").warnings.empty());

  const auto p = parse("  593600,vr -pixarkvr  ;  ; 12345,xbox  ");
  ASSERT_EQ(p.entries.size(), 2u);
  EXPECT_TRUE(p.warnings.empty());
  EXPECT_EQ(p.entries[0].args, "-pixarkvr");
  EXPECT_EQ(p.entries[1].pad, pad_e::xbox);

  const auto spaced = parse("593600 ,vr");
  ASSERT_EQ(spaced.entries.size(), 1u);
  EXPECT_EQ(spaced.entries[0].pad, pad_e::vr);
  EXPECT_TRUE(spaced.entries[0].args.empty());

  const auto after = parse("593600, vr -x");
  ASSERT_EQ(after.entries.size(), 1u);
  EXPECT_EQ(after.entries[0].pad, pad_e::vr);
  EXPECT_EQ(after.entries[0].args, "-x");
}

TEST(VrShortcuts, ArgsMayContainComma) {
  const auto p = parse("593600 -foo,bar");
  ASSERT_EQ(p.entries.size(), 1u);
  EXPECT_TRUE(p.warnings.empty());
  EXPECT_EQ(p.entries[0].pad, pad_e::automatic);
  EXPECT_EQ(p.entries[0].args, "-foo,bar");

  const auto symbols = parse("593600 -log=1 +foo/bar:2");
  ASSERT_EQ(symbols.entries.size(), 1u);
  EXPECT_EQ(symbols.entries[0].args, "-log=1 +foo/bar:2");
}

TEST(VrShortcuts, InvalidAppId) {
  for (const char *text : {"abc", "12345678901", "593600foo", "12345678901 -x"}) {
    const auto p = parse(text);
    EXPECT_TRUE(p.entries.empty()) << text;
    ASSERT_EQ(p.warnings.size(), 1u) << text;
    EXPECT_NE(p.warnings[0].find("invalid app id"), std::string::npos) << p.warnings[0];
  }

  const auto zero = parse("0");
  ASSERT_EQ(zero.entries.size(), 1u);
  EXPECT_EQ(zero.entries[0].app_id, "0");
  EXPECT_TRUE(vr::shortcuts::app_id_ok("0"));
  EXPECT_TRUE(vr::shortcuts::app_id_ok("1234567890"));
  EXPECT_FALSE(vr::shortcuts::app_id_ok(""));
  EXPECT_FALSE(vr::shortcuts::app_id_ok("12345678901"));
  EXPECT_FALSE(vr::shortcuts::app_id_ok("12a"));
}

TEST(VrShortcuts, InvalidPad) {
  const auto stuck = parse("593600,vr-pixarkvr");
  EXPECT_TRUE(stuck.entries.empty());
  ASSERT_EQ(stuck.warnings.size(), 1u);
  EXPECT_NE(stuck.warnings[0].find("invalid pad"), std::string::npos);

  const auto upper = parse("593600,VR");
  EXPECT_TRUE(upper.entries.empty());
  EXPECT_NE(upper.warnings[0].find("invalid pad"), std::string::npos);

  const auto empty_pad = parse("593600,");
  EXPECT_TRUE(empty_pad.entries.empty());
  EXPECT_NE(empty_pad.warnings[0].find("invalid pad"), std::string::npos);
}

TEST(VrShortcuts, InvalidCharactersAndLength) {
  const auto bad = parse("593600 hello&world");
  EXPECT_TRUE(bad.entries.empty());
  ASSERT_EQ(bad.warnings.size(), 1u);
  EXPECT_NE(bad.warnings[0].find("invalid characters in args"), std::string::npos);

  const auto uni = parse("593600 \xe9" "\x81" "\x8a" "\xe6" "\x88" "\xb2");
  EXPECT_TRUE(uni.entries.empty());
  EXPECT_NE(uni.warnings[0].find("invalid characters in args"), std::string::npos);

  const std::string ok = std::string("593600 ") + std::string(200, 'a');
  const auto long_ok = parse(ok);
  ASSERT_EQ(long_ok.entries.size(), 1u);
  EXPECT_TRUE(long_ok.warnings.empty());
  EXPECT_EQ(long_ok.entries[0].args.size(), 200u);
  EXPECT_TRUE(vr::shortcuts::args_ok(std::string(200, 'b')));
  EXPECT_FALSE(vr::shortcuts::args_ok(std::string(201, 'b')));
  EXPECT_FALSE(vr::shortcuts::args_ok("a&b"));
  EXPECT_TRUE(vr::shortcuts::args_ok(""));
  EXPECT_TRUE(vr::shortcuts::args_ok("-pixarkvr"));

  const std::string too = std::string("593600 ") + std::string(201, 'a');
  const auto long_bad = parse(too);
  EXPECT_TRUE(long_bad.entries.empty());
  ASSERT_EQ(long_bad.warnings.size(), 1u);
  EXPECT_NE(long_bad.warnings[0].find("args longer than 200"), std::string::npos);
}

TEST(VrShortcuts, DuplicateKeepsFirstWithoutWarning) {
  const auto p = parse("593600,vr -a; 593600,xbox -b");
  ASSERT_EQ(p.entries.size(), 1u);
  EXPECT_TRUE(p.warnings.empty());
  EXPECT_EQ(p.entries[0].pad, pad_e::vr);
  EXPECT_EQ(p.entries[0].args, "-a");

  const auto zeros = parse("0593600,vr; 593600,xbox");
  ASSERT_EQ(zeros.entries.size(), 2u);
  EXPECT_EQ(zeros.entries[0].app_id, "0593600");
  EXPECT_EQ(zeros.entries[1].app_id, "593600");
}

TEST(VrShortcuts, WarningSnipIsRawFortyChars) {
  const std::string seg = std::string("  \x01") + std::string(50, 'Z');
  const auto p = parse(seg);
  EXPECT_TRUE(p.entries.empty());
  ASSERT_EQ(p.warnings.size(), 1u);
  const std::string expect = std::string("[VIPLE-VR-SHORTCUT] ignored entry \"") + "  ?" + std::string(37, 'Z') + "\": invalid app id";
  EXPECT_EQ(p.warnings[0], expect);

  const auto mixed = parse("593600 -ok;bad;7,xbox");
  ASSERT_EQ(mixed.entries.size(), 2u);
  ASSERT_EQ(mixed.warnings.size(), 1u);
  EXPECT_EQ(mixed.entries[0].app_id, "593600");
  EXPECT_EQ(mixed.entries[1].app_id, "7");
  EXPECT_NE(mixed.warnings[0].find("ignored entry \"bad\""), std::string::npos);
}

TEST(VrShortcuts, PadForIsSilent) {
  EXPECT_EQ(vr::shortcuts::pad_for("593600,vr -x", "593600"), std::optional<bool> {false});
  EXPECT_EQ(vr::shortcuts::pad_for("593600,xbox", "593600"), std::optional<bool> {true});
  EXPECT_FALSE(vr::shortcuts::pad_for("593600 -x", "593600").has_value());
  EXPECT_FALSE(vr::shortcuts::pad_for("593600,vr -x", "1").has_value());
  EXPECT_FALSE(vr::shortcuts::pad_for("593600,vr -x", "").has_value());
  EXPECT_EQ(vr::shortcuts::pad_for("nope; 593600,vr -x", "593600"), std::optional<bool> {false});
}
