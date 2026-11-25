#pragma once

#include <QString>
#include <QStringList>

// §LOGDROP：規則與 moonlight-android 的 LogRedactor 逐條相同，給上傳前遮罩用。
class LogRedactor
{
public:
    static QString redact(const QString& text, const QStringList& extraSecrets);

private:
    LogRedactor() = delete;
};
