// §VITURE：專案目前沒有 JUnit 相依，要加 testImplementation 'junit:junit:4.13.2' 這個檔才會被編譯。不要為了它改 gradle。
package com.limelight.glasses;

import org.junit.Assert;
import org.junit.Test;

public class ImuFusionTest {
    private static final long STEP_NS = 2_500_000L;
    private static final float YAW_RATE = (float) (Math.PI / 2.0);

    @Test
    public void eulerMatchesTheSyntheticAxes() {
        float[] euler = new float[3];
        ImuFusion.eulerDegrees(0f, 0f, 0f, 1f, euler);
        assertEuler(euler, 0f, 0f, 0f);
        float s = (float) Math.sin(Math.PI / 4.0);
        float c = (float) Math.cos(Math.PI / 4.0);
        ImuFusion.eulerDegrees(0f, s, 0f, c, euler);
        assertEuler(euler, 90f, 0f, 0f);
        float sp = (float) Math.sin(Math.toRadians(10.0));
        float cp = (float) Math.cos(Math.toRadians(10.0));
        ImuFusion.eulerDegrees(sp, 0f, 0f, cp, euler);
        assertEuler(euler, 0f, 20f, 0f);
        ImuFusion.eulerDegrees(0f, 0f, sp, cp, euler);
        assertEuler(euler, 0f, 0f, 20f);
    }

    @Test
    public void stillLevelStaysLevelAndGravityPointsUp() {
        ImuFusion fusion = new ImuFusion();
        feed(fusion, 0f, 0f, 0f, 0f, 1f, 0f, 220, 0L, 0L);
        Assert.assertTrue(fusion.hasPose());
        float[] first = eulerOf(fusion);
        assertEuler(first, 0f, 0f, 0f);
        assertBodyAxis(fusion, 1, 0f, 1f, 0f);
        feed(fusion, 0f, 0f, 0f, 0f, 1f, 0f, 200, 220L, 220L * STEP_NS);
        float[] later = eulerOf(fusion);
        Assert.assertEquals(first[0], later[0], 0.5f);
        Assert.assertEquals(first[1], later[1], 0.5f);
        Assert.assertEquals(first[2], later[2], 0.5f);
        assertBodyAxis(fusion, 1, 0f, 1f, 0f);
    }

    @Test
    public void accelAlongPositiveZIsLookDown() {
        ImuFusion fusion = new ImuFusion();
        feed(fusion, 0f, 0f, 0f, 0f, 0f, 1f, 220, 0L, 0L);
        float[] euler = eulerOf(fusion);
        Assert.assertEquals(-90f, euler[1], 2f);
        assertBodyAxis(fusion, 2, 0f, 1f, 0f);
    }

    @Test
    public void constantYawIsWithinTwoDegrees() {
        ImuFusion fusion = new ImuFusion();
        feed(fusion, 0f, 0f, 0f, 0f, 1f, 0f, 201, 0L, 0L);
        feed(fusion, 0f, YAW_RATE, 0f, 0f, 1f, 0f, 400, 201L, 201L * STEP_NS);
        float[] euler = eulerOf(fusion);
        Assert.assertEquals(90f, euler[0], 2f);
        Assert.assertEquals(0f, euler[1], 2f);
        Assert.assertEquals(0f, euler[2], 2f);
    }

    @Test
    public void sequenceThatStepsByFourStillIntegratesOneSamplePerUpdate() {
        // §VITURE-DT：Beast 的序號是裝置時鐘，每筆跳 4～5。轉頭角度不可因此放大。
        ImuFusion fusion = new ImuFusion();
        feedStep(fusion, 0f, 0f, 0f, 0f, 1f, 0f, 201, 0L, 0L, 4);
        feedStep(fusion, 0f, YAW_RATE, 0f, 0f, 1f, 0f, 400, 201L * 4, 201L * STEP_NS, 4);
        float[] euler = eulerOf(fusion);
        Assert.assertEquals(90f, euler[0], 2f);
    }

    private static void feedStep(ImuFusion fusion, float gx, float gy, float gz,
                                 float ax, float ay, float az, int count, long sequence, long arrivalNs, int step) {
        for (int i = 0; i < count; i++) {
            fusion.update(gx, gy, gz, ax, ay, az, sequence + (long) i * step, arrivalNs + i * STEP_NS);
        }
    }

    @Test
    public void recenterZerosYawAndKeepsPitch() {
        ImuFusion fusion = new ImuFusion();
        float pitch = (float) Math.toRadians(20.0);
        float accelY = (float) Math.cos(pitch);
        float accelZ = (float) -Math.sin(pitch);
        feed(fusion, 0f, 0f, 0f, 0f, accelY, accelZ, 201, 0L, 0L);
        float cos = (float) Math.cos(pitch);
        float sin = (float) Math.sin(pitch);
        feed(fusion, 0f, YAW_RATE * cos, -YAW_RATE * sin, 0f, accelY, accelZ, 178, 201L, 201L * STEP_NS);
        float[] before = eulerOf(fusion);
        Assert.assertEquals(40f, before[0], 2f);
        Assert.assertEquals(20f, before[1], 2f);
        fusion.recenter();
        float[] after = eulerOf(fusion);
        Assert.assertEquals(0f, after[0], 2f);
        Assert.assertEquals(20f, after[1], 2f);
        Assert.assertEquals(0f, after[2], 2f);
    }

