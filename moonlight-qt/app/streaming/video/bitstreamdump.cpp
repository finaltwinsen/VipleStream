// VipleStream §SF-PROBE（M2a）— `stream --dump-bitstream <path>` 的實作。
// 介面、寫出格式與「絕不進 StreamingPreferences」的理由見 bitstreamdump.h。
//
// 執行緒：writeAccessUnit() 只在 decoder 執行緒呼叫；close() 在 decoder 解構時（主執行緒，
// decoder 執行緒已 join）呼叫；setPath() 在 session 開始前呼叫。熱路徑只讀 s_Enabled
// （relaxed），其餘狀態一律在 s_Lock 下存取——鎖只在開啟 dump 時才會碰到。

#include "bitstreamdump.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <Limelight.h>
#include <SDL.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#ifdef Q_OS_WIN32
#include <share.h>   // _SH_DENYNO
#endif

namespace {

using Clock = std::chrono::steady_clock;

// 熱路徑唯一會讀的狀態。setPath() 發生在 decoder 執行緒建立之前，執行緒建立本身就
// 保證可見性，所以 relaxed 即可。
std::atomic<bool> s_Enabled{false};

std::mutex s_Lock;
QString s_Base;            // 絕對路徑，已去掉使用者給的已知副檔名
QString s_UserExt;         // 使用者給的已知副檔名（小寫、不含點）；沒有就是空字串
int s_NextIndex = 0;       // 0 → <base>.<ext>；n ≥ 1 → <base>-<n>.<ext>
bool s_ExtWarned = false;

FILE* s_File = nullptr;
QString s_CurPath;
int s_CurFormat = 0;
int s_CurWidth = 0;
int s_CurHeight = 0;
unsigned long long s_Frames = 0;
unsigned long long s_Bytes = 0;
Clock::time_point s_OpenTime;
Clock::time_point s_LastProgress;

constexpr int k_IvfHeaderSize = 32;
constexpr int k_IvfFrameHeaderSize = 12;
constexpr auto k_ProgressInterval = std::chrono::seconds(10);

const char* codecFamily(int videoFormat)
{
    if (videoFormat & VIDEO_FORMAT_MASK_H264) {
        return "h264";
    }
    else if (videoFormat & VIDEO_FORMAT_MASK_H265) {
        return "hevc";
    }
    else if (videoFormat & VIDEO_FORMAT_MASK_AV1) {
        return "av1";
    }
    return nullptr;
}

// 預設副檔名與 decode-bench 依副檔名推 codec 的規則一致
QString defaultExt(int videoFormat)
{
    if (videoFormat & VIDEO_FORMAT_MASK_H264) {
        return QStringLiteral("h264");
    }
    else if (videoFormat & VIDEO_FORMAT_MASK_H265) {
        return QStringLiteral("hevc");
    }
    return QStringLiteral("ivf");
}

bool isKnownExt(const QString& ext)
{
    return ext == QLatin1String("h264") || ext == QLatin1String("264") ||
           ext == QLatin1String("h265") || ext == QLatin1String("265") ||
           ext == QLatin1String("hevc") || ext == QLatin1String("ivf");
}

bool extMatches(const QString& ext, int videoFormat)
{
    if (videoFormat & VIDEO_FORMAT_MASK_H264) {
        return ext == QLatin1String("h264") || ext == QLatin1String("264");
    }
    else if (videoFormat & VIDEO_FORMAT_MASK_H265) {
        return ext == QLatin1String("h265") || ext == QLatin1String("265") || ext == QLatin1String("hevc");
    }
    return ext == QLatin1String("ivf");
}

QString pathFor(int index, const QString& ext)
{
    QString p = s_Base;
    if (index > 0) {
        p += QStringLiteral("-%1").arg(index);
    }
    return p + QLatin1Char('.') + ext;
}

void putLe16(uint8_t* p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

void putLe32(uint8_t* p, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        p[i] = (uint8_t)((v >> (8 * i)) & 0xFF);
    }
}

void putLe64(uint8_t* p, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)((v >> (8 * i)) & 0xFF);
    }
}

