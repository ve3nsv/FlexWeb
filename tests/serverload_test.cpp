#include "serverload.h"
#include <QCoreApplication>
#include <QThread>
#include <cassert>
#include <cmath>
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    ServerLoad load;
    assert(load.sample().isEmpty());
    QThread::msleep(40);
    const auto sample = load.sample();
    assert(sample["type"] == "serverLoad");
    assert(sample["cores"].toInt() >= 1);
    for (const char *field : {"systemPercent", "processPercent"}) {
        assert(sample[field].isDouble());
        assert(std::isfinite(sample[field].toDouble()));
        assert(sample[field].toDouble() >= 0 && sample[field].toDouble() <= 100);
    }
    assert(sample["processMemoryBytes"].toDouble() > 0);
}
