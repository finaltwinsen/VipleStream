// §LOGDROP：跟 ImuFusionTest 一樣，gradle 還沒掛 junit，這個檔是給之後接上測試時用的。
package com.limelight.utils;

import org.junit.Assert;
import org.junit.Test;

import java.util.Arrays;
import java.util.Collections;

public class LogRedactorTest {
    @Test
    public void nullTextStaysNull() {
        Assert.assertNull(LogRedactor.redact(null, null));
        Assert.assertEquals("", LogRedactor.redact("", Collections.<String>emptyList()));
    }

    @Test
    public void privateAndCgnatKeepTheFirstTwoOctets() {
        Assert.assertEquals("10.1.x.x", LogRedactor.redact("10.1.2.3", null));
        Assert.assertEquals("10.0.x.x", LogRedactor.redact("10.0.0.0", null));
        Assert.assertEquals("172.16.x.x", LogRedactor.redact("172.16.5.6", null));
        Assert.assertEquals("172.31.x.x", LogRedactor.redact("172.31.255.255", null));
        Assert.assertEquals("192.168.x.x", LogRedactor.redact("192.168.10.20", null));
        Assert.assertEquals("100.64.x.x", LogRedactor.redact("100.64.0.1", null));
        Assert.assertEquals("100.127.x.x", LogRedactor.redact("100.127.8.9", null));
        Assert.assertEquals("169.254.x.x", LogRedactor.redact("169.254.1.1", null));
        Assert.assertEquals("192.168.x.x:47984", LogRedactor.redact("192.168.1.5:47984", null));
    }

    @Test
    public void publicAddressesAndSpecialCases() {
        Assert.assertEquals("<ip>", LogRedactor.redact("8.8.8.8", null));
        Assert.assertEquals("<ip>", LogRedactor.redact("1.2.3.4", null));
        Assert.assertEquals("<ip>", LogRedactor.redact("172.15.1.2", null));
        Assert.assertEquals("<ip>", LogRedactor.redact("172.32.1.2", null));
        Assert.assertEquals("<ip>", LogRedactor.redact("100.63.1.1", null));
        Assert.assertEquals("<ip>", LogRedactor.redact("100.128.1.1", null));
        Assert.assertEquals("<ip>", LogRedactor.redact("169.255.1.1", null));
        Assert.assertEquals("<ip>", LogRedactor.redact("0.0.0.1", null));
        Assert.assertEquals("127.0.0.1", LogRedactor.redact("127.0.0.1", null));
        Assert.assertEquals("127.1.2.3", LogRedactor.redact("127.1.2.3", null));
        Assert.assertEquals("0.0.0.0", LogRedactor.redact("0.0.0.0", null));
        Assert.assertEquals("256.1.1.1", LogRedactor.redact("256.1.1.1", null));
    }

    @Test
    public void versionsAndLongerNumbersAreNotIps() {
        Assert.assertEquals("2.0.277", LogRedactor.redact("2.0.277", null));
        Assert.assertEquals("10.0.26300.1", LogRedactor.redact("10.0.26300.1", null));
        Assert.assertEquals("192.168.1.2.3", LogRedactor.redact("192.168.1.2.3", null));
        Assert.assertEquals(
                "version 2.0.277 at <ip> and 10.0.26300.1",
                LogRedactor.redact("version 2.0.277 at 8.8.8.8 and 10.0.26300.1", null));
    }

    @Test
    public void logcatTimestampIsNotAnAddress() {
        String line = "11-25 14:03:12.123  1234  5678 I VipleViture: wear 1 -> 0";
        Assert.assertEquals(line, LogRedactor.redact(line, null));
        Assert.assertEquals("12:34:56", LogRedactor.redact("12:34:56", null));
        Assert.assertEquals("9:05:07", LogRedactor.redact("9:05:07", null));
        Assert.assertEquals(
                "11-25 14:03:12.123  845  912 D VipleViture: open 192.168.x.x",
                LogRedactor.redact("11-25 14:03:12.123  845  912 D VipleViture: open 192.168.0.12", null));
    }

