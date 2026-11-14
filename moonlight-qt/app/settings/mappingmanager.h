#pragma once

#include "mappingfetcher.h"

#include <QSettings>

class SdlGamepadMapping
{
public:
    SdlGamepadMapping() {}

    SdlGamepadMapping(QString string)
    {
        QStringList mapping = string.split(",");
        if (!mapping.isEmpty()) {
            m_Guid = mapping[0];

            string.remove(0, m_Guid.length() + 1);
            m_Mapping = string;
        }
    }

    SdlGamepadMapping(QString guid, QString mapping)
        : m_Guid(guid),
          m_Mapping(mapping)
    {

    }

    bool operator==(const SdlGamepadMapping& other) const
    {
        return m_Guid == other.m_Guid && m_Mapping == other.m_Mapping;
    }

    QString getGuid() const
    {
        return m_Guid;
    }

    QString getMapping() const
    {
        return m_Mapping;
    }

    QString getSdlMappingString() const
    {
        if (m_Guid.isEmpty() || m_Mapping.isEmpty()) {
            return "";
        }
        else {
            return m_Guid + "," + m_Mapping;
        }
    }

private:
    QString m_Guid;
    QString m_Mapping;
};

class MappingManager
{
public:
    MappingManager();

    void addMapping(QString gamepadString);

    void addMapping(SdlGamepadMapping& gamepadMapping);

    void applyMappings();

    void save();

    // §PERF-1113 A31：設了之後，手上已經有對應表的快取檔時這個行程不再上網查新版（沒有快取檔照舊去抓）。
    // 給 VR 串流的子行程用：生出它的介面行程啟動時已經查過。要在第一個 MappingManager 建立之前設（GUI 執行緒）
    static void setSkipFetchIfCached(bool skip);

private:
    QMap<QString, SdlGamepadMapping> m_Mappings;

    static MappingFetcher* s_MappingFetcher;
    static bool s_SkipFetchIfCached;
    static bool s_FetchDecided;
};

