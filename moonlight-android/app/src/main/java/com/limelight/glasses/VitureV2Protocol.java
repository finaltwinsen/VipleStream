// Adapted from DroidDeck (GPL-3.0) / elasticjava/viture-v2 (MIT, Copyright (c) 2026 Holger Bartnick).
package com.limelight.glasses;

/**
 * §VITURE：純 Java，JVM 測試才不必帶 Android。數值與位元組順序對齊參考實作，不能在這裡「修正」。
 */
public final class VitureV2Protocol {
    public static final int PACKET_SIZE = 64;
    public static final int HEADER_SIZE = 8;
    public static final int RESPONSE_OFFSET = 0x2000;
    private static final int PREAMBLE = 0x0010;
    private static final int MAX_PAYLOAD_SIZE = PACKET_SIZE - HEADER_SIZE;

    public static final int MSG_IMU_CONTROL = 0x0301;
    public static final int MSG_SET_DUTY_CYCLE = 0x0125;
    public static final int MSG_SET_DISPLAY_MODE = 0x0141;
    public static final int MSG_SET_VOLUME = 0x0201;
    public static final int MSG_GET_SERIAL = 0x3002;
    public static final int MSG_GET_VERSION = 0x3003;
    public static final int MSG_GET_BRIGHTNESS = 0x3122;
    public static final int MSG_GET_DUTY_CYCLE = 0x3125;
    public static final int MSG_GET_DISPLAY = 0x3141;
    public static final int MSG_GET_VOLUME = 0x3201;
    public static final int MSG_GET_WEAR = 0x3321;
    public static final int EVENT_POSE = 0x7308;
    public static final int EVENT_RAW = 0x7309;

    /** 1920×1080 @ 60 Hz，一般 2D。Beast 的 GET 也會回這個值，但 SET 會黑屏，不能送。 */
    public static final int DISPLAY_2D_60 = 0x31;
    /** 3840×1080 @ 60 Hz，左眼在左半的 3D SBS。Beast 送這個值會黑屏。 */
    public static final int DISPLAY_3D_SBS_60 = 0x32;
    /** 1920×1080 @ 120 Hz。Beast 不能送。 */
    public static final int DISPLAY_2D_120 = 0x34;

    public static final int INSPECT_MALFORMED = -1;
    public static final int INSPECT_CHECKSUM = -2;

    public static final int STREAM_OFF = 0;
    public static final int STREAM_POSE = 1;
    public static final int STREAM_RAW = 2;

    private static final char[] HEX = "0123456789ABCDEF".toCharArray();

    public enum Rate {
        HZ_60(0, 60),
        HZ_90(1, 90),
        HZ_120(2, 120),
        HZ_240(3, 240),
        HZ_500(4, 500),
        HZ_1000(5, 1000);

        public final int wireValue;
        public final int hz;

        Rate(int wireValue, int hz) {
            this.wireValue = wireValue;
            this.hz = hz;
        }
    }

    public static final class Frame {
        public final int msgId;
        public final byte[] payload;

        public Frame(int msgId, byte[] payload) {
            this.msgId = msgId;
            this.payload = payload == null ? new byte[0] : payload;
        }
    }

    public static final class Reply {
        public final int status;
        public final byte[] body;

        public Reply(int status, byte[] body) {
            this.status = status;
            this.body = body == null ? new byte[0] : body;
        }

        public boolean success() {
            return status == 0;
        }
    }

    public static final class Pose {
        public final long deviceTick;
        /** §VITURE：線上順序是 w、x、y、z；正規化後排成 x、y、z、w，跟參考實作一致。 */
        public final float[] quaternion;
        public final long unknown;

        public Pose(long deviceTick, float[] quaternion, long unknown) {
            this.deviceTick = deviceTick;
            this.quaternion = quaternion;
            this.unknown = unknown;
        }
    }

    public static final class RawSample {
        public final long deviceTick;
        public final float[] gyroRadPerSec;
        public final float[] accelG;
        public final byte[] payload;

        public RawSample(long deviceTick, float[] gyroRadPerSec, float[] accelG, byte[] payload) {
            this.deviceTick = deviceTick;
            this.gyroRadPerSec = gyroRadPerSec;
            this.accelG = accelG;
            this.payload = payload == null ? new byte[0] : payload;
        }
    }

    private VitureV2Protocol() {
    }

    public static byte[] packet(int msgId) {
        return packet(msgId, new byte[0]);
    }

    public static byte[] packet(int msgId, byte[] payload) {
        if (msgId < 0 || msgId > 0xFFFF) {
            throw new IllegalArgumentException("msgId");
        }
        if (payload == null) {
            throw new IllegalArgumentException("payload");
        }
        if (payload.length > MAX_PAYLOAD_SIZE) {
            throw new IllegalArgumentException("payload");
        }
        byte[] buffer = new byte[HEADER_SIZE + payload.length];
        putU16(buffer, 0, PREAMBLE);
        putU16(buffer, 2, msgId);
        putU16(buffer, 4, payload.length);
        putU16(buffer, 6, checksum(payload, 0, payload.length));
        System.arraycopy(payload, 0, buffer, HEADER_SIZE, payload.length);
        return buffer;
    }

