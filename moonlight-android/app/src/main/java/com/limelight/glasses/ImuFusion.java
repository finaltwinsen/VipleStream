package com.limelight.glasses;

/**
 * §VITURE：頭部方向跟 VIPLE_VR_POSE 同一套。
 * VipleVr.h 的 rot 是 x,y,z,w。qxr_synthetic.cpp 的 yawPitchToQuat 把 yaw 放在繞 +Y、pitch 放在繞 +X，
 * 控制器在頭的前下方（z 比頭少 0.3 m），所以 +Y 朝上、+X 朝右、前方是 -Z。
 * 靜止時加速度計讀到的是比力，約 +1 g 朝上；水平時頭部加速度是 (0, +1, 0)。
 * 正的 X 角速度是抬頭，正的 Y 是面朝左轉，正的 Z 是左耳靠向左肩。
 * 四元數是 body-to-world：v_world = q * v_body * q_conj。輸出再左乘 yaw 偏移。
 * 沒有磁力計，yaw 會慢慢漂。
 */
public final class ImuFusion {
    // §VITURE：400 Hz 時 Kp 太大，一步就跨過傾斜。1.0 大約讓 10° 的誤差在 1 秒內回來。
    public static final float KP = 1.0f;
    // §VITURE：Ki 只修零偏殘差。0.02 讓 0.05 的誤差每秒大約累積 0.001 rad/s，積分才不會暴衝。
    public static final float KI = 0.02f;
    // §VITURE：頭在加速時模長離開 1 g，這時的加速度不再是重力，修正會把 pitch 拉歪。
    static final float ACCEL_GATE_G = 0.15f;
    static final float STILL_SECONDS = 1.0f;
    // §VITURE：等速旋轉的變異數接近 0，所以還要看平均角速度，否則一直轉頭也會被當成靜止。
    static final float STILL_GYRO_STD = 0.015f;
    static final float STILL_ACCEL_STD = 0.03f;
    static final float STILL_MEAN_OMEGA = 0.05f;
    static final float STILL_ACCEL_NORM = 0.1f;
    static final float BIAS_BLEND_OLD = 0.8f;
    static final float BIAS_BLEND_NEW = 0.2f;
    static final float INTEGRATOR_LIMIT = 0.5f;
    // §VITURE：序號跳太大代表中間掉了一串樣本，整段積分會把四元數轉飛。
    static final int SEQ_DELTA_CAP = 20;
    static final long INIT_NS = 500_000_000L;
    // 切 3D 時 IMU 會停。只看首尾相減的話，兩筆隔很久的樣本也會被當成初始化完成。
    static final int INIT_MIN_SAMPLES = 100;
    static final long INIT_GAP_NS = 50_000_000L;
    static final float DEFAULT_HZ = 400f;
    static final int RATE_WINDOW = 32;
    static final int STILL_CAP = 2000;

    // §VITURE：感測器安裝方向還沒在 Beast 上核對。先用單位矩陣，等軸向測試報告再改。
    private final int[] perm = new int[] {0, 1, 2};
    private final int[] sign = new int[] {1, 1, 1};

    private float qx;
    private float qy;
    private float qz;
    private float qw = 1f;
    private float ox;
    private float oy;
    private float oz;
    private float ow = 1f;
    private float biasX;
    private float biasY;
    private float biasZ;
    private float intX;
    private float intY;
    private float intZ;
    private float outQx;
    private float outQy;
    private float outQz;
    private float outQw = 1f;
    private float outWx;
    private float outWy;
    private float outWz;
    private boolean ready;
    private int initCount;
    private double initAx;
    private double initAy;
    private double initAz;
    private long initFirstNs;
    private long initLastNs;
    private long lastSeq;
    private long sequence;
    private long arrivalNs;
    private float hz = DEFAULT_HZ;
    private final long[] arrivals = new long[RATE_WINDOW];
    private int rateCount;
    private int ratePos;
    private final float[] stillGx = new float[STILL_CAP];
    private final float[] stillGy = new float[STILL_CAP];
    private final float[] stillGz = new float[STILL_CAP];
    private final float[] stillAx = new float[STILL_CAP];
    private final float[] stillAy = new float[STILL_CAP];
    private final float[] stillAz = new float[STILL_CAP];
    private int stillHead;
    private int stillCount;
    private int stillWindow;
    private int stillSinceResync;
    private double sumGx;
    private double sumGy;
    private double sumGz;
    private double sumGxx;
    private double sumGyy;
    private double sumGzz;
    private double sumAx;
    private double sumAy;
    private double sumAz;
    private double sumAxx;
    private double sumAyy;
    private double sumAzz;
    private double sumOmega;

