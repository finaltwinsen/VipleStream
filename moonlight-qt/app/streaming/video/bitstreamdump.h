// VipleStream §SF-PROBE（M2a）— `stream --dump-bitstream <path>`：錄下 decoder 實際收到的
// bitstream，當 decode-bench 的樣本（dev-only）。
//
// 設定只存在這個模組的行程內全域變數，**絕不進 StreamingPreferences**：CLI 覆寫會在
// session 開始時被 save() 寫回 QSettings（session.cpp），下次一般啟動就會一直錄。
//
// 寫出內容：ffmpeg.cpp 組好完整 AU（含 SPS 修正）之後、送進 decoder 之前的 bytes。
//   - H.264／HEVC：Annex-B 原樣串接（.h264／.hevc）。
//   - AV1：IVF（32 B 檔頭 + 每幀 12 B 幀頭）；檔頭的幀數固定寫 0（被強制結束時無法回填，
//     decode-bench 的解析器不依賴它）。
// 每個 AU 寫完就 flush：harness 常用 Stop-Process -Force 結束 client，緩衝裡的尾端不能掉。
// 格式或解析度改變（decoder 重建）時關檔，另開 `<base>-<n>.<ext>`。
//
// log：`[VIPLE-BSDUMP] open <path> codec=<…> <W>x<H>`、每 10 秒 `progress frames=… bytes=…`、
// `closed frames=… bytes=…`。

#pragma once

#include <QString>

#include <cstdint>

namespace BitstreamDump {

// 空字串 = 關閉。path 沒有已知副檔名時依 codec 自動補上。只在 session 開始前呼叫。
void setPath(const QString& path);

// 熱路徑用：一次 relaxed atomic load。vrFlags==0 的一般 session 在關閉時只多這一個判斷。
bool isEnabled();

// 寫一個完整 AU。videoFormat 是 Limelight.h 的 VIDEO_FORMAT_*；第一次呼叫時開檔。
// 只會在 decoder 執行緒呼叫。
void writeAccessUnit(const uint8_t* data, int length, int videoFormat, int width, int height);

// decoder 解構時呼叫：關檔並印 closed 行。沒開檔時不做事。
void close();

} // namespace BitstreamDump
