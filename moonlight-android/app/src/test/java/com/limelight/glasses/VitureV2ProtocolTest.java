// §VITURE：專案目前沒有 JUnit 相依，要加 testImplementation 'junit:junit:4.13.2' 這個檔才會被編譯。
// 位元組向量是手算的，測試本身不重算 checksum，避免跟被測程式用同一套公式。
package com.limelight.glasses;

import org.junit.Assert;
import org.junit.Test;

public class VitureV2ProtocolTest {
    private interface Check {
        void run();
    }

    @Test
    public void emptyGetPacketsMatchHandVectors() {
        Assert.assertEquals("1000023000000000", hexOf(VitureV2Protocol.packet(VitureV2Protocol.MSG_GET_SERIAL)));
        Assert.assertEquals("1000223100000000", hexOf(VitureV2Protocol.packet(VitureV2Protocol.MSG_GET_BRIGHTNESS)));
        Assert.assertEquals(8, VitureV2Protocol.packet(VitureV2Protocol.MSG_GET_SERIAL).length);
    }

    @Test
    public void checksumCarriesIntoHighByte() {
        byte[] packet = VitureV2Protocol.packet(VitureV2Protocol.MSG_GET_BRIGHTNESS, new byte[] {(byte) 0xFF, 0x01});
        Assert.assertEquals("1000223102000001FF01", hexOf(packet));
        Assert.assertEquals(10, packet.length);
    }

    @Test
    public void imuControlUsesReferenceWireRates() {
        Assert.assertEquals("10000103020003000102", hexOf(VitureV2Protocol.imuControl(
                VitureV2Protocol.STREAM_POSE, VitureV2Protocol.Rate.HZ_120)));
        Assert.assertEquals("10000103020003000102", hexOf(VitureV2Protocol.imuControl(VitureV2Protocol.STREAM_POSE)));
        Assert.assertEquals("10000103020000000000", hexOf(VitureV2Protocol.imuControl(
                VitureV2Protocol.STREAM_OFF, VitureV2Protocol.Rate.HZ_240)));
        Assert.assertEquals("10000103020005000203", hexOf(VitureV2Protocol.imuControl(
                VitureV2Protocol.STREAM_RAW, VitureV2Protocol.Rate.HZ_240)));
        Assert.assertEquals("10000103020004000103", hexOf(VitureV2Protocol.imuControl(
                VitureV2Protocol.STREAM_POSE, VitureV2Protocol.Rate.HZ_240)));
        Assert.assertEquals("10000103020007000205", hexOf(VitureV2Protocol.imuControl(
                VitureV2Protocol.STREAM_RAW, VitureV2Protocol.Rate.HZ_1000)));
        Assert.assertEquals(0, VitureV2Protocol.Rate.HZ_60.wireValue);
        Assert.assertEquals(1, VitureV2Protocol.Rate.HZ_90.wireValue);
        Assert.assertEquals(2, VitureV2Protocol.Rate.HZ_120.wireValue);
        Assert.assertEquals(3, VitureV2Protocol.Rate.HZ_240.wireValue);
        Assert.assertEquals(4, VitureV2Protocol.Rate.HZ_500.wireValue);
        Assert.assertEquals(5, VitureV2Protocol.Rate.HZ_1000.wireValue);
    }

    @Test
    public void buildersRejectOutOfRangeInputs() {
        expectIllegal(new Check() {
            @Override
            public void run() {
                VitureV2Protocol.imuControl(VitureV2Protocol.STREAM_POSE, VitureV2Protocol.Rate.HZ_500);
            }
        });
        expectIllegal(new Check() {
            @Override
            public void run() {
                VitureV2Protocol.imuControl(3);
            }
        });
        expectIllegal(new Check() {
            @Override
            public void run() {
                VitureV2Protocol.imuControl(-1);
            }
        });
        expectIllegal(new Check() {
            @Override
            public void run() {
                VitureV2Protocol.packet(-1);
            }
        });
        expectIllegal(new Check() {
            @Override
            public void run() {
                VitureV2Protocol.packet(0x10000);
            }
        });
        expectIllegal(new Check() {
            @Override
            public void run() {
                VitureV2Protocol.packet(VitureV2Protocol.MSG_GET_SERIAL, null);
            }
        });
        final byte[] tooLong = new byte[57];
        expectIllegal(new Check() {
            @Override
            public void run() {
                VitureV2Protocol.packet(VitureV2Protocol.MSG_GET_BRIGHTNESS, tooLong);
            }
        });
    }