    public synchronized void setAxisMapping(int[] newPerm, int[] newSign) {
        if (newPerm == null || newSign == null || newPerm.length != 3 || newSign.length != 3) {
            throw new IllegalArgumentException("axis");
        }
        boolean[] used = new boolean[3];
        for (int i = 0; i < 3; i++) {
            int axis = newPerm[i];
            if (axis < 0 || axis > 2 || used[axis]) {
                throw new IllegalArgumentException("perm");
            }
            used[axis] = true;
            if (newSign[i] != 1 && newSign[i] != -1) {
                throw new IllegalArgumentException("sign");
            }
        }
        System.arraycopy(newPerm, 0, perm, 0, 3);
        System.arraycopy(newSign, 0, sign, 0, 3);
    }

    /** @return 格式不對就維持原本的對應。開流之前呼叫，中途改軸會把舊四元數留在舊座標。 */
    public synchronized boolean applyAxisText(String text) {
        int[] parsedPerm = new int[3];
        int[] parsedSign = new int[3];
        if (!VitureAxisMap.parse(text, parsedPerm, parsedSign)) {
            return false;
        }
        setAxisMapping(parsedPerm, parsedSign);
        return true;
    }

    /** §VITURE：只消掉 yaw。pitch／roll 還在，使用者按回正才不會被拉去看地板。 */
    public synchronized void recenter() {
        publish();
        float yaw = extractYaw(outQx, outQy, outQz, outQw);
        float half = -0.5f * yaw;
        float sy = (float) Math.sin(half);
        float cy = (float) Math.cos(half);
        float nx = cy * ox + sy * oz;
        float ny = cy * oy + sy * ow;
        float nz = cy * oz - sy * ox;
        float nw = cy * ow - sy * oy;
        float norm = (float) Math.sqrt(nx * nx + ny * ny + nz * nz + nw * nw);
        if (norm < 1.0e-8f) {
            ox = 0f;
            oy = 0f;
            oz = 0f;
            ow = 1f;
        } else {
            float inv = 1f / norm;
            ox = nx * inv;
            oy = ny * inv;
            oz = nz * inv;
            ow = nw * inv;
        }
        publish();
    }

    /**
     * @param sequence 裝置時間戳，每筆 +1，uint32 會回繞
     * @param arrivalNs 呼叫端的 System.nanoTime，測試才能喂固定的到達時間
     * @return 已經連續收滿 0.5 秒而且至少 100 筆、可以讀方向
     */
    public synchronized boolean update(float sensorGx, float sensorGy, float sensorGz,
                                       float sensorAx, float sensorAy, float sensorAz,
                                       long sequence, long arrivalNs) {
        float gx = mapped(sensorGx, sensorGy, sensorGz, 0);
        float gy = mapped(sensorGx, sensorGy, sensorGz, 1);
        float gz = mapped(sensorGx, sensorGy, sensorGz, 2);
        float ax = mapped(sensorAx, sensorAy, sensorAz, 0);
        float ay = mapped(sensorAx, sensorAy, sensorAz, 1);
        float az = mapped(sensorAx, sensorAy, sensorAz, 2);
        noteArrival(arrivalNs);
        this.arrivalNs = arrivalNs;
        long seq = sequence & 0xFFFFFFFFL;
        this.sequence = seq;
        if (!ready) {
            if (initCount > 0 && arrivalNs - initLastNs > INIT_GAP_NS) {
                initCount = 0;
                initAx = 0d;
                initAy = 0d;
                initAz = 0d;
            }
            if (initCount == 0) {
                initFirstNs = arrivalNs;
            }
            initAx += ax;
            initAy += ay;
            initAz += az;
            initCount++;
            initLastNs = arrivalNs;
            lastSeq = seq;
            if (initCount >= INIT_MIN_SAMPLES && arrivalNs - initFirstNs >= INIT_NS) {
                snapFromAccel();
                ready = true;
                outWx = gx - biasX;
                outWy = gy - biasY;
                outWz = gz - biasZ;
                publish();
                pushStill(gx, gy, gz, ax, ay, az);
            }
            return ready;
        }
        long delta = (seq - lastSeq) & 0xFFFFFFFFL;
        lastSeq = seq;
        if (delta > SEQ_DELTA_CAP) {
            delta = SEQ_DELTA_CAP;
        }
        float dt = delta / hz;
        if (dt > 0f) {
            integrate(gx, gy, gz, ax, ay, az, dt);
        }
        outWx = gx - biasX;
        outWy = gy - biasY;
        outWz = gz - biasZ;
        publish();
        pushStill(gx, gy, gz, ax, ay, az);
        return true;
    }

