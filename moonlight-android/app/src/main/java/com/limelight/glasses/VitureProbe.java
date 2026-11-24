// Adapted from DroidDeck (GPL-3.0) / elasticjava/viture-v2 (MIT, Copyright (c) 2026 Holger Bartnick).
package com.limelight.glasses;

import java.io.IOException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.Locale;
import java.util.Map;

/**
 * §VITURE：只送讀取指令與 IMU 串流開關。亮度、音量、顯示模式、DOF 都不寫。
 */
public final class VitureProbe {
    public static final int QUERY_TIMEOUT_MS = 1000;
    public static final int STREAM_WINDOW_MS = 3000;
    public static final int READ_SLICE_MS = 200;
    public static final int WRITE_TIMEOUT_MS = 250;

    private static final int[] GET_IDS = new int[] {
            VitureV2Protocol.MSG_GET_SERIAL,
            VitureV2Protocol.MSG_GET_VERSION,
            VitureV2Protocol.MSG_GET_BRIGHTNESS,
            VitureV2Protocol.MSG_GET_DUTY_CYCLE,
            VitureV2Protocol.MSG_GET_DISPLAY,
            VitureV2Protocol.MSG_GET_VOLUME,
            VitureV2Protocol.MSG_GET_WEAR,
    };

    public interface Link {
        int write(byte[] packet, int timeoutMs, boolean padToPacketSize) throws IOException;

        /** @return null when the transfer times out or the stack returns a negative count */
        byte[] read(int timeoutMs) throws IOException;
    }

    public interface Host {
        boolean isCancelled();

        void onStage(Stage stage);
    }

    public enum Stage {
        QUERIES,
        POSE,
        RAW
    }

    private final Link link;
    private final Host host;
    private final StringBuilder report;
    private final LinkedHashMap<String, Bucket> unknown = new LinkedHashMap<>();
    private final LinkedHashMap<String, Bucket> unparsed = new LinkedHashMap<>();
    private final LinkedHashMap<String, Integer> unsolicited = new LinkedHashMap<>();
    private boolean pad;
    private boolean imuActive;

    private VitureProbe(Link link, Host host, StringBuilder report) {
        this.link = link;
        this.host = host;
        this.report = report;
    }

    public static void run(Link link, Host host, StringBuilder report) {
        new VitureProbe(link, host, report).execute();
    }

    public static String errorText(Throwable error) {
        if (error == null) {
            return "unknown";
        }
        String message = error.getMessage();
        String name = error.getClass().getSimpleName();
        if (message == null || message.length() == 0) {
            return name;
        }
        return name + ": " + message.replace('\n', ' ').replace('\r', ' ');
    }

    private void execute() {
        report.append("protocol=v2 response_offset=")
                .append(VitureV2Protocol.hex16(VitureV2Protocol.RESPONSE_OFFSET))
                .append('\n');
        report.append("commands=read_only imu_toggle\n");
        try {
            queries();
            if (!host.isCancelled()) {
                stream(VitureV2Protocol.STREAM_POSE, VitureV2Protocol.Rate.HZ_120, VitureV2Protocol.EVENT_POSE, "pose");
            }
            if (!host.isCancelled()) {
                stream(VitureV2Protocol.STREAM_RAW, VitureV2Protocol.Rate.HZ_240, VitureV2Protocol.EVENT_RAW, "raw");
            }
        } catch (RuntimeException error) {
            report.append("probe_error=").append(errorText(error)).append('\n');
            VitureUsb.logW("probe failed", error);
        } finally {
            // stream() 自己的 finally 已經關過。這裡只補「開過但沒走到那個 finally」的情況，避免再白等一輪。
            if (imuActive) {
                sendOff("IMU_OFF_FINAL");
            }
        }
        appendTallies();
        if (host.isCancelled()) {
            report.append("cancelled\n");
        }
    }

    private void queries() {
        host.onStage(Stage.QUERIES);
        boolean padChosen = false;
        for (int i = 0; i < GET_IDS.length; i++) {
            if (host.isCancelled()) {
                return;
            }
            int id = GET_IDS[i];
            String name = getName(id);
            String label = "GET " + name + " id=" + VitureV2Protocol.hex16(id);
            Exchange first = exchange(id, label, pad, "get-" + name);
            if (!padChosen) {
                padChosen = true;
                // §VITURE：有些 HID 只收滿封包。第一個指令完全沒有可解析幀時才改填零重送，成功才沿用。
                if (!first.gotReply && !first.sawFrame) {
                    report.append("  retry=padded\n");
                    Exchange retry = exchange(id, label + " retry_padded", true, "get-" + name);
                    if (retry.gotReply || retry.sawFrame) {
                        pad = true;
                    }
                }
                report.append("transport_padding=").append(pad ? "on" : "off").append('\n');
            }
        }
    }

