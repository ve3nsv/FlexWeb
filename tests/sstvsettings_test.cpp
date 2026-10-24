#include "sstvsettings.h"
#include <cassert>
int main() {
    const auto defaults = defaultSstvSettings();
    assert(validateSstvSettings(defaults));
    assert(defaults["frequencies"].toArray().size() == 5);
    assert(!validateSstvSettings({}));
    assert(!validateSstvSettings(QJsonObject{{"frequencies", QJsonArray{}}}));
    auto band = QJsonObject{{"label", "2 m"}, {"hz", 145500000}, {"mode", "FM"}};
    assert(validateSstvSettings(QJsonObject{{"frequencies", QJsonArray{band}}}));
    band["hz"] = "145500000";
    assert(!validateSstvSettings(QJsonObject{{"frequencies", QJsonArray{band}}}));
    band["hz"] = -1;
    assert(!validateSstvSettings(QJsonObject{{"frequencies", QJsonArray{band}}}));
    band["hz"] = 7170000;
    band["mode"] = "UNKNOWN";
    assert(!validateSstvSettings(QJsonObject{{"frequencies", QJsonArray{band}}}));
}