// qt_error_string 不觸發 MSVC 對 strerror 的 C4996；附上 errno 數字方便對照
QString errText(int err)
{
    return QStringLiteral("%1 (errno %2)").arg(qt_error_string(err)).arg(err);
}

bool writeAll(const void* data, size_t length)
{
    return fwrite(data, 1, length, s_File) == length;
}

void closeLocked(const char* reason)
{
    if (s_File == nullptr) {
        return;
    }

    fclose(s_File);
    s_File = nullptr;

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-BSDUMP] closed frames=%llu bytes=%llu path=%s reason=%s",
                s_Frames, s_Bytes,
                qUtf8Printable(QDir::toNativeSeparators(s_CurPath)),
                reason);

    s_CurPath.clear();
    s_CurFormat = s_CurWidth = s_CurHeight = 0;
    s_Frames = s_Bytes = 0;
}

// 開檔或寫檔失敗後整個關掉：不再佔熱路徑，也不會每幀刷錯誤訊息。
void disableLocked()
{
    s_Enabled.store(false, std::memory_order_relaxed);
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-BSDUMP] dump disabled for the rest of this process");
}

bool openLocked(int videoFormat, int width, int height)
{
    const char* family = codecFamily(videoFormat);
    if (family == nullptr) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-BSDUMP] unsupported videoFormat 0x%x",
                    videoFormat);
        return false;
    }

    QString ext = defaultExt(videoFormat);
    if (!s_UserExt.isEmpty()) {
        if (extMatches(s_UserExt, videoFormat)) {
            ext = s_UserExt;
        }
        else if (!s_ExtWarned) {
            // 副檔名和 codec 不符時改用正確的副檔名：decode-bench 是依副檔名推 codec 的
            s_ExtWarned = true;
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-BSDUMP] extension .%s does not match codec %s; writing .%s instead",
                        qUtf8Printable(s_UserExt), family, qUtf8Printable(ext));
        }
    }

    const QString path = pathFor(s_NextIndex++, ext);
    QDir().mkpath(QFileInfo(path).absolutePath());

#ifdef Q_OS_WIN32
    // _wfsopen＋_SH_DENYNO：錄製中也能讓別的行程讀檔（_wfopen 會觸發 C4996；_wfopen_s 則是獨佔開檔）
    FILE* f = _wfsopen(path.toStdWString().c_str(), L"wb", _SH_DENYNO);
#else
    FILE* f = fopen(QFile::encodeName(path).constData(), "wb");
#endif
    if (f == nullptr) {
        const int err = errno;
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-BSDUMP] cannot open %s: %s",
                     qUtf8Printable(QDir::toNativeSeparators(path)), qUtf8Printable(errText(err)));
        return false;
    }

    s_File = f;
    s_CurPath = path;
    s_CurFormat = videoFormat;
    s_CurWidth = width;
    s_CurHeight = height;
    s_Frames = 0;
    s_Bytes = 0;
    s_OpenTime = Clock::now();
    s_LastProgress = s_OpenTime;

    if (videoFormat & VIDEO_FORMAT_MASK_AV1) {
        // IVF 檔頭：時基 1/1000000，幀時間戳是開檔後的微秒數（保留到達節奏）。
        // 幀數欄固定寫 0：被 Stop-Process -Force 結束時無法回填，decode-bench 不依賴它。
        uint8_t hdr[k_IvfHeaderSize] = {};
        memcpy(hdr, "DKIF", 4);
        putLe16(hdr + 4, 0);                          // version
        putLe16(hdr + 6, k_IvfHeaderSize);            // header size
        memcpy(hdr + 8, "AV01", 4);                   // fourcc
        putLe16(hdr + 12, (uint16_t)width);
        putLe16(hdr + 14, (uint16_t)height);
        putLe32(hdr + 16, 1000000);                   // time base denominator
        putLe32(hdr + 20, 1);                         // time base numerator
        putLe32(hdr + 24, 0);                         // frame count（見上）
        if (!writeAll(hdr, sizeof(hdr)) || fflush(s_File) != 0) {
            const int err = errno;
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "[VIPLE-BSDUMP] cannot write IVF header to %s: %s",
                         qUtf8Printable(QDir::toNativeSeparators(path)), qUtf8Printable(errText(err)));
            closeLocked("write-error");
            return false;
        }
        s_Bytes = sizeof(hdr);
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-BSDUMP] open %s codec=%s(0x%x) %dx%d",
                qUtf8Printable(QDir::toNativeSeparators(path)),
                family, videoFormat, width, height);
    return true;
}

} // namespace