    private Exchange exchange(int requestId, String label, boolean usePad, String phase) {
        Exchange result = new Exchange();
        report.append(label).append('\n');
        byte[] tx;
        try {
            tx = VitureV2Protocol.packet(requestId);
        } catch (RuntimeException error) {
            report.append("  error=").append(errorText(error)).append('\n');
            return result;
        }
        writeAndWait(tx, requestId, usePad, result, phase);
        return result;
    }

    private void stream(int streams, VitureV2Protocol.Rate rate, int eventId, String name) {
        host.onStage(streams == VitureV2Protocol.STREAM_POSE ? Stage.POSE : Stage.RAW);
        byte[] tx;
        try {
            tx = VitureV2Protocol.imuControl(streams, rate);
        } catch (RuntimeException error) {
            report.append("IMU ").append(name).append(" error=").append(errorText(error)).append('\n');
            return;
        }
        report.append("IMU ").append(name)
                .append(" stream=").append(streams)
                .append(" rate_hz=").append(rate.hz)
                .append(" wire=").append(rate.wireValue)
                .append(" window_ms=").append(STREAM_WINDOW_MS)
                .append('\n');
        ArrayList<Sample> samples = new ArrayList<>();
        Integer controlStatus = null;
        long startNs = System.nanoTime();
        try {
            controlStatus = writeStream(tx, samples, eventId, name, startNs);
        } catch (Exception error) {
            report.append("  error=").append(errorText(error)).append('\n');
            VitureUsb.logW("imu " + name + " failed", error);
        } finally {
            appendStreamStats(samples, startNs, controlStatus);
            sendOff("IMU_OFF_" + name);
        }
    }

    private Integer writeStream(byte[] tx, ArrayList<Sample> samples, int eventId, String phase, long startNs) throws IOException {
        int written = link.write(tx, WRITE_TIMEOUT_MS, pad);
        imuActive = true;
        report.append("  tx=").append(VitureV2Protocol.toHex(tx))
                .append(" tx_len=").append(tx.length)
                .append(" padded=").append(pad)
                .append('\n');
        report.append("  write=").append(written).append('\n');
        Integer controlStatus = null;
        long deadline = startNs + STREAM_WINDOW_MS * 1_000_000L;
        while (System.nanoTime() < deadline) {
            if (host.isCancelled()) {
                break;
            }
            int slice = sliceMs(deadline);
            byte[] rx = link.read(slice);
            if (rx == null) {
                continue;
            }
            VitureV2Protocol.Frame frame = VitureV2Protocol.parse(rx);
            if (frame == null) {
                noteUnparsed(phase, rx);
                continue;
            }
            VitureV2Protocol.Reply control = VitureV2Protocol.reply(frame, VitureV2Protocol.MSG_IMU_CONTROL);
            if (control != null) {
                if (controlStatus == null) {
                    controlStatus = control.status;
                    VitureUsb.logI("imu control reply status=" + control.status
                            + " payload=" + frame.payload.length);
                }
                continue;
            }
            if (frame.msgId == eventId) {
                samples.add(new Sample(System.nanoTime(), rx, frame));
                continue;
            }
            noteOther(phase, rx, frame);
        }
        return controlStatus;
    }

    private void writeAndWait(byte[] tx, int requestId, boolean usePad, Exchange result, String phase) {
        try {
            int written = link.write(tx, WRITE_TIMEOUT_MS, usePad);
            report.append("  tx=").append(VitureV2Protocol.toHex(tx))
                    .append(" tx_len=").append(tx.length)
                    .append(" padded=").append(usePad)
                    .append('\n');
            report.append("  write=").append(written).append('\n');
            long deadline = System.nanoTime() + QUERY_TIMEOUT_MS * 1_000_000L;
            boolean loggedReply = false;
            while (System.nanoTime() < deadline) {
                if (host.isCancelled()) {
                    report.append("  cancelled\n");
                    return;
                }
                byte[] rx = link.read(sliceMs(deadline));
                if (rx == null) {
                    continue;
                }
                VitureV2Protocol.Frame frame = VitureV2Protocol.parse(rx);
                if (frame == null) {
                    noteUnparsed(phase, rx);
                    continue;
                }
                result.sawFrame = true;
                VitureV2Protocol.Reply reply = VitureV2Protocol.reply(frame, requestId);
                if (reply != null && !loggedReply) {
                    loggedReply = true;
                    result.gotReply = true;
                    appendReply(requestId, rx, reply);
                    return;
                }
                if (reply == null) {
                    noteOther(phase, rx, frame);
                }
            }
            if (!loggedReply) {
                report.append("  timeout\n");
            }
        } catch (Exception error) {
            report.append("  error=").append(errorText(error)).append('\n');
            VitureUsb.logW("command failed id=" + VitureV2Protocol.hex16(requestId), error);
        }
    }

