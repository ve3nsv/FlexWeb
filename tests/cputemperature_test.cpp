#include "cputemperature.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <cassert>
#include <limits>

static QJsonObject sensor(const QString &name, const QString &id, QJsonValue value) {
    return QJsonObject{{"Name", name}, {"Identifier", id}, {"Value", value}};
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    using namespace CpuTemperature;
    QJsonArray sensors{sensor("GPU Core", "/nvidiagpu/0/temperature/0", 90),
        sensor("Core #1", "/intelcpu/0/temperature/1", 70),
        sensor("CPU Package", "/intelcpu/0/temperature/0", 65.5)};
    auto reading = select(sensors, "test");
    assert(reading && reading->sensor == "CPU Package" && reading->celsius == 65.5);
    sensors.removeAt(2);
    sensors.append(sensor("Core #2", "/intelcpu/0/temperature/2", 75));
    assert(select(sensors, "test")->celsius == 75);
    assert(!select({sensor("CPU Package", "/intelcpu/0/temperature/0", QJsonValue())}, "test"));
    assert(!select({sensor("CPU Package", "/intelcpu/0/temperature/0", "65")}, "test"));
    assert(!select({sensor("CPU Package", "/intelcpu/0/temperature/0", 0)}, "test"));
    assert(!select({sensor("CPU Package", "/intelcpu/0/temperature/0", 999)}, "test"));
    assert(!select({sensor("CPU Package", "/lpc/0/temperature/0", 65)}, "test"));
    Monitor monitor;
    assert(monitor.snapshot()["cpuTemperatureC"].isNull());
    monitor.accept(QJsonDocument(QJsonObject{{"Provider", "test"}, {"Sensors", sensors}}).toJson());
    assert(monitor.snapshot()["cpuTemperatureC"].toDouble() == 75);
    assert(monitor.snapshot(-1)["cpuTemperatureStatus"] == "stale");
    assert(monitor.snapshot(-1)["cpuTemperatureC"].isNull());
    monitor.accept("not JSON");
    assert(monitor.snapshot()["cpuTemperatureC"].isNull());
    QTemporaryDir root;
    QDir().mkpath(root.path() + "/hwmon0");
    auto write = [&](const QString &file, const char *text) {
        QFile f(root.path() + "/hwmon0/" + file); assert(f.open(QIODevice::WriteOnly)); f.write(text);
    };
    write("name", "coretemp\n"); write("temp1_label", "Package id 0\n"); write("temp1_input", "62500\n");
    assert(select(linuxSensors(root.path()), "hwmon")->celsius == 62.5);
    write("temp1_fault", "1\n"); assert(linuxSensors(root.path()).isEmpty());
    write("temp1_fault", "0\n"); write("name", "acpitz\n"); assert(linuxSensors(root.path()).isEmpty());
}