    public static byte[] imuControl(int streams) {
        return imuControl(streams, Rate.HZ_120);
    }

    public static byte[] imuControl(int streams, Rate rate) {
        if (streams != STREAM_OFF && streams != STREAM_POSE && streams != STREAM_RAW) {
            throw new IllegalArgumentException("streams");
        }
        if (rate == null) {
            throw new IllegalArgumentException("rate");
        }
        // §VITURE：參考實作只允許姿態串流到 240 Hz，更快的速率只能用在原始串流。
        if (streams == STREAM_POSE && rate.hz > 240) {
            throw new IllegalArgumentException("pose rate");
        }
        int wireRate = streams == STREAM_OFF ? 0 : rate.wireValue;
        return packet(MSG_IMU_CONTROL, new byte[] {(byte) streams, (byte) wireRate});
    }

    public static byte[] setDisplayMode(int mode) {
        if (mode < 0 || mode > 0xFF) {
            throw new IllegalArgumentException("mode");
        }
        return packet(MSG_SET_DISPLAY_MODE, new byte[] {(byte) mode});
    }

    public static byte[] setDutyCycle(int percent) {
        if (percent < 0 || percent > 100) {
            throw new IllegalArgumentException("duty");
        }
        return packet(MSG_SET_DUTY_CYCLE, new byte[] {(byte) percent});
    }

    public static byte[] setVolume(int level) {
        if (level < 0 || level > 0xFF) {
            throw new IllegalArgumentException("volume");
        }
        // §VITURE：參考只說 payload 兩個位元組，沒寫欄位。查詢回的是一個 u8，低位元組放音量、高位元組留 0。
        return packet(MSG_SET_VOLUME, new byte[] {(byte) level, 0});
    }

    /**
     * @return MsgID，或 {@link #INSPECT_MALFORMED}、{@link #INSPECT_CHECKSUM}。
     *         跟 {@link #parse} 不同的是 checksum 錯誤要單獨計數。
     */
    public static int classify(byte[] buffer, int size) {
        // 端點的 max packet 可以大於 64。多出來的是填充，真正長度在 header，不能整包丟掉。
        if (buffer == null || size < HEADER_SIZE || size > buffer.length) {
            return INSPECT_MALFORMED;
        }
        if (u16(buffer, 0) != PREAMBLE) {
            return INSPECT_MALFORMED;
        }
        int payloadSize = u16(buffer, 4);
        if (payloadSize > MAX_PAYLOAD_SIZE || HEADER_SIZE + payloadSize > size) {
            return INSPECT_MALFORMED;
        }
        if (checksum(buffer, HEADER_SIZE, payloadSize) != u16(buffer, 6)) {
            return INSPECT_CHECKSUM;
        }
        return u16(buffer, 2);
    }

    /** 熱路徑用。不配置封包複本。序列是 payload offset 4 的 u32。 */
    public static boolean fillRaw(byte[] buffer, int size, float[] gyro, float[] accel, long[] sequence) {
        if (classify(buffer, size) != EVENT_RAW) {
            return false;
        }
        return fillRawBody(buffer, size, gyro, accel, sequence);
    }

    /**
     * 呼叫端已經 {@link #classify} 過，而且結果是 {@link #EVENT_RAW}。
     * 400 Hz 不能對同一包再跑一次 checksum。
     */
    static boolean fillRawBody(byte[] buffer, int size, float[] gyro, float[] accel, long[] sequence) {
        if (gyro == null || gyro.length < 3 || accel == null || accel.length < 3
                || sequence == null || sequence.length < 1) {
            return false;
        }
        if (buffer == null || size < HEADER_SIZE || u16(buffer, 4) != 56 || HEADER_SIZE + 56 > size) {
            return false;
        }
        int offset = HEADER_SIZE;
        float gx = float32(buffer, offset + 10);
        float gy = float32(buffer, offset + 14);
        float gz = float32(buffer, offset + 18);
        float ax = float32(buffer, offset + 22);
        float ay = float32(buffer, offset + 26);
        float az = float32(buffer, offset + 30);
        if (!Float.isFinite(gx) || !Float.isFinite(gy) || !Float.isFinite(gz)
                || !Float.isFinite(ax) || !Float.isFinite(ay) || !Float.isFinite(az)) {
            return false;
        }
        gyro[0] = gx;
        gyro[1] = gy;
        gyro[2] = gz;
        accel[0] = ax;
        accel[1] = ay;
        accel[2] = az;
        sequence[0] = u32(buffer, offset + 4);
        return true;
    }

