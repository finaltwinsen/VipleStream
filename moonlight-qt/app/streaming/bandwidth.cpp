#include "bandwidth.h"

using namespace std::chrono;

BandwidthTracker::BandwidthTracker(uint32_t windowSeconds, uint32_t bucketIntervalMs)
  : windowSeconds(seconds(windowSeconds)),
    // §REVIEW-1103：原本在建構子本體改參數 bucketIntervalMs（成員是 const、早已在初始化列表賦值），防護無效
    bucketIntervalMs(bucketIntervalMs > 0 ? (int)bucketIntervalMs : 250)
{
    bucketCount = (windowSeconds * 1000) / this->bucketIntervalMs;
    if (bucketCount == 0) {
        bucketCount = 1;  // §REVIEW-1103：視窗 0 秒時避免後面 % bucketCount 除以零
    }
    buckets.resize(bucketCount);
}

// Add bytes recorded at the current time.
void BandwidthTracker::AddBytes(size_t bytes) {
    std::lock_guard<std::mutex> lock(mtx);
    auto now = steady_clock::now();
    updateBucket(bytes, now);
}

// We don't want to average the entire window used for peak,
// so average only the newest 25% of complete buckets
double BandwidthTracker::GetAverageMbps() {
    std::lock_guard<std::mutex> lock(mtx);
    auto now = steady_clock::now();
    auto ms = duration_cast<milliseconds>(now.time_since_epoch());
    int currentIndex = (ms.count() / bucketIntervalMs) % bucketCount;
    int maxBuckets = (int)bucketCount / 4;
    if (maxBuckets < 1) {
        maxBuckets = 1;
    }
    size_t totalBytes = 0;
    int counted = 0;

    // Sum bytes from 25% most recent buckets as long as they are completed
    // §REVIEW-1103：原本 i 從 0 開始（i=0 是進行中的 bucket 或上一輪的舊資料）、分母用「現在－最舊 bucket 起點」
    // （含進行中那格已過的時間）→ 平均值週期性低估、還少算一格。改成只算 i=1..maxBuckets 已完成的格子，
    // 用起點比對排除上一輪的舊資料，分母＝計入格數 × 格長。
    const auto currentStartMs = ms.count() - (ms.count() % bucketIntervalMs);
    for (int i = 1; i <= maxBuckets; i++) {
        int idx = (currentIndex - i + (int)bucketCount * 2) % (int)bucketCount;
        const Bucket &bucket = buckets[idx];
        const auto expectedStart = steady_clock::time_point(milliseconds(currentStartMs - (long long)i * bucketIntervalMs));
        if (bucket.start == expectedStart) {
            totalBytes += bucket.bytes;
        }
        // 沒有資料的格子也算時間（那段時間確實沒有流量）
        counted++;
    }

    const double elapsed = counted * (bucketIntervalMs / 1000.0);
    if (elapsed <= 0.0) {
        return 0.0;
    }

    return totalBytes * 8.0 / 1000000.0 / elapsed;
}

double BandwidthTracker::GetPeakMbps() {
    std::lock_guard<std::mutex> lock(mtx);
    auto now = steady_clock::now();
    double peak = 0.0;
    for (const auto& bucket : buckets) {
        if (isValid(bucket, now)) {
            double throughput = getBucketMbps(bucket);
            if (throughput > peak) {
                peak = throughput;
            }
        }
    }
    return peak;
}

unsigned int BandwidthTracker::GetWindowSeconds() {
    return (unsigned int)windowSeconds.count();
}

/// private methods

inline double BandwidthTracker::getBucketMbps(const Bucket &bucket) const {
    return bucket.bytes * 8.0 / 1000000.0 / (bucketIntervalMs / 1000.0);
}

// Check if a bucket's data is still valid (within the window)
inline bool BandwidthTracker::isValid(const Bucket &bucket, steady_clock::time_point now) const {
    return (now - bucket.start) <= windowSeconds;
}

void BandwidthTracker::updateBucket(size_t bytes, steady_clock::time_point now) {
    auto ms          = duration_cast<milliseconds>(now.time_since_epoch()).count();
    int bucketIndex  = (ms / bucketIntervalMs) % bucketCount;
    auto aligned_ms  = ms - (ms % bucketIntervalMs);
    auto bucketStart = steady_clock::time_point(milliseconds(aligned_ms));

    Bucket &bucket = buckets[bucketIndex];

    if (now - bucket.start > windowSeconds) {
        bucket.bytes = 0;
        bucket.start = bucketStart;
    }

    if (bucket.start != bucketStart) {
        bucket.bytes = bytes;
        bucket.start = bucketStart;
    }
    else {
        bucket.bytes += bytes;
    }
}
