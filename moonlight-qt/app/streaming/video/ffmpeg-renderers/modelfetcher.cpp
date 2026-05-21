// VipleStream §G.4 — on-demand FRUC ML model download.  See modelfetcher.h
// for the design rationale.

#include "modelfetcher.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QTimer>

#include <SDL_log.h>

namespace {

// 60-second timeout.  39 MB * 8 bits / 60 s ≈ 5.2 Mbit/s minimum which is
// trivially achievable on any modern broadband; for slower users the
// timeout fires, retry kicks in, and they fall back to inline graph if
// retry also fails.  We don't want to hang stream init forever.
constexpr int FETCH_TIMEOUT_MS = 60000;

} // namespace

const ModelFetcher::ModelSpec* ModelFetcher::lookupSpec(const QString& filename)
{
    // SHA-256 hashes pinned to permanent ONNX asset tags.
    // - fruc.onnx and fruc_ifrnet_s.onnx still come from the v1.3.310 release.
    // - fruc_fp16.onnx was re-converted with onnxconverter_common 1.16, which
    //   drops 72 nodes (-13%), most notably 13 GridSample, 56 Slice, 57 Add
    //   and 52 Div ops.  Same byte-size on disk but much shallower DML
    //   command list.  Re-rolled under tag `assets-onnx-v2`.
    // Mismatched hashes cause one retry then fail to inline graph, so when a
    // future release re-rolls a model: bump the URL tag + paste new hashes
    // here in the same commit that uploads the asset.
    static const ModelSpec MODEL_TABLE[] = {
        {
            "fruc.onnx",
            "610b5de57cdcfbcce9914c23e60a1cd357779a6f9582a1bcfcb035f8eb38509b",
            22538831,
            "https://github.com/finaltwinsen/VipleStream/releases/download/v1.3.310/fruc.onnx"
        },
        {
            "fruc_fp16.onnx",
            "50180cfc4aca7853daf8c44ace5fec4287f8c71c9dc2bc87551d8296965f182d",
            11396749,
            "https://github.com/finaltwinsen/VipleStream/releases/download/assets-onnx-v2/fruc_fp16.onnx"
        },
        {
            "fruc_ifrnet_s.onnx",
            "bb24196608b7eea22f464308559dafeb7ddbe71a6b8b736b88ea50f956474c83",
            5783526,
            "https://github.com/finaltwinsen/VipleStream/releases/download/v1.3.310/fruc_ifrnet_s.onnx"
        },
        // §SLIM 2026-05-21 — RIFE 4.25-lite NCNN model.  flownet.bin
        // (11 MB) is the big half of the bundle; flownet.param (36 KB)
        // is small enough to keep shipping inside the zip.  Pulled from
        // the repo `raw` URL because the file is already committed to
        // main — no separate release-asset upload required.  Cached
        // at %LOCALAPPDATA%\VipleStream\fruc_models\rife-v4.25-lite\
        // flownet.bin; first NCNN/Native-RIFE backend init sees a
        // one-time ~3-5 s pause on broadband.
        {
            "rife-v4.25-lite/flownet.bin",
            "350a15e464bea5ad378e06c0fb43996e90a0d35653d5a6ef6bc980d832538fb7",
            11276252,
            "https://github.com/finaltwinsen/VipleStream/raw/main/moonlight-qt/app/rife_models/rife-v4.25-lite/flownet.bin"
        },
    };
    for (const auto& spec : MODEL_TABLE) {
        if (filename == QLatin1String(spec.filename)) {
            return &spec;
        }
    }
    return nullptr;
}

QString ModelFetcher::cacheDirPath()
{
    // %LOCALAPPDATA%\VipleStream\fruc_models on Windows; XDG-cache on
    // Linux; ~/Library/Caches on macOS.  AppLocalDataLocation maps to
    // %LOCALAPPDATA% on Windows (vs. CacheLocation which lives further
    // below %LOCALAPPDATA%\<org>\<app>\cache and is meant for ephemeral
    // data).  Models are downloaded once and reused across launches —
    // AppLocalDataLocation is the right bucket.
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (base.isEmpty()) {
        // Fallback: Qt default cache.  Should never trip on a normal
        // Windows install (AppLocalDataLocation always resolves).
        base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    }
    return QDir(base).absoluteFilePath(QStringLiteral("fruc_models"));
}

bool ModelFetcher::verifyOnDisk(const QString& path, const ModelSpec& spec)
{
    QFile f(path);
    if (!f.exists()) {
        return false;
    }
    if (f.size() != spec.sizeBytes) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-MODELFETCH] %s size mismatch: got %lld expected %lld",
                    spec.filename,
                    (long long)f.size(), (long long)spec.sizeBytes);
        return false;
    }
    if (!f.open(QIODevice::ReadOnly)) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-MODELFETCH] %s open-for-hash failed: %s",
                    spec.filename, qPrintable(f.errorString()));
        return false;
    }
    QCryptographicHash hasher(QCryptographicHash::Sha256);
    hasher.addData(&f);
    f.close();
    QString hex = QString::fromLatin1(hasher.result().toHex());
    if (hex.toLower() != QString::fromLatin1(spec.sha256).toLower()) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-MODELFETCH] %s sha256 mismatch: got %s expected %s",
                    spec.filename, qPrintable(hex), spec.sha256);
        return false;
    }
    return true;
}