    public static Frame parse(byte[] buffer, int size) {
        // 跟 classify 一樣：大於 64 的讀取後面是填充，payload 長度仍由 header 決定。
        if (buffer == null || size < HEADER_SIZE || size > buffer.length) {
            return null;
        }
        if (u16(buffer, 0) != PREAMBLE) {
            return null;
        }
        int payloadSize = u16(buffer, 4);
        if (payloadSize > MAX_PAYLOAD_SIZE || HEADER_SIZE + payloadSize > size) {
            return null;
        }
        if (checksum(buffer, HEADER_SIZE, payloadSize) != u16(buffer, 6)) {
            return null;
        }
        byte[] payload = new byte[payloadSize];
        System.arraycopy(buffer, HEADER_SIZE, payload, 0, payloadSize);
        return new Frame(u16(buffer, 2), payload);
    }

    public static Frame parse(byte[] buffer) {
        if (buffer == null) {
            return null;
        }
        return parse(buffer, buffer.length);
    }

    public static Reply reply(Frame frame, int requestMsgId) {
        if (frame == null || requestMsgId < 0 || requestMsgId > 0xFFFF) {
            return null;
        }
        int expected = (requestMsgId + RESPONSE_OFFSET) & 0xFFFF;
        if (frame.msgId != expected) {
            return null;
        }
        if (frame.payload.length == 0) {
            // 0x2301 有時沒有 payload。空的 ACK 仍是成功。其他查詢維持沒有回覆，避免把失敗當成空內容。
            if (requestMsgId == MSG_IMU_CONTROL) {
                return new Reply(0, new byte[0]);
            }
            return null;
        }
        byte[] body = new byte[frame.payload.length - 1];
        System.arraycopy(frame.payload, 1, body, 0, body.length);
        return new Reply(frame.payload[0] & 0xFF, body);
    }

    public static Pose pose(Frame frame) {
        if (frame == null || frame.msgId != EVENT_POSE || frame.payload.length != 24) {
            return null;
        }
        byte[] payload = frame.payload;
        float w = float32(payload, 8);
        float x = float32(payload, 12);
        float y = float32(payload, 16);
        float z = float32(payload, 20);
        if (!Float.isFinite(w) || !Float.isFinite(x) || !Float.isFinite(y) || !Float.isFinite(z)) {
            return null;
        }
        double norm = Math.sqrt((double) w * w + (double) x * x + (double) y * y + (double) z * z);
        if (norm < 0.9d || norm > 1.1d) {
            return null;
        }
        float n = (float) norm;
        return new Pose(u32(payload, 4), new float[] {x / n, y / n, z / n, w / n}, u32(payload, 0));
    }

    public static RawSample raw(Frame frame) {
        if (frame == null || frame.msgId != EVENT_RAW || frame.payload.length != 56) {
            return null;
        }
        byte[] payload = frame.payload;
        float[] gyro = new float[] {
                float32(payload, 10),
                float32(payload, 14),
                float32(payload, 18)
        };
        float[] accel = new float[] {
                float32(payload, 22),
                float32(payload, 26),
                float32(payload, 30)
        };
        for (int i = 0; i < 3; i++) {
            if (!Float.isFinite(gyro[i]) || !Float.isFinite(accel[i])) {
                return null;
            }
        }
        byte[] copy = new byte[payload.length];
        System.arraycopy(payload, 0, copy, 0, payload.length);
        return new RawSample(u32(payload, 4), gyro, accel, copy);
    }

    public static String toHex(byte[] data) {
        if (data == null || data.length == 0) {
            return "";
        }
        char[] out = new char[data.length * 2];
        for (int i = 0; i < data.length; i++) {
            int value = data[i] & 0xFF;
            out[i * 2] = HEX[value >>> 4];
            out[i * 2 + 1] = HEX[value & 0x0F];
        }
        return new String(out);
    }

    public static String hex16(int value) {
        return "0x" + nibble((value >>> 12) & 0xF) + nibble((value >>> 8) & 0xF)
                + nibble((value >>> 4) & 0xF) + nibble(value & 0xF);
    }

    private static char nibble(int value) {
        return HEX[value & 0xF];
    }

    private static int checksum(byte[] buffer, int offset, int size) {
        int sum = 0;
        for (int i = offset; i < offset + size; i++) {
            sum += buffer[i] & 0xFF;
        }
        return sum & 0xFFFF;
    }

    private static int u16(byte[] buffer, int offset) {
        return (buffer[offset] & 0xFF) | ((buffer[offset + 1] & 0xFF) << 8);
    }

    private static long u32(byte[] buffer, int offset) {
        return (u16(buffer, offset) & 0xFFFFL) | (((long) u16(buffer, offset + 2)) << 16);
    }

    private static float float32(byte[] buffer, int offset) {
        return Float.intBitsToFloat((int) u32(buffer, offset));
    }

    private static void putU16(byte[] buffer, int offset, int value) {
        buffer[offset] = (byte) value;
        buffer[offset + 1] = (byte) (value >>> 8);
    }
}
