// ============================================================================
// cs_downsample_mv_8x8.hlsl
// §VR-GAME-MV (M-MV0) — 8x8 -> 1 前景優先池化（Closest-Depth Pooling）降採樣 Compute Shader
//
// 輸入：
//   t0: MotionVectors (2D 浮點動態向量，如 R16G16_FLOAT / R32G32_FLOAT)
//   t1: Depth         (深度貼圖，如 D32_FLOAT / R32_FLOAT / R24_UNORM_X8_TYPELESS)
//   t2: Color         (色彩貼圖，供傾印縮圖驗證反向 warp 用)
//
// 輸出：
//   u0: OutMv8        (1/8 解析度雙眼並排 RGBA16F：
//                      .xy = 單眼正規化 UV 位移 (uv_prev - uv_cur)
//                      .z  = 正規化逆深度 invZ in [0, 1] (越大越靠近相機)
//                      .w  = 有效度 validity in [0, 1]
//                      左眼寫入左半 [0, eyeGridW)，右眼寫入右半 [eyeGridW, 2*eyeGridW))
//   u1: OutColor8     (1/8 解析度雙眼並排 RGBA8_UNORM Color 縮圖，僅在傾印開啟時寫入)
//   u2: OutRawMv4     (1/4 原始 MV 貼圖雙眼並排 RG32F，僅在除錯傾印含 "raw" 時由 cs_downsample_raw_mv_4x4 寫入)
// ============================================================================

cbuffer DownsampleCB : register(b0)
{
    uint2  g_MvValidDim;     // 0, 1: MV 有效尺寸 (高解析度 MV 時為 outW x outH 如 2152x2152；低解析度時為 renderW x renderH)
    uint2  g_DepthValidDim;  // 2, 3: Depth 有效算圖尺寸 (如 1436x1436)
    uint2  g_ColorValidDim;  // 4, 5: Color / 算圖基準有效尺寸 (renderW, renderH，如 1436x1436)
    uint2  g_EyeGridDim;     // 6, 7: 單眼 1/8 輸出網格尺寸 (eyeGridW, eyeGridH，如 180x180)
    uint   g_EyeIndex;       // 8:    0 = 左眼 (寫左半), 1 = 右眼 (寫右半)
    uint   g_Flags;          // 9:    b0=hasDepth, b1=depthInverted(Reverse-Z), b2=mvJittered, b3=dumpColor
    float2 g_MvScaleToUv;    // 10,11: (mvScaleX / baseW, mvScaleY / baseH)
    float2 g_JitterDeltaUv;  // 12,13: 若 mvJittered 時需扣除的 (jitterCur - jitterPrev) / renderDim
    float  g_DepthEpsilon;   // 14:   前景深度相對容許比例 (預設 0.05)
    float  g_DepthRef;       // 15:   參考距離 (1m) 對應的硬體逆深度基準值 z_near / world_to_meters (預設 1e-5)
    uint2  g_MvBase;         // 16,17: MV 子區域起始偏移 (DLSS.Input.MV.Subrect.Base.X/Y)
    uint2  g_DepthBase;      // 18,19: Depth 子區域起始偏移 (DLSS.Input.Depth.Subrect.Base.X/Y)
    uint2  g_ColorBase;      // 20,21: Color 子區域起始偏移 (DLSS.Input.Color.Subrect.Base.X/Y)
    uint2  g_RawMv4GridDim;  // 22,23: 單眼 MV 原始貼圖 1/4 縮小版網格尺寸 ((mvValidW+3)/4, (mvValidH+3)/4)
};

Texture2D<float2>         g_MvTex     : register(t0);
Texture2D<float>          g_DepthTex  : register(t1);
Texture2D<float4>         g_ColorTex  : register(t2);

RWTexture2D<float4>       g_OutMv8    : register(u0);
RWTexture2D<unorm float4> g_OutColor8 : register(u1);
RWTexture2D<float2>       g_OutRawMv4 : register(u2);

groupshared float2 s_Mv[64];
groupshared float  s_InvZ[64];
groupshared uint   s_Valid[64];
groupshared float3 s_Color[64];