bool ModelFetcher::downloadOnce(const ModelSpec& spec, const QString& destPath)
{
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-MODELFETCH] downloading %s (%lld bytes) from %s",
                spec.filename, (long long)spec.sizeBytes, spec.url);

    QString partialPath = destPath + QStringLiteral(".partial");

    // Clean any prior partial — better to start fresh than try to resume
    // (HTTP range requests against GitHub release assets are supported,
    // but adds complexity for marginal benefit on 5-22 MB files).
    QFile::remove(partialPath);

    QNetworkAccessManager nam;
    QNetworkRequest req((QUrl(QString::fromLatin1(spec.url))));
    // GitHub serves a 302 to the actual S3 download URL.
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    // Identify ourselves so 403s look less mysterious in repo traffic logs.
    req.setRawHeader("User-Agent", "VipleStream-ModelFetcher/1.0");

    QNetworkReply* reply = nam.get(req);
    QFile out(partialPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-MODELFETCH] cannot create %s: %s",
                     qPrintable(partialPath), qPrintable(out.errorString()));
        reply->deleteLater();
        return false;
    }

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);

    QObject::connect(reply, &QNetworkReply::readyRead, [&]() {
        // Stream-write incoming bytes so we don't buffer 22 MB in RAM.
        out.write(reply->readAll());
    });

    qint64 lastLogged = 0;
    QObject::connect(reply, &QNetworkReply::downloadProgress,
                     [&](qint64 received, qint64 total) {
        // Throttle progress log to once per ~2 MB so log isn't spammed.
        if (received - lastLogged >= 2 * 1024 * 1024 || (total > 0 && received == total)) {
            lastLogged = received;
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "[VIPLE-MODELFETCH] %s: %.1f / %.1f MB",
                        spec.filename,
                        received / 1024.0 / 1024.0,
                        total > 0 ? total / 1024.0 / 1024.0
                                  : spec.sizeBytes / 1024.0 / 1024.0);
        }
    });

    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, [&]() {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-MODELFETCH] %s: timeout after %d ms",
                    spec.filename, FETCH_TIMEOUT_MS);
        reply->abort();
    });
    timeout.start(FETCH_TIMEOUT_MS);

    loop.exec();

    // Drain any final bytes the readyRead handler missed.
    out.write(reply->readAll());
    out.close();

    bool ok = (reply->error() == QNetworkReply::NoError);
    if (!ok) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-MODELFETCH] %s download error: %s",
                    spec.filename, qPrintable(reply->errorString()));
    }
    reply->deleteLater();

    if (!ok) {
        QFile::remove(partialPath);
        return false;
    }

    // Verify the .partial *before* renaming to final destPath.
    if (!verifyOnDisk(partialPath, spec)) {
        QFile::remove(partialPath);
        return false;
    }

    // Atomic-ish rename.  On Windows QFile::rename refuses to overwrite,
    // so remove the destination first.  Race-window between remove() and
    // rename() is irrelevant because we only one fetch per model per
    // process.
    if (QFile::exists(destPath)) {
        QFile::remove(destPath);
    }
    if (!QFile::rename(partialPath, destPath)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-MODELFETCH] %s rename .partial → final failed",
                     spec.filename);
        QFile::remove(partialPath);
        return false;
    }

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-MODELFETCH] %s downloaded + verified → %s",
                spec.filename, qPrintable(destPath));
    return true;
}

QString ModelFetcher::ensureModelPath(const QString& filename)
{
    const ModelSpec* spec = lookupSpec(filename);
    if (!spec) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "[VIPLE-MODELFETCH] unknown model '%s' — no spec table entry",
                    qPrintable(filename));
        return QString();
    }

    // Ensure cache dir exists.
    QString dir = cacheDirPath();
    if (!QDir().mkpath(dir)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-MODELFETCH] cannot create cache dir: %s",
                     qPrintable(dir));
        return QString();
    }
    QString destPath = QDir(dir).absoluteFilePath(filename);
    // §SLIM 2026-05-21 — spec filenames may carry a subdir prefix
    // (e.g. "rife-v4.25-lite/flownet.bin").  Ensure that subdir exists
    // before downloadOnce tries to open the .partial file in it.
    QString destParent = QFileInfo(destPath).absolutePath();
    if (destParent != dir && !QDir().mkpath(destParent)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "[VIPLE-MODELFETCH] cannot create model subdir: %s",
                     qPrintable(destParent));
        return QString();
    }

    // Cache hit: file present + verified.
    if (verifyOnDisk(destPath, *spec)) {
        return destPath;
    }

    // Either missing, wrong size, or hash mismatch.  Remove any stale
    // copy and download (one retry on failure).
    QFile::remove(destPath);

    if (downloadOnce(*spec, destPath)) {
        return destPath;
    }
    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-MODELFETCH] %s first download failed, retrying once",
                spec->filename);
    if (downloadOnce(*spec, destPath)) {
        return destPath;
    }

    SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                "[VIPLE-MODELFETCH] %s unavailable after 2 attempts; "
                "DirectML backend will fall back to inline blend graph",
                spec->filename);
    return QString();
}
