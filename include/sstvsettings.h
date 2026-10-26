#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <cmath>
#include <optional>

inline QJsonObject defaultSstvSettings() {
    QJsonArray bands;
    const int hz[] = {3845000, 7171000, 14230000, 21340000, 28680000};
    const char *labels[] = {"80 m", "40 m", "20 m", "15 m", "10 m"};
    for (int i = 0; i < 5; ++i)
        bands.append(QJsonObject{{"label", labels[i]}, {"hz", hz[i]}, {"mode", hz[i] < 10000000 ? "LSB" : "USB"}});
    return QJsonObject{{"frequencies", bands}};
}

// Reject the entire edit on error; never silently drop an operator's bands.
inline std::optional<QJsonObject> validateSstvSettings(const QJsonObject &input) {
    if (!input.value("frequencies").isArray()) return std::nullopt;
    const auto bands = input.value("frequencies").toArray();
    if (bands.isEmpty() || bands.size() > 30) return std::nullopt;
    QJsonArray clean;
    const QStringList modes{QString(), "LSB", "USB", "FM", "AM"};
    for (const auto &value : bands) {
        if (!value.isObject()) return std::nullopt;
        const auto band = value.toObject();
        const QString label = band.value("label").toString().trimmed();
        const double hz = band.value("hz").toDouble(-1);
        const QString mode = band.value("mode").toString().toUpper();
        if (label.isEmpty() || label.size() > 24 || !std::isfinite(hz) || hz < 100000 || hz > 1300000000
            || std::floor(hz) != hz || !modes.contains(mode)) return std::nullopt;
        clean.append(QJsonObject{{"label", label}, {"hz", hz}, {"mode", mode}});
    }
    return QJsonObject{{"frequencies", clean}};
}