    private void appendReply(int requestId, byte[] rx, VitureV2Protocol.Reply reply) {
        report.append("  status=").append(reply.status)
                .append(" success=").append(reply.success())
                .append(" body_len=").append(reply.body.length)
                .append('\n');
        if (requestId == VitureV2Protocol.MSG_GET_SERIAL) {
            // §VITURE：序號會被公開貼出，完整內容與可還原的十六進位都不能留下。
            String text = asciiOrNull(reply.body);
            if (text == null) {
                report.append("  serial=(not ascii)\n");
            } else {
                report.append("  serial=").append(maskSerial(reply.body, false)).append('\n');
            }
            report.append("  body_hex=").append(maskSerial(reply.body, true)).append('\n');
            return;
        }
        report.append("  body_hex=").append(VitureV2Protocol.toHex(reply.body)).append('\n');
        report.append("  rx=").append(hexForReport(rx)).append('\n');
        appendParsedValue(requestId, reply.body);
    }

    private void appendParsedValue(int requestId, byte[] body) {
        if (body.length == 1 && confirmedU8(requestId)) {
            report.append("  value_u8=").append(body[0] & 0xFF).append('\n');
            return;
        }
        String text = asciiOrNull(body);
        if (text != null) {
            report.append("  text=").append(text).append(" note=heuristic\n");
        }
        if (body.length == 1) {
            report.append("  value_u8=").append(body[0] & 0xFF).append(" note=heuristic\n");
        } else if (body.length == 2) {
            int value = (body[0] & 0xFF) | ((body[1] & 0xFF) << 8);
            report.append("  value_u16le=").append(value).append(" note=heuristic\n");
        } else if (body.length == 4) {
            long value = (body[0] & 0xFFL)
                    | ((body[1] & 0xFFL) << 8)
                    | ((body[2] & 0xFFL) << 16)
                    | ((body[3] & 0xFFL) << 24);
            report.append("  value_u32le=").append(value)
                    .append(" bytes=")
                    .append(body[0] & 0xFF).append('.')
                    .append(body[1] & 0xFF).append('.')
                    .append(body[2] & 0xFF).append('.')
                    .append(body[3] & 0xFF)
                    .append(" note=heuristic\n");
        }
    }

    private void appendStreamStats(ArrayList<Sample> samples, long startNs, Integer controlStatus) {
        if (controlStatus == null) {
            report.append("  control_reply=timeout\n");
        } else {
            report.append("  control_status=").append(controlStatus.intValue()).append('\n');
        }
        int parsed = 0;
        for (int i = 0; i < samples.size(); i++) {
            if (samples.get(i).parsedOk) {
                parsed++;
            }
        }
        report.append("  samples=").append(samples.size())
                .append(" parsed=").append(parsed)
                .append('\n');
        long elapsedMs = (System.nanoTime() - startNs) / 1_000_000L;
        report.append("  elapsed_ms=").append(elapsedMs).append('\n');
        if (samples.size() < 2) {
            report.append("  actual_hz=n/a\n");
            report.append("  interval_us n=0\n");
        } else {
            long dtNs = samples.get(samples.size() - 1).hostNs - samples.get(0).hostNs;
            if (dtNs <= 0) {
                report.append("  actual_hz=n/a\n");
            } else {
                double hz = (samples.size() - 1) * 1_000_000_000d / (double) dtNs;
                report.append("  actual_hz=").append(String.format(Locale.US, "%.2f", hz)).append('\n');
            }
            long[] deltas = new long[samples.size() - 1];
            for (int i = 1; i < samples.size(); i++) {
                deltas[i - 1] = (samples.get(i).hostNs - samples.get(i - 1).hostNs) / 1000L;
            }
            Arrays.sort(deltas);
            // §VITURE：樣本少，用 nearest-rank，避免內插造出沒收到的間隔。
            report.append("  interval_us n=").append(deltas.length)
                    .append(" method=nearest_rank")
                    .append(" p50=").append(percentile(deltas, 0.50d))
                    .append(" p95=").append(percentile(deltas, 0.95d))
                    .append('\n');
        }
        int shown = Math.min(5, samples.size());
        for (int i = 0; i < shown; i++) {
            appendSample(i, samples.get(i), startNs, false);
        }
        if (samples.size() > 5) {
            appendSample(samples.size() - 1, samples.get(samples.size() - 1), startNs, true);
        }
    }

