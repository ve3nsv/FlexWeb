#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <QRegularExpression>
#include <cmath>
#include <optional>

inline QJsonObject defaultSstvSettings() {
    QJsonArray bands;
    const int hz[] = {3845000, 7171000, 14230000, 21340000, 28680000};
    const char *labels[] = {"80 m", "40 m", "20 m", "15 m", "10 m"};
    for (int i = 0; i < 5; ++i)
        bands.append(QJsonObject{{"label", labels[i]}, {"hz", hz[i]}, {"mode", hz[i] < 10000000 ? "LSB" : "USB"}});
    return QJsonObject{{"frequencies", bands}, {"templates", QJsonArray{}}};
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
    QJsonArray templates;
    if (input.contains("templates")) {
        if (!input["templates"].isArray()) return std::nullopt;
        templates = input["templates"].toArray();
        if (templates.size() > 12) return std::nullopt;
        const QRegularExpression color("^#[0-9a-fA-F]{6}$");
        for (const auto &item : templates) {
            if (!item.isObject()) return std::nullopt;
            const auto t = item.toObject();
            if (!t["name"].isString() || t["name"].toString().trimmed().isEmpty() || t["name"].toString().size() > 32 || !t["layers"].isArray()) return std::nullopt;
            const auto layers = t["layers"].toArray();
            if (layers.size() > 16) return std::nullopt;
            for (const auto &value : layers) {
                if (!value.isObject()) return std::nullopt;
                const auto layer = value.toObject();
                auto number = [&](const char *key, double lo, double hi) {
                    const auto v = layer[key]; const double n = v.toDouble(-1);
                    return v.isDouble() && std::isfinite(n) && n >= lo && n <= hi;
                };
                if (!layer["text"].isString() || layer["text"].toString().size() > 160 || !number("x",0,1) || !number("y",0,1)) return std::nullopt;
                if (layer["text"].toString() == "%i") {
                    if (!number("w",0.05,1) || !number("h",0.05,1)) return std::nullopt;
                } else if (!number("size",0.02,0.3) || !color.match(layer["color"].toString()).hasMatch()) return std::nullopt;
            }
        }
    }
    return QJsonObject{{"frequencies", clean}, {"templates", templates}};
}