    @Test
    public void maxPayloadIsSixtyFourBytesAndNotPaddedShorter() {
        byte[] payload = new byte[56];
        for (int i = 0; i < payload.length; i++) {
            payload[i] = (byte) 0xFF;
        }
        byte[] packet = VitureV2Protocol.packet(VitureV2Protocol.MSG_GET_BRIGHTNESS, payload);
        Assert.assertEquals(64, packet.length);
        Assert.assertEquals("100022313800C837", hexOf(packet).substring(0, 16));
        for (int i = 8; i < packet.length; i++) {
            Assert.assertEquals((byte) 0xFF, packet[i]);
        }
    }

    @Test
    public void parseRejectsBrokenFrames() {
        Assert.assertNull(VitureV2Protocol.parse(new byte[8], 7));
        Assert.assertNull(VitureV2Protocol.parse(new byte[80], 65));
        Assert.assertNull(VitureV2Protocol.parse(new byte[8], 9));
        Assert.assertNull(VitureV2Protocol.parse(null));

        byte[] payloadTooBig = hex("1000223139000000");
        byte[] padded = new byte[64];
        System.arraycopy(payloadTooBig, 0, padded, 0, payloadTooBig.length);
        Assert.assertNull(VitureV2Protocol.parse(padded));

        byte[] shortBody = hex("1000223104000000FFFF");
        Assert.assertNull(VitureV2Protocol.parse(shortBody, 10));

        byte[] badPreamble = hex("1100023000000000");
        Assert.assertNull(VitureV2Protocol.parse(badPreamble));

        byte[] flipped = hex("1000223102000100FF01");
        Assert.assertNull(VitureV2Protocol.parse(flipped));
    }

    @Test
    public void parseAcceptsPaddingPastPacketSize() {
        byte[] packet = VitureV2Protocol.packet(VitureV2Protocol.MSG_GET_DISPLAY);
        byte[] padded = new byte[128];
        System.arraycopy(packet, 0, padded, 0, packet.length);
        VitureV2Protocol.Frame frame = VitureV2Protocol.parse(padded, padded.length);
        Assert.assertNotNull(frame);
        Assert.assertEquals(VitureV2Protocol.MSG_GET_DISPLAY, frame.msgId);
        Assert.assertEquals(0, frame.payload.length);
        Assert.assertEquals(VitureV2Protocol.MSG_GET_DISPLAY, VitureV2Protocol.classify(padded, padded.length));
    }