    private void appendSample(int index, Sample sample, long startNs, boolean last) {
        double tMs = (sample.hostNs - startNs) / 1_000_000d;
        report.append("  [").append(index).append("] t_ms=")
                .append(String.format(Locale.US, "%.3f", tMs))
                .append(" hex=").append(hexForReport(sample.raw))
                .append(' ').append(sample.text);
        if (last) {
            report.append(" last");
        }
        report.append('\n');
    }

    private void sendOff(String label) {
        byte[] tx;
        try {
            tx = VitureV2Protocol.imuControl(VitureV2Protocol.STREAM_OFF);
        } catch (RuntimeException error) {
            report.append(label).append(" error=").append(errorText(error)).append('\n');
            return;
        }
        try {
            report.append(label).append('\n');
            int written = link.write(tx, WRITE_TIMEOUT_MS, pad);
            report.append("  tx=").append(VitureV2Protocol.toHex(tx))
                    .append(" tx_len=").append(tx.length)
                    .append(" padded=").append(pad)
                    .append('\n');
            report.append("  write=").append(written).append('\n');
            long deadline = System.nanoTime() + QUERY_TIMEOUT_MS * 1_000_000L;
            boolean got = false;
            boolean cancelRead = false;
            while (System.nanoTime() < deadline) {
                if (host.isCancelled()) {
                    if (cancelRead) {
                        break;
                    }
                    // 取消時最多再讀一個切片，避免關掉 Activity 時還卡滿整段查詢逾時。
                    cancelRead = true;
                }
                int slice = host.isCancelled() ? READ_SLICE_MS : sliceMs(deadline);
                byte[] rx = link.read(slice);
                if (rx == null) {
                    continue;
                }
                VitureV2Protocol.Frame frame = VitureV2Protocol.parse(rx);
                if (frame == null) {
                    noteUnparsed("imu-off", rx);
                    continue;
                }
                VitureV2Protocol.Reply reply = VitureV2Protocol.reply(frame, VitureV2Protocol.MSG_IMU_CONTROL);
                if (reply != null) {
                    report.append("  status=").append(reply.status)
                            .append(" success=").append(reply.success())
                            .append('\n');
                    VitureUsb.logI("imu off reply status=" + reply.status
                            + " payload=" + frame.payload.length);
                    got = true;
                    return;
                }
                noteOther("imu-off", rx, frame);
            }
            if (!got) {
                report.append("  timeout\n");
            }
        } catch (Exception error) {
            report.append("  error=").append(errorText(error)).append('\n');
            VitureUsb.logW("imu off failed", error);
        } finally {
            imuActive = false;
        }
    }

    private void noteOther(String phase, byte[] rx, VitureV2Protocol.Frame frame) {
        if (frame.msgId == VitureV2Protocol.EVENT_POSE || frame.msgId == VitureV2Protocol.EVENT_RAW) {
            String key = phase + ":" + VitureV2Protocol.hex16(frame.msgId);
            Integer count = unsolicited.get(key);
            unsolicited.put(key, count == null ? 1 : count + 1);
            return;
        }
        String key = phase + ":" + frame.msgId;
        boolean hide = isSerialReply(frame.msgId);
        remember(unknown, key, frame.msgId, phase, hide ? null : rx, rx == null ? 0 : rx.length);
    }

    private void noteUnparsed(String phase, byte[] rx) {
        boolean hide = "get-serial".equals(phase) || isSerialReply(msgIdOf(rx));
        remember(unparsed, phase, -1, phase, hide ? null : rx, rx == null ? 0 : rx.length);
    }

    private void remember(LinkedHashMap<String, Bucket> map, String key, int msgId, String phase,
                          byte[] stored, int length) {
        Bucket bucket = map.get(key);
        if (bucket == null) {
            map.put(key, new Bucket(msgId, phase, stored, length));
        } else {
            bucket.add(length);
        }
    }

