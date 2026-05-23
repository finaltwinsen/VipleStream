// VipleStream §G.4 — on-demand FRUC ML model download.
//
// Models (~39 MB total) used to be bundled in the release zip, but only
// users who pick the DirectML backend ever load them.  Most NVIDIA users
// run NvOF or Generic and never touch the .onnx files.  Strip them from
// the zip and pull on first DirectML use into a per-user cache dir:
//
//   %LOCALAPPDATA%\VipleStream\fruc_models\<filename>
//
// Source: GitHub release v1.3.310 attached assets.  SHA-256 verified
// after each download — corrupt / partial files are deleted and one
// retry is attempted.
//
// Synchronous-on-Qt-event-loop design: the caller blocks on a local
// QEventLoop while QNetworkReply runs.  This is fine because we only
// fetch from DirectMLFRUC::tryLoadOnnxModel, which already happens off
// the render hot path during stream init.  First DML stream launch sees
// a one-time 5–10 s pause (with status overlay text); subsequent
// launches hit the cache and are instant.

#pragma once

#include <QString>

#include <string>

// §SLIM 2026-05-21 — RIFE model dir resolver.  flownet.param ships in
// the release zip (36 KB); flownet.bin (11 MB) is fetched on demand
// via ModelFetcher and cached under %LOCALAPPDATA%\VipleStream\
// fruc_models\rife-v4.25-lite\.
//
// Returns a directory containing both files (so callers that pass a
// modelDir into the rife_native_vk executor or ncnn loader get a
// single coherent path).  Resolution order:
//   1. Data dir (legacy install / dev tree where both files bundled)
//      — use directly with no copy.
//   2. Hybrid: ModelFetcher fetches flownet.bin into the cache dir,
//      then we copy flownet.param from the data dir into the same
//      cache dir so the caller sees one self-contained model dir.
//   3. Failure (no data, no network) — return empty; caller logs and
//      the FRUC backend cascade picks the next available option
//      (Generic / NvOFFRUC / DML).
//
// §SLIM 2026-05-23 — promoted from ncnnfruc.cpp's anonymous namespace
// so VkFrucRenderer (vkfruc.cpp) can use the same lazy-fetch path
// instead of reading the deploy dir directly.  Both NCNN and Vulkan
// Native-RIFE backends now share this resolver.
QString ensureRifeModelDir(const std::string& modelDir);

class ModelFetcher
{
public:
    // Returns absolute path to a verified-on-disk copy of `filename`,
    // or empty string if download / verification failed and no cached
    // copy exists.  Always returns immediately if the file is already
    // cached and SHA-256 matches.
    //
    // `filename` must be one of the registered model names below; an
    // unknown filename returns "" (caller falls back to inline graph).
    static QString ensureModelPath(const QString& filename);

    // Path that ensureModelPath() would write to (or read from).  Useful
    // for diagnostics / logging without triggering a fetch.
    static QString cacheDirPath();

private:
    struct ModelSpec {
        const char* filename;
        const char* sha256;     // lowercase hex, 64 chars
        qint64      sizeBytes;  // for sanity check
        const char* url;        // GitHub release asset URL
    };
    static const ModelSpec* lookupSpec(const QString& filename);

    // Verify the existing on-disk file's SHA-256 matches `spec`.  Returns
    // true if the file matches.  False if missing, wrong size, or
    // hash-mismatched.
    static bool verifyOnDisk(const QString& path, const ModelSpec& spec);

    // Fetch the URL into `destPath` (atomic via .partial → rename).
    // Returns true on success (verified hash + size).  Caller is
    // responsible for retry / fallback decisions.
    static bool downloadOnce(const ModelSpec& spec, const QString& destPath);
};
