/**
 * @file tests/unit/test_logdrop.cpp
 * @brief §LOGDROP：redact 的規則。改了遮法這裡會先失敗。
 */
#include "../tests_common.h"

#include <string>
#include <utility>
#include <vector>

#include <src/logdrop.h>

namespace {
  std::string redacted(std::string_view text, std::vector<std::string> secrets = {}) {
    return logdrop::redact(text, secrets);
  }
}  // namespace

TEST(LogdropRedact, Ipv4PrivateAndPublic) {
  const std::pair<const char *, const char *> cases[] = {
    {"10.1.2.3", "10.1.x.x"},
    {"10.255.0.1", "10.255.x.x"},
    {"172.16.5.5", "172.16.x.x"},
    {"172.31.255.1", "172.31.x.x"},
    {"172.15.1.1", "<ip>"},
    {"172.32.1.1", "<ip>"},
    {"192.168.1.20", "192.168.x.x"},
    {"192.168.001.002", "192.168.x.x"},
    {"192.169.1.1", "<ip>"},
    {"100.64.0.1", "100.64.x.x"},
    {"100.127.255.1", "100.127.x.x"},
    {"100.63.1.1", "<ip>"},
    {"100.128.1.1", "<ip>"},
    {"169.254.10.20", "169.254.x.x"},
    {"169.253.1.1", "<ip>"},
    {"169.255.1.1", "<ip>"},
    {"1192.168.0.1", "1192.168.0.1"},
    {"127.0.0.1", "127.0.0.1"},
    {"127.1.2.3", "127.1.2.3"},
    {"127.255.255.254", "127.255.255.254"},
    {"0.0.0.0", "0.0.0.0"},
    {"0.0.0.1", "<ip>"},
    {"8.8.8.8", "<ip>"},
    {"1.2.3.4", "<ip>"},
    {"255.255.255.255", "<ip>"},
    {"256.1.2.3", "256.1.2.3"},
    {"192.168.1", "192.168.1"},
    {"192.168.1.1.5", "192.168.1.1.5"},
    {"192.168.1.1.", "192.168.1.1."},
    {".192.168.1.1", ".192.168.1.1"},
    {"x192.168.1.1y", "x192.168.x.xy"},
  };
  for (const auto &[in, expect] : cases) {
    EXPECT_EQ(redacted(in), std::string(expect)) << in;
  }
}

TEST(LogdropRedact, VersionsAndTimestampsStay) {
  const std::string line = "[2026-11-25 12:34:56.789]: Info: version 2.0.277 os 10.0.26300.1 uptime 12:34:56";
  EXPECT_EQ(redacted(line), line);
  EXPECT_EQ(redacted("2.0.277 10.1.2.3"), "2.0.277 10.1.x.x");
  EXPECT_EQ(redacted("10.0.26300.1 and 8.8.8.8"), "10.0.26300.1 and <ip>");
}

TEST(LogdropRedact, Ipv6) {
  EXPECT_EQ(redacted("::1"), "::1");
  EXPECT_EQ(redacted("1:2::3"), "<ipv6>");
  EXPECT_EQ(redacted("see ::1."), "see ::1.");
  EXPECT_EQ(redacted("::1%eth0"), "::1%eth0");
  EXPECT_EQ(redacted("addr=::1"), "addr=::1");
  EXPECT_EQ(redacted("fe80::1"), "<ipv6>");
  EXPECT_EQ(redacted("FE80::ABCD%eth0"), "<ipv6>");
  EXPECT_EQ(redacted("fe80::1."), "<ipv6>.");
  EXPECT_EQ(redacted("2001:db8::1"), "<ipv6>");
  EXPECT_EQ(redacted("2001:db8:85a3:0000:0000:8a2e:0370:7334"), "<ipv6>");
  EXPECT_EQ(redacted("::ffff:192.168.1.50"), "<ipv6>");
  EXPECT_EQ(redacted("::ffff:8.8.8.8"), "<ipv6>");
  EXPECT_EQ(redacted("::"), "<ipv6>");
  EXPECT_EQ(redacted("fade::beef"), "<ipv6>");
  // 八組全數字、沒有 ::：協定不當成 IPv6。
  EXPECT_EQ(redacted("0:0:0:0:0:0:0:1"), "0:0:0:0:0:0:0:1");
  // 前後接著字母就還在識別字裡，不能從中間切開。整段都是十六進位的 fade::beef 上面已遮。
  EXPECT_EQ(redacted("std::string"), "std::string");
  EXPECT_EQ(redacted("std::vector"), "std::vector");
  EXPECT_EQ(redacted("std::move"), "std::move");
  EXPECT_EQ(redacted("::string"), "::string");
  EXPECT_EQ(redacted("12:34:56"), "12:34:56");
}