    private void appendTallies() {
        report.append("UNKNOWN\n");
        if (unknown.isEmpty()) {
            report.append("none\n");
        } else {
            report.append("ids=").append(unknown.size()).append('\n');
            for (Map.Entry<String, Bucket> entry : unknown.entrySet()) {
                Bucket bucket = entry.getValue();
                report.append("id=").append(VitureV2Protocol.hex16(bucket.msgId))
                        .append(" phase=").append(bucket.phase)
                        .append(" len=").append(bucket.firstLen)
                        .append(" varies=").append(bucket.varies)
                        .append(" count=").append(bucket.count);
                appendFirst(report, bucket);
                report.append('\n');
            }
        }
        report.append("UNSOLICITED\n");
        if (unsolicited.isEmpty()) {
            report.append("none\n");
        } else {
            for (Map.Entry<String, Integer> entry : unsolicited.entrySet()) {
                report.append("id=").append(entry.getKey())
                        .append(" count=").append(entry.getValue())
                        .append('\n');
            }
        }
        report.append("UNPARSED\n");
        if (unparsed.isEmpty()) {
            report.append("none\n");
        } else {
            for (Map.Entry<String, Bucket> entry : unparsed.entrySet()) {
                Bucket bucket = entry.getValue();
                report.append("phase=").append(bucket.phase)
                        .append(" len=").append(bucket.firstLen)
                        .append(" varies=").append(bucket.varies)
                        .append(" count=").append(bucket.count);
                appendFirst(report, bucket);
                report.append('\n');
            }
        }
    }

    private static int sliceMs(long deadlineNs) {
        long remainingMs = (deadlineNs - System.nanoTime()) / 1_000_000L;
        if (remainingMs < 1) {
            return 1;
        }
        if (remainingMs > READ_SLICE_MS) {
            return READ_SLICE_MS;
        }
        return (int) remainingMs;
    }

    private static long percentile(long[] sorted, double p) {
        int n = sorted.length;
        int rank = (int) Math.ceil(p * n);
        if (rank < 1) {
            rank = 1;
        }
        if (rank > n) {
            rank = n;
        }
        return sorted[rank - 1];
    }

    // §VITURE：參考連線只把這三個指令的單一位元組 body 當成數值，其他長度仍標成推測。
    private static boolean confirmedU8(int requestId) {
        return requestId == VitureV2Protocol.MSG_GET_BRIGHTNESS
                || requestId == VitureV2Protocol.MSG_GET_VOLUME
                || requestId == VitureV2Protocol.MSG_GET_DISPLAY;
    }

    private static String getName(int id) {
        switch (id) {
            case VitureV2Protocol.MSG_GET_SERIAL:
                return "serial";
            case VitureV2Protocol.MSG_GET_VERSION:
                return "version";
            case VitureV2Protocol.MSG_GET_BRIGHTNESS:
                return "brightness";
            case VitureV2Protocol.MSG_GET_DUTY_CYCLE:
                return "duty";
            case VitureV2Protocol.MSG_GET_DISPLAY:
                return "display";
            case VitureV2Protocol.MSG_GET_VOLUME:
                return "volume";
            case VitureV2Protocol.MSG_GET_WEAR:
                return "wear";
            default:
                return "get";
        }
    }

    /**
     * 序號回覆的內容只能經過這裡再寫進報告。長度 6 以下全部遮住，7 到 8 只留頭尾各 1，更長留頭 4 尾 2。
     * asHex 為 false 且內容不是 ASCII 時回傳 null。
     */
    static String maskSerial(byte[] body, boolean asHex) {
        if (!asHex) {
            String text = asciiOrNull(body);
            if (text == null) {
                return null;
            }
            int[] bounds = serialMaskBounds(text.length());
            char[] chars = text.toCharArray();
            for (int i = bounds[0]; i < bounds[1]; i++) {
                chars[i] = '*';
            }
            return new String(chars);
        }
        if (body == null || body.length == 0) {
            return "";
        }
        int[] bounds = serialMaskBounds(body.length);
        StringBuilder out = new StringBuilder(body.length * 2);
        for (int i = 0; i < body.length; i++) {
            if (i >= bounds[0] && i < bounds[1]) {
                out.append("**");
            } else {
                out.append(String.format(Locale.US, "%02X", body[i] & 0xFF));
            }
        }
        return out.toString();
    }

    private static int[] serialMaskBounds(int length) {
        if (length <= 6) {
            return new int[] {0, length};
        }
        if (length <= 8) {
            return new int[] {1, length - 1};
        }
        return new int[] {4, length - 2};
    }