namespace BitstreamDump {

void setPath(const QString& path)
{
    std::lock_guard<std::mutex> lock(s_Lock);

    closeLocked("set-path");
    s_NextIndex = 0;
    s_ExtWarned = false;

    if (path.isEmpty()) {
        s_Base.clear();
        s_UserExt.clear();
        s_Enabled.store(false, std::memory_order_relaxed);
        return;
    }

    const QFileInfo fi(path);
    if (fi.isDir()) {
        // 給的是既有目錄：在裡面產生帶時間的檔名，副檔名開檔時依 codec 補
        s_Base = QDir(fi.absoluteFilePath()).filePath(
            QStringLiteral("viple-dump-%1").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"))));
        s_UserExt.clear();
    }
    else {
        const QString abs = fi.absoluteFilePath();
        const QString ext = fi.suffix().toLower();
        if (!ext.isEmpty() && isKnownExt(ext)) {
            s_UserExt = ext;
            s_Base = abs.left(abs.size() - ext.size() - 1);
        }
        else {
            s_UserExt.clear();
            s_Base = abs;
        }
    }

    s_Enabled.store(true, std::memory_order_relaxed);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-BSDUMP] armed base=%s ext=%s (dev-only, not persisted)",
                qUtf8Printable(QDir::toNativeSeparators(s_Base)),
                s_UserExt.isEmpty() ? "auto" : qUtf8Printable(s_UserExt));
}

bool isEnabled()
{
    return s_Enabled.load(std::memory_order_relaxed);
}

void writeAccessUnit(const uint8_t* data, int length, int videoFormat, int width, int height)
{
    if (data == nullptr || length <= 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(s_Lock);

    if (!s_Enabled.load(std::memory_order_relaxed) || s_Base.isEmpty()) {
        return;
    }

    // decoder 重建後格式或解析度變了：關掉舊檔，另開 <base>-<n>
    if (s_File != nullptr &&
            (videoFormat != s_CurFormat || width != s_CurWidth || height != s_CurHeight)) {
        closeLocked("format-change");
    }

    if (s_File == nullptr && !openLocked(videoFormat, width, height)) {
        disableLocked();
        return;
    }

    const Clock::time_point now = Clock::now();
    bool ok = true;

    if (s_CurFormat & VIDEO_FORMAT_MASK_AV1) {
        uint8_t frameHdr[k_IvfFrameHeaderSize];
        const long long tsUs =
            std::chrono::duration_cast<std::chrono::microseconds>(now - s_OpenTime).count();
        putLe32(frameHdr, (uint32_t)length);
        putLe64(frameHdr + 4, (uint64_t)tsUs);
        ok = writeAll(frameHdr, sizeof(frameHdr));
    }

    // 每個 AU 都 flush 到 OS：harness 常用 Stop-Process -Force 結束 client，
    // 留在 stdio 緩衝裡的尾端會掉；進了 OS 的資料不受行程被殺影響。
    ok = ok && writeAll(data, (size_t)length);
    ok = ok && fflush(s_File) == 0;
    if (!ok) {
        const int err = errno;
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-BSDUMP] write failed on %s: %s",
                     qUtf8Printable(QDir::toNativeSeparators(s_CurPath)), qUtf8Printable(errText(err)));
        closeLocked("write-error");
        disableLocked();
        return;
    }

    s_Frames++;
    s_Bytes += (unsigned long long)length;
    if (s_CurFormat & VIDEO_FORMAT_MASK_AV1) {
        s_Bytes += k_IvfFrameHeaderSize;
    }

    if (now - s_LastProgress >= k_ProgressInterval) {
        s_LastProgress = now;
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-BSDUMP] progress frames=%llu bytes=%llu",
                    s_Frames, s_Bytes);
    }
}

void close()
{
    std::lock_guard<std::mutex> lock(s_Lock);
    closeLocked("decoder-destroyed");
}

} // namespace BitstreamDump