TEST(LogdropRedact, Mac) {
  EXPECT_EQ(redacted("AA:BB:CC:DD:EE:FF"), "AA:BB:CC:xx:xx:xx");
  EXPECT_EQ(redacted("aa-bb-cc-dd-ee-ff"), "aa-bb-cc-xx-xx-xx");
  EXPECT_EQ(redacted("Ab:Cd:Ef:01:02:03"), "Ab:Cd:Ef:xx:xx:xx");
  EXPECT_EQ(redacted("12:34:56:78:90:12"), "12:34:56:xx:xx:xx");
  EXPECT_EQ(redacted("12:34:56"), "12:34:56");
  EXPECT_EQ(redacted("AA:BB:CC:DD:EE:FF:11"), "AA:BB:CC:DD:EE:FF:11");
  EXPECT_EQ(redacted("AA:BB-CC:DD:EE:FF"), "AA:BB-CC:DD:EE:FF");
  EXPECT_EQ(redacted("mac AA:BB:CC:DD:EE:FF."), "mac AA:BB:CC:xx:xx:xx.");
}

TEST(LogdropRedact, Email) {
  EXPECT_EQ(redacted("user@example.com"), "<email>");
  EXPECT_EQ(redacted("User.Name+tag@mail.example.co.uk"), "<email>");
  EXPECT_EQ(redacted("user@example.com."), "<email>.");
  EXPECT_EQ(redacted("(user@example.com)"), "(<email>)");
  EXPECT_EQ(redacted("a@b.c"), "a@b.c");
  EXPECT_EQ(redacted("8.8.8.8@example.com"), "<email>");
  EXPECT_EQ(redacted("8.8.8.8"), "<ip>");
}

TEST(LogdropRedact, ExtraSecrets) {
  EXPECT_EQ(redacted("Hello Alice", {"alice"}), "Hello <redacted>");
  EXPECT_EQ(redacted("aLiCe", {"Alice"}), "<redacted>");
  EXPECT_EQ(redacted("Al", {"Al"}), "Al");
  EXPECT_EQ(redacted("Bob and BOB", {"bob"}), "<redacted> and <redacted>");
  EXPECT_EQ(redacted("a.b axb", {"a.b"}), "<redacted> axb");
  EXPECT_EQ(redacted("MyPC-1", {"MyPC", "MyPC-1"}), "<redacted>");
  // 佔位字裡還有 act／red，不能再掃進去，否則會一直變長。
  EXPECT_EQ(redacted("red", {"red", "act", "dac"}), "<redacted>");
  // 位址先遮，名稱才遮。168 若先拿掉，剩下的片段就不再是 IP。
  EXPECT_EQ(redacted("192.168.1.20", {"168"}), "192.<redacted>.x.x");
  EXPECT_EQ(redacted("Hello Alice at 8.8.8.8", {"alice"}), "Hello <redacted> at <ip>");
}

TEST(LogdropRedact, EmptyAndLongLine) {
  EXPECT_EQ(redacted(""), "");
  std::string ones(100000, '1');
  const auto out = redacted(ones, {"111"});
  EXPECT_EQ(out.find("111"), std::string::npos);
  EXPECT_NE(out.find("<redacted>"), std::string::npos);

  std::string messy(20000, '1');
  messy.push_back('@');
  for (int i = 0; i < 5000; ++i) {
    messy += ".a";
  }
  EXPECT_EQ(redacted(messy), messy);
}