[numthreads(8, 8, 1)]
void cs_downsample_mv_8x8(
    uint3 gid  : SV_GroupID,
    uint3 gtid : SV_GroupThreadID,
    uint  gi   : SV_GroupIndex)
{
    // 以單眼算圖基準尺寸 (renderW, renderH = g_ColorValidDim) 為 8x8 網格座標系，
    // 透過單眼正規化 UV 對齊高解析度 MV (如 2152x2152) 與低解析度 Depth / Color (如 1436x1436)
    uint2 renderDim = max(g_ColorValidDim, uint2(1u, 1u));
    uint2 renderPx  = gid.xy * 8u + gtid.xy;
    bool inBounds   = (renderPx.x < renderDim.x) && (renderPx.y < renderDim.y);

    uint2 clampedRenderPx = min(renderPx, renderDim - 1u);
    float2 normUv = (float2(clampedRenderPx) + 0.5f) / float2(renderDim);

    uint2 mvDim = max(g_MvValidDim, uint2(1u, 1u));
    uint2 mvPx  = (mvDim.x == renderDim.x && mvDim.y == renderDim.y)
                      ? clampedRenderPx
                      : min(uint2(normUv * float2(mvDim)), mvDim - 1u);
    float2 rawMv = g_MvTex.Load(int3(g_MvBase + mvPx, 0)).xy;

    // 換算為單眼正規化 UV 位移 (uv_prev - uv_cur)
    float2 uvMv = rawMv * g_MvScaleToUv - g_JitterDeltaUv;

    bool isFiniteMv = !any(isnan(uvMv)) && !any(isinf(uvMv)) &&
                      (abs(uvMv.x) <= 2.0f) && (abs(uvMv.y) <= 2.0f);

    // 讀取深度並轉為正規化逆深度 invZ in [0, 1]（越大代表越靠近相機）
    float invZ = 0.0f;
    bool hasDepth = (g_Flags & 1u) != 0u;
    if (hasDepth)
    {
        uint2 depthDim = max(g_DepthValidDim, uint2(1u, 1u));
        uint2 depthPx  = (depthDim.x == renderDim.x && depthDim.y == renderDim.y)
                             ? clampedRenderPx
                             : min(uint2(normUv * float2(depthDim)), depthDim - 1u);
        float rawD = g_DepthTex.Load(int3(g_DepthBase + depthPx, 0)).x;
        if (!isnan(rawD) && !isinf(rawD))
        {
            bool depthInverted = (g_Flags & 2u) != 0u; // UE5 預設為 Reverse-Z (近=大, 遠=0)
            float rawInvZ = depthInverted ? max(rawD, 0.0f) : max(1.0f - rawD, 0.0f);
            // UEVR Reverse-Z 近平面極小 (proj[3][2]=0.001, world_to_meters=100 -> 1m 處 rawInvZ=1e-5)，
            // 直接存入 float16 會落入 subnormal (<6.1e-5) 或下溢為 0。
            // 以 1m 參考逆深度 g_DepthRef 做雙曲正規化映射：invZ = rawInvZ / (rawInvZ + g_DepthRef) in [0, 1]
            float depthRef = max(g_DepthRef, 1e-9f);
            invZ = (rawInvZ > 0.0f) ? (rawInvZ / (rawInvZ + depthRef)) : 0.0f;
        }
    }

    s_Mv[gi]    = (inBounds && isFiniteMv) ? uvMv : float2(0.0f, 0.0f);
    s_InvZ[gi]  = (inBounds && isFiniteMv) ? invZ : -1.0f;
    s_Valid[gi] = (inBounds && isFiniteMv) ? 1u : 0u;

    bool dumpColor = (g_Flags & 8u) != 0u;
    if (dumpColor)
    {
        float3 col = g_ColorTex.Load(int3(g_ColorBase + clampedRenderPx, 0)).rgb;
        if (any(isnan(col)) || any(isinf(col)))
        {
            col = float3(0.0f, 0.0f, 0.0f);
        }
        col = max(col, float3(0.0f, 0.0f, 0.0f));
        // 軟壓縮保護 HDR 線性色彩，避免高光溢位同時保留明暗梯度供反向 warp PSNR 比對
        col = col / (1.0f + max(col.r, max(col.g, col.b)) * 0.25f);
        s_Color[gi] = inBounds ? saturate(col) : float3(0.0f, 0.0f, 0.0f);
    }

    GroupMemoryBarrierWithGroupSync();

    if (gi != 0u)
    {
        return;
    }

    if (gid.x >= g_EyeGridDim.x || gid.y >= g_EyeGridDim.y)
    {
        return;
    }

    // 步驟 1：找出 8x8 區塊內離相機最近的前景逆深度 zMax
    float zMax = -1.0f;
    uint totalValid = 0u;
    [unroll]
    for (uint i = 0u; i < 64u; ++i)
    {
        if (s_Valid[i] != 0u)
        {
            zMax = max(zMax, s_InvZ[i]);
            totalValid += 1u;
        }
    }

    uint2 outCoord = uint2(gid.x + g_EyeIndex * g_EyeGridDim.x, gid.y);

    if (totalValid == 0u)
    {
        g_OutMv8[outCoord] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    else
    {
        // 步驟 2：前景優先池化（Closest-Depth Pooling，設計 3.2 節）
        // 使用相對於 zMax 的比例門檻，只對逆深度接近 zMax 的前景像素取平均 MV，避免被靜止背景與天空(0)稀釋
        float relEps = clamp(g_DepthEpsilon, 0.005f, 0.5f);
        float eps = max(0.002f, zMax * relEps);
        float threshold = (hasDepth && zMax > 0.0f) ? max(0.0f, zMax - eps) : -1.0f;

        float2 sumMv = float2(0.0f, 0.0f);
        float  sumZ  = 0.0f;
        uint   fgCount = 0u;

        for (uint j = 0u; j < 64u; ++j)
        {
            if (s_Valid[j] != 0u && s_InvZ[j] >= threshold)
            {
                sumMv += s_Mv[j];
                sumZ  += s_InvZ[j];
                fgCount += 1u;
            }
        }

        float invCount = (fgCount > 0u) ? (1.0f / float(fgCount)) : 0.0f;
        float2 avgMv = sumMv * invCount;
        float  avgZ  = sumZ * invCount;

        // 步驟 3：計算前景像素的 MV 一致性作為有效度 (w in (0, 1])
        float varSum = 0.0f;
        for (uint k = 0u; k < 64u; ++k)
        {
            if (s_Valid[k] != 0u && s_InvZ[k] >= threshold)
            {
                float2 diff = s_Mv[k] - avgMv;
                varSum += dot(diff, diff);
            }
        }
        float avgVar = varSum * invCount;
        // 變異度為 0 時有效度 = 1.0；變異度越大時平滑下降，保留 >= 0.05 表示資料有效
        float validity = clamp(1.0f / (1.0f + avgVar * 256.0f), 0.05f, 1.0f);

        g_OutMv8[outCoord] = float4(avgMv.x, avgMv.y, saturate(avgZ), validity);
    }

    if (dumpColor)
    {
        float3 sumCol = float3(0.0f, 0.0f, 0.0f);
        uint colCount = 0u;
        for (uint c = 0u; c < 64u; ++c)
        {
            uint2 cPx = gid.xy * 8u + uint2(c & 7u, c >> 3u);
            if (cPx.x < renderDim.x && cPx.y < renderDim.y)
            {
                sumCol += s_Color[c];
                colCount += 1u;
            }
        }
        float3 avgCol = (colCount > 0u) ? (sumCol / float(colCount)) : float3(0.0f, 0.0f, 0.0f);
        g_OutColor8[outCoord] = float4(saturate(avgCol), 1.0f);
    }
}

// ============================================================================
// cs_downsample_raw_mv_4x4
// 除錯傾印專用（當 viple_mv_dump.txt 包含 "raw"，如 "12 raw" 時啟用）：
// 對每眼 MV 原始貼圖（自 g_MvBase 起算的 g_MvValidDim 區域）直接做 4x4 算術平均，
// 不做前景深度池化、保留原始貼圖 RG float 數值（寫入 u2: g_OutRawMv4, RG32F），
// 左眼寫入左半 [0, g_RawMv4GridDim.x)，右眼寫入右半 [g_RawMv4GridDim.x, 2*g_RawMv4GridDim.x)。
// ============================================================================
[numthreads(8, 8, 1)]
void cs_downsample_raw_mv_4x4(uint3 dtid : SV_DispatchThreadID)
{
    if (dtid.x >= g_RawMv4GridDim.x || dtid.y >= g_RawMv4GridDim.y)
    {
        return;
    }

    uint2 mvDim = max(g_MvValidDim, uint2(1u, 1u));
    uint2 basePx = dtid.xy * 4u;
    float2 sumRaw = float2(0.0f, 0.0f);
    uint validCount = 0u;

    [unroll]
    for (uint dy = 0u; dy < 4u; ++dy)
    {
        [unroll]
        for (uint dx = 0u; dx < 4u; ++dx)
        {
            uint2 px = basePx + uint2(dx, dy);
            if (px.x < mvDim.x && px.y < mvDim.y)
            {
                float2 v = g_MvTex.Load(int3(g_MvBase + px, 0)).xy;
                if (!any(isnan(v)) && !any(isinf(v)))
                {
                    sumRaw += v;
                    validCount += 1u;
                }
            }
        }
    }

    float2 avgRaw = (validCount > 0u) ? (sumRaw / float(validCount)) : float2(0.0f, 0.0f);
    uint2 outCoord = uint2(dtid.x + g_EyeIndex * g_RawMv4GridDim.x, dtid.y);
    g_OutRawMv4[outCoord] = avgRaw;
}