    private static void appendFirst(StringBuilder report, Bucket bucket) {
        if (bucket.first == null) {
            return;
        }
        report.append(" first=").append(hexForReport(bucket.first));
    }

    private static String hexForReport(byte[] rx) {
        if (isSerialReply(msgIdOf(rx))) {
            return maskSerialPacket(rx);
        }
        return VitureV2Protocol.toHex(rx);
    }

    private static String maskSerialPacket(byte[] packet) {
        int header = Math.min(VitureV2Protocol.HEADER_SIZE, packet.length);
        StringBuilder out = new StringBuilder();
        out.append(VitureV2Protocol.toHex(java.util.Arrays.copyOf(packet, header)));
        if (packet.length <= header) {
            return out.toString();
        }
        byte[] payload = java.util.Arrays.copyOfRange(packet, header, packet.length);
        if (payload.length <= 1) {
            out.append(maskSerial(payload, true));
            return out.toString();
        }
        out.append(String.format(Locale.US, "%02X", payload[0] & 0xFF));
        byte[] secret = java.util.Arrays.copyOfRange(payload, 1, payload.length);
        out.append(maskSerial(secret, true));
        return out.toString();
    }

    private static boolean isSerialReply(int msgId) {
        return msgId == ((VitureV2Protocol.MSG_GET_SERIAL + VitureV2Protocol.RESPONSE_OFFSET) & 0xFFFF);
    }

    private static int msgIdOf(byte[] rx) {
        if (rx == null || rx.length < 4) {
            return -1;
        }
        return (rx[2] & 0xFF) | ((rx[3] & 0xFF) << 8);
    }

    private static String asciiOrNull(byte[] body) {
        if (body == null || body.length == 0) {
            return null;
        }
        int end = body.length;
        while (end > 0 && body[end - 1] == 0) {
            end--;
        }
        if (end == 0) {
            return null;
        }
        for (int i = 0; i < end; i++) {
            int value = body[i] & 0xFF;
            if (value < 0x20 || value > 0x7E) {
                return null;
            }
        }
        return new String(body, 0, end, java.nio.charset.StandardCharsets.US_ASCII);
    }

    private static final class Exchange {
        boolean gotReply;
        boolean sawFrame;
    }

    private static final class Bucket {
        final int msgId;
        final String phase;
        final byte[] first;
        final int firstLen;
        int count = 1;
        boolean varies;

        Bucket(int msgId, String phase, byte[] raw, int length) {
            this.msgId = msgId;
            this.phase = phase;
            this.firstLen = length;
            this.first = raw == null ? null : Arrays.copyOf(raw, raw.length);
        }

        void add(int length) {
            count++;
            if (length != firstLen) {
                varies = true;
            }
        }
    }

    private static final class Sample {
        final long hostNs;
        final byte[] raw;
        final String text;
        final boolean parsedOk;

        Sample(long hostNs, byte[] raw, VitureV2Protocol.Frame frame) {
            this.hostNs = hostNs;
            this.raw = raw;
            if (frame.msgId == VitureV2Protocol.EVENT_POSE) {
                VitureV2Protocol.Pose pose = VitureV2Protocol.pose(frame);
                if (pose == null) {
                    this.parsedOk = false;
                    this.text = "pose parsed=fail";
                } else {
                    this.parsedOk = true;
                    this.text = "pose tick=" + pose.deviceTick
                            + " unknown=" + pose.unknown
                            + " q=" + formatFloats(pose.quaternion);
                }
            } else if (frame.msgId == VitureV2Protocol.EVENT_RAW) {
                VitureV2Protocol.RawSample sample = VitureV2Protocol.raw(frame);
                if (sample == null) {
                    this.parsedOk = false;
                    this.text = "raw parsed=fail";
                } else {
                    this.parsedOk = true;
                    this.text = "raw tick=" + sample.deviceTick
                            + " gyro_rad_s=" + formatFloats(sample.gyroRadPerSec)
                            + " accel_g=" + formatFloats(sample.accelG);
                }
            } else {
                this.parsedOk = false;
                this.text = "parsed=fail";
            }
        }

        private static String formatFloats(float[] values) {
            StringBuilder out = new StringBuilder();
            for (int i = 0; i < values.length; i++) {
                if (i > 0) {
                    out.append(',');
                }
                out.append(String.format(Locale.US, "%.6f", values[i]));
            }
            return out.toString();
        }
    }
}