    public synchronized boolean hasPose() {
        return ready;
    }

    /** x, y, z, w。含回正偏移。 */
    public synchronized void copyQuaternion(float[] xyzw) {
        if (xyzw == null || xyzw.length < 4) {
            throw new IllegalArgumentException("quat");
        }
        xyzw[0] = outQx;
        xyzw[1] = outQy;
        xyzw[2] = outQz;
        xyzw[3] = outQw;
    }

    public synchronized void copyAngularVelocity(float[] xyz) {
        if (xyz == null || xyz.length < 3) {
            throw new IllegalArgumentException("gyro");
        }
        xyz[0] = outWx;
        xyz[1] = outWy;
        xyz[2] = outWz;
    }

    public synchronized long sequence() {
        return sequence;
    }

    public synchronized long arrivalNs() {
        return arrivalNs;
    }

    public synchronized float measuredHz() {
        return hz;
    }

    /** yaw、pitch、roll，度。pitch 正值是抬頭，roll 正值是左耳向下。 */
    public static void eulerDegrees(float x, float y, float z, float w, float[] yawPitchRollDeg) {
        if (yawPitchRollDeg == null || yawPitchRollDeg.length < 3) {
            throw new IllegalArgumentException("euler");
        }
        float fx = -2f * (x * z + w * y);
        float fy = -2f * (y * z - w * x);
        float fz = -(1f - 2f * (x * x + y * y));
        float yaw = (float) Math.atan2(-fx, -fz);
        float pitch = (float) Math.atan2(fy, Math.hypot(fx, fz));
        float r10 = 2f * (x * y + w * z);
        float r11 = 1f - 2f * (x * x + z * z);
        float roll = (float) Math.atan2(r10, r11);
        yawPitchRollDeg[0] = (float) Math.toDegrees(yaw);
        yawPitchRollDeg[1] = (float) Math.toDegrees(pitch);
        yawPitchRollDeg[2] = (float) Math.toDegrees(roll);
    }

    private float mapped(float x, float y, float z, int headAxis) {
        return sign[headAxis] * component(perm[headAxis], x, y, z);
    }

    private static float component(int axis, float x, float y, float z) {
        if (axis == 0) {
            return x;
        }
        if (axis == 1) {
            return y;
        }
        return z;
    }

    private void noteArrival(long timeNs) {
        arrivals[ratePos] = timeNs;
        ratePos = (ratePos + 1) % RATE_WINDOW;
        if (rateCount < RATE_WINDOW) {
            rateCount++;
        }
        if (rateCount < RATE_WINDOW) {
            return;
        }
        long oldest = arrivals[ratePos];
        long newest = arrivals[(ratePos + RATE_WINDOW - 1) % RATE_WINDOW];
        long span = newest - oldest;
        if (span <= 0L) {
            return;
        }
        float instant = (float) ((RATE_WINDOW - 1) * 1.0e9 / (double) span);
        hz += 0.05f * (instant - hz);
        if (hz < 50f) {
            hz = 50f;
        } else if (hz > 2000f) {
            hz = 2000f;
        }
    }

    private void snapFromAccel() {
        float ax = (float) (initAx / initCount);
        float ay = (float) (initAy / initCount);
        float az = (float) (initAz / initCount);
        float norm = (float) Math.sqrt(ax * ax + ay * ay + az * az);
        intX = 0f;
        intY = 0f;
        intZ = 0f;
        if (norm < 0.2f) {
            // §VITURE：加速度幾乎是 0 時沒有傾斜可對，用單位四元數才不會算出 NaN。
            qx = 0f;
            qy = 0f;
            qz = 0f;
            qw = 1f;
            return;
        }
        float inv = 1f / norm;
        ax *= inv;
        ay *= inv;
        az *= inv;
        if (ay < -0.999f && ax * ax + az * az < 1.0e-4f) {
            qx = 1f;
            qy = 0f;
            qz = 0f;
            qw = 0f;
            return;
        }
        // §VITURE：a × up，up = (0, 1, 0)。最短旋轉把量到的朝上方向轉到世界 +Y，yaw 維持 0。
        qx = -az;
        qy = 0f;
        qz = ax;
        qw = 1f + ay;
        normalizeBody();
    }

