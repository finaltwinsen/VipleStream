// VipleStream §SF-PROBE（M2a）— `viplestream decode-bench`：`[VIPLE-V4L2] bench:`
//
// 用真實的串流 bitstream 量解碼器：PoC-3（延遲／吞吐）、PoC-3b（掉幀或破損後 decoder
// 怎麼恢復）、PoC-3c（capture buffer 數）、PoC-4（DRM_PRIME 幀映射成 Vulkan 的成本）。
// AVCodecContext 的設定完全照 app（ffmpeg.cpp），量到的就是串流時的行為。
//
// 輸入：H.264／HEVC 的 Annex-B（.h264 .264 .h265 .265 .hevc），AV1 的 IVF（.ivf）。
// 不連 libavformat（app 本來就不連），所以不吃 mp4。樣本用 `stream --dump-bitstream` 錄。
//
// 結束碼：0 完成；11 輸入檔讀不到或格式不對；13 指定的 decoder 開不起來；14 解碼中途失敗；
// 10 本建置沒有 FFmpeg。見 cli/probeutil.h。

#pragma once

#include <QList>
#include <QString>

struct DecodeBenchOptions {
    QString file;
    QString codec = "auto";      // auto（依副檔名）| h264 | hevc | av1
    QString decoder = "auto";    // auto | sw | <FFmpeg decoder 名，如 hevc_v4l2m2m> | hwaccel:<vaapi|vulkan|d3d11va|…>
    QString out = "drm_prime";   // drm_prime：get_format 選 DRM_PRIME（L2）；sw：系統記憶體幀（L3）
    int fps = 0;                 // >0：依 i/fps 排程送出，模擬串流；0：連發量吞吐
    int frames = 0;              // 0 = 整個檔案
    int loop = 1;
    int warmup = 0;              // 前 N 幀不計入統計
    int captureBuffers = -1;     // -1 = 照 app：4 + PACER_MAX_OUTSTANDING_FRAMES + 2
    int outputBuffers = -1;      // -1 = 照 app：2
    int hold = 0;                // 延後 N 幀才 unref，模擬 mailbox 與 GPU 持有
    QList<int> dropFrames;       // 這些幀號不送（PoC-3b）
    int dropEvery = 0;           // 每 N 幀丟一幀
    QList<int> corruptFrames;    // 這些幀號的 payload 中段翻轉位元組後才送
    bool flushOnError = false;   // 出錯後 avcodec_flush_buffers，看能否恢復
    bool compareSw = false;      // 另用 SW decoder 解同一檔（不丟幀）當參考，逐幀算 luma PSNR
    bool mapVulkan = false;      // headless libplacebo：每幀 pl_map_avframe_ex 計時（需 HAVE_LIBPLACEBO_VULKAN）
    bool verbose = false;        // av_log 升到 DEBUG
    QString jsonPath;            // 空 = ProbeUtil::defaultJsonPath("decode-bench")
};

int runDecodeBench(const DecodeBenchOptions& options);
