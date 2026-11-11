// §VR-LOWBATT（2026-11-04）：頭盔低電量提示。讀 Linux 的 /sys/class/power_supply（Flatpak 沙盒裡也讀得到），
// 其他平台不做事。電量 ≤ 15% 且沒在充電時進入低電量（顯示圖示＋嘟一聲）；回到 18% 以上或開始充電才解除，
// 避免在 15% 附近來回跳。
#pragma once

#include <QDir>
#include <QFile>
#include <QString>

#include <optional>

struct BatteryReading
{
    int capacity = -1;       // 0–100
    bool charging = false;   // Charging／Full（插著電）
    int currentMa = 0;       // 平均電流，負＝放電（mA）
    int voltageMv = 0;
};

class BatteryMonitor
{
public:
    static constexpr int kLowPct = 15;
    static constexpr int kClearPct = 18;

    // dev（--vr-fake-battery）：>= 0 時不讀硬體，固定回報這個電量、未充電
    // dev（--vr-low-battery-at）：>= 0 時門檻改成這個值，而且不看充電狀態（測試時頭盔常插著電）
    explicit BatteryMonitor(int fakePct = -1, int lowAt = -1) : m_FakePct(fakePct), m_LowAt(lowAt) {}

    std::optional<BatteryReading> read()
    {
        if (m_FakePct >= 0) {
            BatteryReading r;
            r.capacity = m_FakePct > 100 ? 100 : m_FakePct;
            return r;
        }
#ifdef Q_OS_LINUX
        if (m_Path.isEmpty()) {
            if (m_Probed) {
                return std::nullopt;
            }
            m_Probed = true;
            const QDir dir("/sys/class/power_supply");
            for (const QString& e : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                const QString p = dir.filePath(e);
                if (readText(p + "/type") == "Battery" && !readText(p + "/capacity").isEmpty()) {
                    m_Path = p;
                    break;
                }
            }
            if (m_Path.isEmpty()) {
                return std::nullopt;
            }
        }
        BatteryReading r;
        bool ok = false;
        r.capacity = readText(m_Path + "/capacity").toInt(&ok);
        if (!ok) {
            return std::nullopt;
        }
        const QString st = readText(m_Path + "/status");
        // Frame 插著電、充滿時是 "Not charging"：另外看充電器的 online
        // §REVIEW-1111：只有「Not charging」才參考充電器。原本只要充電器 online 就算在充電，接著供電不足的充電器、
        // 實際在放電（Discharging）時，低電量提示會被當成「正在充電」而不出現。
        r.charging = st == "Charging" || st == "Full" || (st == "Not charging" && chargerOnline());
        QString cur = readText(m_Path + "/current_avg");
        if (cur.isEmpty()) {
            cur = readText(m_Path + "/current_now");
        }
        r.currentMa = static_cast<int>(cur.toLongLong() / 1000);
        r.voltageMv = static_cast<int>(readText(m_Path + "/voltage_now").toLongLong() / 1000);
        return r;
#else
        return std::nullopt;
#endif
    }

    // 回傳這次是否「剛進入」低電量（要嘟一聲）；low() 是目前狀態
    bool update(const BatteryReading& r)
    {
        const bool was = m_Low;
        if (m_LowAt >= 0) {
            m_Low = r.capacity <= m_LowAt;
            return m_Low && !was;
        }
        if (r.charging || r.capacity >= kClearPct) {
            m_Low = false;
        }
        else if (r.capacity <= kLowPct) {
            m_Low = true;
        }
        return m_Low && !was;
    }

    bool low() const { return m_Low; }

private:
    static QString readText(const QString& path)
    {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            return QString();
        }
        return QString::fromLatin1(f.readAll()).trimmed();
    }

    bool chargerOnline()
    {
#ifdef Q_OS_LINUX
        const QDir dir("/sys/class/power_supply");
        for (const QString& e : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            const QString p = dir.filePath(e);
            const QString type = readText(p + "/type");
            if (type != "Battery" && readText(p + "/online") == "1" && (type == "USB" || type == "Mains")) {
                return true;
            }
        }
#endif
        return false;
    }

    int m_FakePct;
    int m_LowAt;
    QString m_Path;
    bool m_Probed = false;
    bool m_Low = false;
};