    private void integrate(float gx, float gy, float gz, float ax, float ay, float az, float dt) {
        float ex = 0f;
        float ey = 0f;
        float ez = 0f;
        float accelNorm = (float) Math.sqrt(ax * ax + ay * ay + az * az);
        boolean correct = accelNorm > 1.0e-3f && Math.abs(accelNorm - 1f) <= ACCEL_GATE_G;
        if (correct) {
            float inv = 1f / accelNorm;
            float anx = ax * inv;
            float any = ay * inv;
            float anz = az * inv;
            // §VITURE：預測的機體朝上是 R^T * (0, 1, 0)，也就是旋轉矩陣的第二列。
            float vx = 2f * (qx * qy + qw * qz);
            float vy = 1f - 2f * (qx * qx + qz * qz);
            float vz = 2f * (qy * qz - qw * qx);
            ex = any * vz - anz * vy;
            ey = anz * vx - anx * vz;
            ez = anx * vy - any * vx;
            intX = clamp(intX + ex * dt);
            intY = clamp(intY + ey * dt);
            intZ = clamp(intZ + ez * dt);
        }
        float wx = gx - biasX + KP * ex + (correct ? KI * intX : 0f);
        float wy = gy - biasY + KP * ey + (correct ? KI * intY : 0f);
        float wz = gz - biasZ + KP * ez + (correct ? KI * intZ : 0f);
        float half = 0.5f * dt;
        float nx = qx + half * (qw * wx + qy * wz - qz * wy);
        float ny = qy + half * (qw * wy - qx * wz + qz * wx);
        float nz = qz + half * (qw * wz + qx * wy - qy * wx);
        float nw = qw + half * (-qx * wx - qy * wy - qz * wz);
        qx = nx;
        qy = ny;
        qz = nz;
        qw = nw;
        normalizeBody();
    }

    private void pushStill(float gx, float gy, float gz, float ax, float ay, float az) {
        // 400 Hz 時若每筆都重掃一整窗，融合鎖會被佔住。進一筆加、出一筆減。
        int slot = stillHead;
        if (stillWindow >= STILL_CAP) {
            // 視窗已經佔滿整圈，下一格就是最舊的樣本。先退出總和再寫，否則被蓋掉的值會留在 sum。
            accumulateStill(slot, -1d);
            stillWindow--;
        }
        stillGx[slot] = gx;
        stillGy[slot] = gy;
        stillGz[slot] = gz;
        stillAx[slot] = ax;
        stillAy[slot] = ay;
        stillAz[slot] = az;
        stillHead = (stillHead + 1) % STILL_CAP;
        if (stillCount < STILL_CAP) {
            stillCount++;
        }
        accumulateStill(slot, 1d);
        stillWindow++;
        int need = Math.round(hz * STILL_SECONDS);
        if (need < 8) {
            need = 8;
        } else if (need > STILL_CAP) {
            need = STILL_CAP;
        }
        int target = stillCount < need ? stillCount : need;
        while (stillWindow > target) {
            accumulateStill(ringIndex(stillHead - stillWindow), -1d);
            stillWindow--;
        }
        stillSinceResync++;
        if (stillSinceResync >= need) {
            // 加減累久了浮點會漂。每滿一窗用環形緩衝重算一次。
            resyncStill();
            stillSinceResync = 0;
        }
        if (stillCount < need || stillWindow != need) {
            return;
        }
        double inv = 1d / stillWindow;
        double gyroVarLimit = (double) STILL_GYRO_STD * STILL_GYRO_STD;
        double accelVarLimit = (double) STILL_ACCEL_STD * STILL_ACCEL_STD;
        if (variance(sumGx, sumGxx, inv) > gyroVarLimit
                || variance(sumGy, sumGyy, inv) > gyroVarLimit
                || variance(sumGz, sumGzz, inv) > gyroVarLimit
                || variance(sumAx, sumAxx, inv) > accelVarLimit
                || variance(sumAy, sumAyy, inv) > accelVarLimit
                || variance(sumAz, sumAzz, inv) > accelVarLimit
                || sumOmega * inv >= STILL_MEAN_OMEGA) {
            return;
        }
        double meanAx = sumAx * inv;
        double meanAy = sumAy * inv;
        double meanAz = sumAz * inv;
        double meanNorm = Math.sqrt(meanAx * meanAx + meanAy * meanAy + meanAz * meanAz);
        if (Math.abs(meanNorm - 1d) > STILL_ACCEL_NORM) {
            return;
        }
        biasX = BIAS_BLEND_OLD * biasX + BIAS_BLEND_NEW * (float) (sumGx * inv);
        biasY = BIAS_BLEND_OLD * biasY + BIAS_BLEND_NEW * (float) (sumGy * inv);
        biasZ = BIAS_BLEND_OLD * biasZ + BIAS_BLEND_NEW * (float) (sumGz * inv);
        // §VITURE：零偏一更新，舊的積分還在拉同一個方向，會把剛修掉的誤差加回去。
        intX = 0f;
        intY = 0f;
        intZ = 0f;
        stillCount = 0;
        stillWindow = 0;
        stillSinceResync = 0;
        clearStillSums();
    }