    @Test
    public void ipv6IsMaskedButLoopbackAndCppScopesStay() {
        Assert.assertEquals("<ipv6>", LogRedactor.redact("2001:db8::1", null));
        Assert.assertEquals("<ipv6>", LogRedactor.redact("2001:0db8:85a3:0000:0000:8a2e:0370:7334", null));
        Assert.assertEquals("<ipv6>", LogRedactor.redact("fe80::1%wlan0", null));
        Assert.assertEquals("<ipv6>", LogRedactor.redact("FE80::ABCD%rmnet0", null));
        Assert.assertEquals("<ipv6>", LogRedactor.redact("::ffff:192.0.2.1", null));
        Assert.assertEquals("<ipv6>", LogRedactor.redact("1::2", null));
        Assert.assertEquals("<ipv6>", LogRedactor.redact("::", null));
        Assert.assertEquals("::1", LogRedactor.redact("::1", null));
        Assert.assertEquals("::1%lo", LogRedactor.redact("::1%lo", null));
        Assert.assertEquals("ping ::1 now", LogRedactor.redact("ping ::1 now", null));
        Assert.assertEquals("see <ipv6> now", LogRedactor.redact("see 2001:db8::1 now", null));
        Assert.assertEquals("std::string", LogRedactor.redact("std::string", null));
        Assert.assertEquals("Foo::Bar::baz", LogRedactor.redact("Foo::Bar::baz", null));
        Assert.assertEquals("1:2:3:4:5:6:7:8", LogRedactor.redact("1:2:3:4:5:6:7:8", null));
    }

    @Test
    public void macKeepsTheFirstThreeGroups() {
        Assert.assertEquals("aa:bb:cc:xx:xx:xx", LogRedactor.redact("aa:bb:cc:dd:ee:ff", null));
        Assert.assertEquals("AA-BB-CC-xx-xx-xx", LogRedactor.redact("AA-BB-CC-DD-EE-FF", null));
        Assert.assertEquals("11:22:33:xx:xx:xx", LogRedactor.redact("11:22:33:44:55:66", null));
        Assert.assertEquals("aa:bb:cc-dd:ee:ff", LogRedactor.redact("aa:bb:cc-dd:ee:ff", null));
        Assert.assertEquals("<ipv6>", LogRedactor.redact("aa:bb:cc:dd:ee:ff:11:22", null));
    }

    @Test
    public void emailIsReplaced() {
        Assert.assertEquals("<email>", LogRedactor.redact("User.Name+tag@example.com", null));
        Assert.assertEquals("mail <email> today", LogRedactor.redact("mail a.b@example.co.uk today", null));
        Assert.assertEquals("not@localhost", LogRedactor.redact("not@localhost", null));
    }

    @Test
    public void extraSecretsAreCaseInsensitiveAndLiteral() {
        Assert.assertEquals(
                "connected to <redacted> now",
                LogRedactor.redact("connected to myhost now", Collections.singletonList("MyHost")));
        Assert.assertEquals(
                "x<redacted>y",
                LogRedactor.redact("xMyPCy", Collections.singletonList("mypc")));
        Assert.assertEquals(
                "ab stays",
                LogRedactor.redact("ab stays", Arrays.asList("ab", "", null)));
        Assert.assertEquals(
                "<redacted> axb",
                LogRedactor.redact("a.b axb", Collections.singletonList("a.b")));
        Assert.assertEquals(
                "<redacted>",
                LogRedactor.redact("OfficePC", Arrays.asList("Office", "OfficePC")));
        Assert.assertEquals(
                "<redacted>",
                LogRedactor.redact("8.8.8.8", Collections.singletonList("8.8.8.8")));
    }

    @Test
    public void mixedLineAppliesEveryRule() {
        String input = "mail a@b.co mac aa:bb:cc:dd:ee:ff ip 8.8.8.8 lan 10.1.2.3 v6 2001:db8::1 host Office";
        String expected = "mail <email> mac aa:bb:cc:xx:xx:xx ip <ip> lan 10.1.x.x v6 <ipv6> host <redacted>";
        Assert.assertEquals(expected, LogRedactor.redact(input, Collections.singletonList("Office")));
    }
}