    @Test
    public void axisPermutationAndSignsAreApplied() {
        ImuFusion swapped = new ImuFusion();
        Assert.assertTrue(swapped.applyAxisText("-z,+x,-y"));
        feed(swapped, 0f, 0f, 0f, 1f, 0f, 0f, 201, 0L, 0L);
        assertBodyAxis(swapped, 1, 0f, 1f, 0f);
        feed(swapped, YAW_RATE, 0f, 0f, 1f, 0f, 0f, 400, 201L, 201L * STEP_NS);
        float[] swappedEuler = eulerOf(swapped);
        Assert.assertEquals(90f, swappedEuler[0], 2f);
        Assert.assertEquals(0f, swappedEuler[1], 2f);

        ImuFusion flipped = new ImuFusion();
        flipped.setAxisMapping(new int[] {0, 1, 2}, new int[] {-1, -1, -1});
        feed(flipped, 0f, 0f, 0f, 0f, -1f, 0f, 201, 0L, 0L);
        assertBodyAxis(flipped, 1, 0f, 1f, 0f);
        feed(flipped, 0f, -YAW_RATE, 0f, 0f, -1f, 0f, 400, 201L, 201L * STEP_NS);
        float[] flippedEuler = eulerOf(flipped);
        Assert.assertEquals(90f, flippedEuler[0], 2f);
        Assert.assertEquals(0f, flippedEuler[1], 2f);
    }

    @Test
    public void initRestartsWhenTheStreamGapsAndNeedsAHundredSamples() {
        ImuFusion sparse = new ImuFusion();
        // 兩筆隔 0.6 秒：時間跨過 0.5 秒，但樣本數與斷訊都不合格。
        sparse.update(0f, 0f, 0f, 0f, 1f, 0f, 1L, 0L);
        sparse.update(0f, 0f, 0f, 0f, 1f, 0f, 2L, 600_000_000L);
        Assert.assertFalse(sparse.hasPose());

        ImuFusion gapped = new ImuFusion();
        for (int i = 0; i < 80; i++) {
            gapped.update(0f, 0f, 0f, 0f, 1f, 0f, i, i * 5_000_000L);
        }
        Assert.assertFalse(gapped.hasPose());
        long restart = 80L * 5_000_000L + 60_000_000L;
        for (int i = 0; i < 100; i++) {
            gapped.update(0f, 0f, 0f, 0f, 1f, 0f, 1000L + i, restart + i * 5_000_000L);
        }
        // 100 筆、間隔 5 ms，跨距 495 ms，還沒到 0.5 秒。
        Assert.assertFalse(gapped.hasPose());
        gapped.update(0f, 0f, 0f, 0f, 1f, 0f, 1100L, restart + 100L * 5_000_000L);
        Assert.assertTrue(gapped.hasPose());
    }

    @Test
    public void illegalAxisTextAndMappingAreRejected() {
        ImuFusion fusion = new ImuFusion();
        Assert.assertFalse(fusion.applyAxisText("x,y,z"));
        Assert.assertFalse(fusion.applyAxisText("+x,+x,+y"));
        Assert.assertNull(VitureAxisMap.canonical("nope"));
        Assert.assertEquals("+x,+y,+z", VitureAxisMap.canonical("+x,+y,+z"));
        try {
            fusion.setAxisMapping(new int[] {0, 0, 1}, new int[] {1, 1, 1});
            Assert.fail("expected IllegalArgumentException");
        } catch (IllegalArgumentException expected) {
            // 重複的軸不能當置換。
        }
        feed(fusion, 0f, 0f, 0f, 0f, 1f, 0f, 201, 0L, 0L);
        assertBodyAxis(fusion, 1, 0f, 1f, 0f);
    }

    private static void feed(ImuFusion fusion, float gx, float gy, float gz,
                             float ax, float ay, float az, int count, long sequence, long arrivalNs) {
        for (int i = 0; i < count; i++) {
            fusion.update(gx, gy, gz, ax, ay, az, sequence + i, arrivalNs + i * STEP_NS);
        }
    }

    private static float[] eulerOf(ImuFusion fusion) {
        float[] quat = new float[4];
        fusion.copyQuaternion(quat);
        float[] euler = new float[3];
        ImuFusion.eulerDegrees(quat[0], quat[1], quat[2], quat[3], euler);
        return euler;
    }

    private static void assertEuler(float[] euler, float yaw, float pitch, float roll) {
        Assert.assertEquals(yaw, euler[0], 2f);
        Assert.assertEquals(pitch, euler[1], 2f);
        Assert.assertEquals(roll, euler[2], 2f);
    }

    /** column：0＝機體 +X、1＝+Y、2＝+Z，在世界座標的方向。 */
    private static void assertBodyAxis(ImuFusion fusion, int column, float ex, float ey, float ez) {
        float[] quat = new float[4];
        fusion.copyQuaternion(quat);
        float x = quat[0];
        float y = quat[1];
        float z = quat[2];
        float w = quat[3];
        float wx;
        float wy;
        float wz;
        if (column == 0) {
            wx = 1f - 2f * (y * y + z * z);
            wy = 2f * (x * y + w * z);
            wz = 2f * (x * z - w * y);
        } else if (column == 1) {
            wx = 2f * (x * y - w * z);
            wy = 1f - 2f * (x * x + z * z);
            wz = 2f * (y * z + w * x);
        } else {
            wx = 2f * (x * z + w * y);
            wy = 2f * (y * z - w * x);
            wz = 1f - 2f * (x * x + y * y);
        }
        Assert.assertEquals(ex, wx, 0.05f);
        Assert.assertEquals(ey, wy, 0.05f);
        Assert.assertEquals(ez, wz, 0.05f);
    }
}