    @Test
    public void serialMaskHidesShortValues() {
        Assert.assertEquals("******", VitureProbe.maskSerial(new byte[] {'A', 'B', 'C', 'D', 'E', 'F'}, false));
        Assert.assertEquals("A*****G", VitureProbe.maskSerial(new byte[] {'A', 'B', 'C', 'D', 'E', 'F', 'G'}, false));
        Assert.assertEquals("A******H", VitureProbe.maskSerial(
                new byte[] {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H'}, false));
        Assert.assertEquals("ABCD****IJ", VitureProbe.maskSerial(
                new byte[] {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J'}, false));
        Assert.assertEquals("******", VitureProbe.maskSerial(new byte[] {0x01, 0x02, 0x03}, true));
        Assert.assertEquals("01**********07", VitureProbe.maskSerial(
                new byte[] {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07}, true));
        Assert.assertNull(VitureProbe.maskSerial(new byte[] {0x01, 0x02}, false));
    }

    @Test
    public void parseIgnoresBytesPastTheDeclaredPayload() {
        byte[] dirty = hex("1000023000000000FFFFFFFF");
        VitureV2Protocol.Frame sized = VitureV2Protocol.parse(dirty, 8);
        Assert.assertNotNull(sized);
        Assert.assertEquals(VitureV2Protocol.MSG_GET_SERIAL, sized.msgId);
        Assert.assertEquals(0, sized.payload.length);

        VitureV2Protocol.Frame whole = VitureV2Protocol.parse(dirty);
        Assert.assertNotNull(whole);
        Assert.assertEquals(0, whole.payload.length);
    }

    @Test
    public void replyUsesResponseOffsetAndStatusByte() {
        Assert.assertEquals(0x2000, VitureV2Protocol.RESPONSE_OFFSET);
        VitureV2Protocol.Frame serial = mustParse("1000025003008300004142");
        VitureV2Protocol.Reply reply = VitureV2Protocol.reply(serial, VitureV2Protocol.MSG_GET_SERIAL);
        Assert.assertNotNull(reply);
        Assert.assertEquals(0, reply.status);
        Assert.assertTrue(reply.success());
        Assert.assertEquals("4142", hexOf(reply.body));

        VitureV2Protocol.Frame failed = mustParse("10000250020042000141");
        VitureV2Protocol.Reply failedReply = VitureV2Protocol.reply(failed, VitureV2Protocol.MSG_GET_SERIAL);
        Assert.assertNotNull(failedReply);
        Assert.assertEquals(1, failedReply.status);
        Assert.assertFalse(failedReply.success());

        VitureV2Protocol.Frame wrapped = mustParse("100023110100000000");
        Assert.assertNotNull(VitureV2Protocol.reply(wrapped, 0xF123));
        Assert.assertEquals(0, VitureV2Protocol.reply(wrapped, 0xF123).status);

        Assert.assertNull(VitureV2Protocol.reply(mustParse("1000025000000000"), VitureV2Protocol.MSG_GET_SERIAL));
        VitureV2Protocol.Reply emptyImu = VitureV2Protocol.reply(
                mustParse("1000012300000000"), VitureV2Protocol.MSG_IMU_CONTROL);
        Assert.assertNotNull(emptyImu);
        Assert.assertEquals(0, emptyImu.status);
        Assert.assertEquals(0, emptyImu.body.length);
        Assert.assertNull(VitureV2Protocol.reply(mustParse("1000012300000000"), VitureV2Protocol.MSG_GET_SERIAL));
        Assert.assertNull(VitureV2Protocol.reply(serial, -1));
        Assert.assertNull(VitureV2Protocol.reply(serial, 0x10000));
        Assert.assertNull(VitureV2Protocol.reply(serial, 0x5002));
        Assert.assertNull(VitureV2Protocol.reply(null, VitureV2Protocol.MSG_GET_SERIAL));
    }

    @Test
    public void poseParsesHandVectorsAndRejectsBadQuaternions() {
        VitureV2Protocol.Pose identity = poseOf("100008731800730144332211010203040000803F000000000000000000000000");
        Assert.assertEquals(0x11223344L, identity.unknown);
        Assert.assertEquals(0x04030201L, identity.deviceTick);
        assertQuat(identity, 0f, 0f, 0f, 1f);

        assertQuat(poseOf("100008731800C4000000000005000000000000000000803F0000000000000000"), 1f, 0f, 0f, 0f);
        assertQuat(poseOf("100008731800C500000000000600000000000000000000000000803F00000000"), 0f, 1f, 0f, 0f);
        assertQuat(poseOf("100008731800C60000000000070000000000000000000000000000000000803F"), 0f, 0f, 1f, 0f);
        assertQuat(poseOf("100008731800C70000000000080000000000803F000000000000000000000000"), 0f, 0f, 0f, 1f);

        VitureV2Protocol.Pose normalized = poseOf("100008731800BF0000000000010000000000403F0000003F0000000000000000");
        float scale = (float) Math.sqrt(0.75d * 0.75d + 0.5d * 0.5d);
        Assert.assertEquals(0.5f / scale, normalized.quaternion[0], 1e-6f);
        Assert.assertEquals(0f, normalized.quaternion[1], 0f);
        Assert.assertEquals(0f, normalized.quaternion[2], 0f);
        Assert.assertEquals(0.75f / scale, normalized.quaternion[3], 1e-6f);
        Assert.assertEquals(1L, normalized.deviceTick);

        VitureV2Protocol.Pose halves = poseOf("100008731800050100000000090000000000003F0000003F0000003F0000003F");
        assertQuat(halves, 0.5f, 0.5f, 0.5f, 0.5f);
        Assert.assertEquals(9L, halves.deviceTick);

        Assert.assertNull(VitureV2Protocol.pose(mustParse("1000087318003F0000000000000000000000003F000000000000000000000000")));
        Assert.assertNull(VitureV2Protocol.pose(mustParse("1000087318004000000000000000000000000040000000000000000000000000")));
        Assert.assertNull(VitureV2Protocol.pose(mustParse("1000087318003F0100000000000000000000C07F000000000000000000000000")));
        Assert.assertNull(VitureV2Protocol.pose(new VitureV2Protocol.Frame(VitureV2Protocol.EVENT_POSE, new byte[23])));
        Assert.assertNull(VitureV2Protocol.pose(new VitureV2Protocol.Frame(VitureV2Protocol.EVENT_RAW, new byte[24])));
    }

    @Test
    public void rawParsesHandVectorAndRejectsNonFinite() {
        VitureV2Protocol.RawSample sample = rawOf(
                "1000097338003F07"
                        + "AABBCCDD000100001122"
                        + "0000803F00000040000080BF"
                        + "0000003F000000C000008040"
                        + "00000000000000000000000000000000000000000000");
        Assert.assertEquals(256L, sample.deviceTick);
        Assert.assertEquals(1f, sample.gyroRadPerSec[0], 0f);
        Assert.assertEquals(2f, sample.gyroRadPerSec[1], 0f);
        Assert.assertEquals(-1f, sample.gyroRadPerSec[2], 0f);
        Assert.assertEquals(0.5f, sample.accelG[0], 0f);
        Assert.assertEquals(-2f, sample.accelG[1], 0f);
        Assert.assertEquals(4f, sample.accelG[2], 0f);
        Assert.assertEquals(56, sample.payload.length);

        Assert.assertNull(VitureV2Protocol.raw(mustParse(
                "100009733800BF07"
                        + "AABBCCDD000100001122"
                        + "0000C07F00000040000080BF"
                        + "0000003F000000C000008040"
                        + "00000000000000000000000000000000000000000000")));
        Assert.assertNull(VitureV2Protocol.raw(new VitureV2Protocol.Frame(VitureV2Protocol.EVENT_RAW, new byte[55])));
        Assert.assertNull(VitureV2Protocol.raw(new VitureV2Protocol.Frame(VitureV2Protocol.EVENT_POSE, new byte[56])));
    }

    private static void assertQuat(VitureV2Protocol.Pose pose, float x, float y, float z, float w) {
        Assert.assertEquals(x, pose.quaternion[0], 0f);
        Assert.assertEquals(y, pose.quaternion[1], 0f);
        Assert.assertEquals(z, pose.quaternion[2], 0f);
        Assert.assertEquals(w, pose.quaternion[3], 0f);
    }

    private static VitureV2Protocol.Pose poseOf(String text) {
        VitureV2Protocol.Pose pose = VitureV2Protocol.pose(mustParse(text));
        Assert.assertNotNull(pose);
        return pose;
    }

    private static VitureV2Protocol.RawSample rawOf(String text) {
        VitureV2Protocol.RawSample sample = VitureV2Protocol.raw(mustParse(text));
        Assert.assertNotNull(sample);
        return sample;
    }

    private static VitureV2Protocol.Frame mustParse(String text) {
        VitureV2Protocol.Frame frame = VitureV2Protocol.parse(bytes(text));
        Assert.assertNotNull(text, frame);
        return frame;
    }

    private static String hexOf(byte[] data) {
        return VitureV2Protocol.toHex(data);
    }

    private static byte[] bytes(String text) {
        String compact = text.replace(" ", "");
        byte[] out = new byte[compact.length() / 2];
        for (int i = 0; i < out.length; i++) {
            out[i] = (byte) Integer.parseInt(compact.substring(i * 2, i * 2 + 2), 16);
        }
        return out;
    }

    private static byte[] hex(String text) {
        return bytes(text);
    }

    private static void expectIllegal(Check check) {
        try {
            check.run();
            Assert.fail("expected IllegalArgumentException");
        } catch (IllegalArgumentException expected) {
            Assert.assertNotNull(expected);
        }
    }
}