    private void accumulateStill(int index, double sign) {
        double sgx = stillGx[index];
        double sgy = stillGy[index];
        double sgz = stillGz[index];
        double sax = stillAx[index];
        double say = stillAy[index];
        double saz = stillAz[index];
        sumGx += sign * sgx;
        sumGy += sign * sgy;
        sumGz += sign * sgz;
        sumGxx += sign * sgx * sgx;
        sumGyy += sign * sgy * sgy;
        sumGzz += sign * sgz * sgz;
        sumAx += sign * sax;
        sumAy += sign * say;
        sumAz += sign * saz;
        sumAxx += sign * sax * sax;
        sumAyy += sign * say * say;
        sumAzz += sign * saz * saz;
        sumOmega += sign * Math.sqrt(sgx * sgx + sgy * sgy + sgz * sgz);
    }

    private void clearStillSums() {
        sumGx = 0d;
        sumGy = 0d;
        sumGz = 0d;
        sumGxx = 0d;
        sumGyy = 0d;
        sumGzz = 0d;
        sumAx = 0d;
        sumAy = 0d;
        sumAz = 0d;
        sumAxx = 0d;
        sumAyy = 0d;
        sumAzz = 0d;
        sumOmega = 0d;
    }

    private int ringIndex(int index) {
        // 視窗長度可能等於緩衝容量，差一次減完仍是負的，不能只加一次容量。
        index %= STILL_CAP;
        if (index < 0) {
            index += STILL_CAP;
        }
        return index;
    }

    private void resyncStill() {
        clearStillSums();
        if (stillWindow <= 0) {
            return;
        }
        int index = ringIndex(stillHead - stillWindow);
        for (int i = 0; i < stillWindow; i++) {
            accumulateStill(index, 1d);
            index++;
            if (index == STILL_CAP) {
                index = 0;
            }
        }
    }

    private static double variance(double sum, double sumSq, double inv) {
        double mean = sum * inv;
        double value = sumSq * inv - mean * mean;
        return value < 0d ? 0d : value;
    }

    private static float clamp(float value) {
        if (value > INTEGRATOR_LIMIT) {
            return INTEGRATOR_LIMIT;
        }
        if (value < -INTEGRATOR_LIMIT) {
            return -INTEGRATOR_LIMIT;
        }
        return value;
    }

    private void normalizeBody() {
        float norm = (float) Math.sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
        if (norm < 1.0e-8f) {
            qx = 0f;
            qy = 0f;
            qz = 0f;
            qw = 1f;
            return;
        }
        float inv = 1f / norm;
        qx *= inv;
        qy *= inv;
        qz *= inv;
        qw *= inv;
    }

    private void publish() {
        outQx = ow * qx + ox * qw + oy * qz - oz * qy;
        outQy = ow * qy - ox * qz + oy * qw + oz * qx;
        outQz = ow * qz + ox * qy - oy * qx + oz * qw;
        outQw = ow * qw - ox * qx - oy * qy - oz * qz;
    }

    private static float extractYaw(float x, float y, float z, float w) {
        float fx = -2f * (x * z + w * y);
        float fz = -(1f - 2f * (x * x + y * y));
        return (float) Math.atan2(-fx, -fz);
    }
}
