#include "mappingmanager.h"
#include "path.h"

#include <QDir>

#include "SDL_compat.h"

#define SER_GAMEPADMAPPING "gcmapping"

#define SER_GUID "guid"
#define SER_MAPPING "mapping"

MappingFetcher* MappingManager::s_MappingFetcher;
bool MappingManager::s_SkipFetchIfCached = false;
bool MappingManager::s_FetchDecided = false;

void MappingManager::setSkipFetchIfCached(bool skip)
{
    s_SkipFetchIfCached = skip;
}

MappingManager::MappingManager()
{
    QSettings settings;

    // Load updated mappings from the Internet once per Moonlight launch
    // §PERF-1113 A31：每個行程只決定一次。VR 串流的子行程（setSkipFetchIfCached）手上有快取檔就不查——只看檔案在不在、
    // 有沒有內容，不看它多舊：伺服器回「沒有更新」時檔案的時間不會變，用時間判斷會變成每次都查
    if (!s_FetchDecided) {
        s_FetchDecided = true;

        bool fetch = true;
        if (s_SkipFetchIfCached) {
            const QFileInfo cached = Path::getCacheFileInfo("gamecontrollerdb.txt");
            fetch = !(cached.exists() && cached.size() > 0);
        }
        if (fetch) {
            s_MappingFetcher = new MappingFetcher();
            s_MappingFetcher->start();
        }
    }

    // First load existing saved mappings. This ensures the user's
    // hints can always override the old data.
    int mappingCount = settings.beginReadArray(SER_GAMEPADMAPPING);
    for (int i = 0; i < mappingCount; i++) {
        settings.setArrayIndex(i);

        SdlGamepadMapping mapping(settings.value(SER_GUID).toString(), settings.value(SER_MAPPING).toString());
        addMapping(mapping);
    }
    settings.endArray();

    // §PERF-1113 A31：記下設定裡原本的樣子，結尾只在真的有變時才寫回。原本每建一次就整組刪掉重寫（一個行程裡
    // 兩三次），Linux 上是整份設定檔重寫再 fsync。個數對不上＝設定裡有重複的 GUID，照舊寫一次把它整理掉
    const QMap<QString, SdlGamepadMapping> savedMappings = m_Mappings;
    const bool savedIsNormalized = (mappingCount == m_Mappings.size());

    // Finally load mappings from SDL_HINT_GAMECONTROLLERCONFIG
    QStringList sdlMappings =
            QString::fromLocal8Bit(SDL_GetHint(SDL_HINT_GAMECONTROLLERCONFIG))
        #if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
            .split('\n', Qt::SkipEmptyParts);
        #else
            .split('\n', QString::SkipEmptyParts);
        #endif
    for (const QString& sdlMapping : std::as_const(sdlMappings)) {
        SdlGamepadMapping mapping(sdlMapping);
        addMapping(mapping);
    }

    // Save the updated mappings to settings
    if (!savedIsNormalized || m_Mappings != savedMappings) {
        save();
    }
}

void MappingManager::save()
{
    QSettings settings;

    settings.remove(SER_GAMEPADMAPPING);
    settings.beginWriteArray(SER_GAMEPADMAPPING);
    QList<SdlGamepadMapping> mappings = m_Mappings.values();
    for (int i = 0; i < mappings.count(); i++) {
        settings.setArrayIndex(i);

        settings.setValue(SER_GUID, mappings[i].getGuid());
        settings.setValue(SER_MAPPING, mappings[i].getMapping());
    }
    settings.endArray();
}

void MappingManager::applyMappings()
{
    QByteArray mappingData = Path::readDataFile("gamecontrollerdb.txt");
    if (!mappingData.isEmpty()) {
        int newMappings = SDL_GameControllerAddMappingsFromRW(
                    SDL_RWFromConstMem(mappingData.constData(), mappingData.size()), 1);

        if (newMappings > 0) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Loaded %d new gamepad mappings",
                        newMappings);
        }
        else if (newMappings < 0) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Error loading gamepad mappings: %s",
                         SDL_GetError());
            // §REVIEW-1104：只有載入失敗才刪 cache；回 0 是沒有新 GUID，不是檔案壞掉
            Path::deleteCacheFile("gamecontrollerdb.txt");
        }
        else if (newMappings == 0) {
            // §REVIEW-1104：回 0 是沒有新 GUID（已載入過），不是檔案壞掉
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "No new gamepad mappings in gamecontrollerdb.txt");
        }
    }
    else {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "Unable to load gamepad mapping file");
    }

    QList<SdlGamepadMapping> mappings = m_Mappings.values();
    for (const SdlGamepadMapping& mapping : std::as_const(mappings)) {
        QString sdlMappingString = mapping.getSdlMappingString();
        int ret = SDL_GameControllerAddMapping(qPrintable(sdlMappingString));
        if (ret < 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Unable to add mapping: %s",
                        qPrintable(sdlMappingString));
        }
        else if (ret == 1) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Loaded saved user mapping: %s",
                        qPrintable(sdlMappingString));
        }
    }
}

void MappingManager::addMapping(QString mappingString)
{
    SdlGamepadMapping mapping(mappingString);
    addMapping(mapping);
}

void MappingManager::addMapping(SdlGamepadMapping& mapping)
{
    m_Mappings[mapping.getGuid()] = mapping;
}
