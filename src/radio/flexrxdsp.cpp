// Flex receive audio processing on its own thread. See include/flexrxdsp.h.

#include "flexrxdsp.h"

#include <QDateTime>
#include <QTime>
#include <QtEndian>
#include <QDebug>
#include <algorithm>
#include <cmath>
#include <cstring>

#include "logcategories.h"

flexRxDsp::flexRxDsp(const QByteArray& g, QObject* parent)
    : QObject(parent), guid(g)
{
}

flexRxDsp::~flexRxDsp() = default;

void flexRxDsp::applySettings(const Settings& s)
{
    if (cfg.method != s.method) {
        if (nr2) nr2->reset();
        if (filter) filter->reset();
        nr2WasActive = false; lastActive = false;
    }
    cfg = s;
    applyToFilter();
    if (nr2) {
        nr2->setGainMax(cfg.nr2Reduction / 100.0f);
        nr2->setGainFloor(cfg.nr2Naturalness / 100.0f);
        nr2->setGainSmooth(cfg.nr2Smoothing / 100.0f);
        nr2->setQspp(cfg.nr2Threshold / 100.0f);
        nr2->setGainMethod(cfg.nr2Gain); nr2->setNpeMethod(cfg.nr2Noise);
        nr2->setAeFilter(cfg.nr2Ae != 0);
    }
}

void flexRxDsp::applyToFilter()
{
    if (!filter) return;
    filter->setStrength(cfg.strength);
    filter->setAlpha(cfg.alpha / 100.0);
    filter->setKnee(cfg.knee / 10.0);
    filter->setTau(cfg.tau / 100.0);
    filter->setMaxGain(cfg.maxGain / 10.0);
    filter->setSmooth(cfg.attack, cfg.release);
    modelInUse = filter->setModel(cfg.model);
}

void flexRxDsp::reset()
{
    if (filter) filter->reset();
    if (nr2) nr2->resetTransient();
}

void flexRxDsp::process(QVector<float> samples, int rate, bool nnrWanted, qint64 queuedAtMs, quint32 seq)
{
    if (cfg.method == 1) {
        const bool active = nnrWanted;
        if (active && (!nr2 || nr2Rate != rate)) {
            nr2.reset(new AetherSDR::SpectralNR(2048, rate, 4)); nr2Rate = rate;
            applySettings(cfg);
        }
        if (active && nr2) {
            if (!nr2WasActive) nr2->resetTransient();
            if (QDateTime::currentMSecsSinceEpoch() - queuedAtMs < kOverloadMs) {
                double before = 0; for (float x : samples) before += double(x)*x;
                QVector<float> output(samples.size());
                nr2->process(samples.constData(), output.data(), samples.size()); samples.swap(output);
                const qint64 now = QDateTime::currentMSecsSinceEpoch();
                if (now - gainAt >= 200) {
                    double after = 0; for (float x : samples) after += double(x)*x;
                    emit nnrGain(before > 1e-12 ? float(qBound(0.0, std::sqrt(after/before), 1.0)) : 1.0f, true); gainAt = now;
                }
            }
        }
        nr2WasActive = active;
    } else if (nnrWanted) {
        if (!filter || filter->rate() != rate) {
            filter.reset(new nnrFilter(rate));
            if (!filter->isValid()) {
                qWarning(logRig()) << "Flex: NNR could not start at" << rate << "Hz";
                filter.reset();
                emit nnrFailed(rate);
            } else {
                applyToFilter();
                qInfo(logRig()) << "Flex: NNR on, model" << (modelInUse ? "Premium" : "Standard")
                                << "delay" << filter->delayMs() << "ms (own thread)";
            }
        }
        if (filter) {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            const qint64 behind = now - queuedAtMs;
            if (!overloaded && behind > kOverloadMs) {
                overloaded = true;
                passedThrough = 0;
                qWarning(logRig()).noquote() << QString("Flex: NNR overloaded (%1 ms behind) - receive audio "
                                                        "passed through unprocessed until it catches up").arg(behind);
            } else if (overloaded && behind < kCaughtUpMs) {
                overloaded = false;
                qInfo(logRig()).noquote() << QString("Flex: NNR caught up (%1 blocks passed through)").arg(passedThrough);
            }
            if (overloaded) {
                ++passedThrough;
            } else {
                filter->process(samples);
                if (now - gainAt >= 200) {
                    gainAt = now;
                    emit nnrGain(filter->gain(), true);
                }
            }
        }
    } else if (filter) {
        filter->reset();
        overloaded = false;
    }

    if (cfg.method == 0) nr2WasActive = false;
    const bool active = nnrWanted && (cfg.method == 1 ? bool(nr2) : bool(filter));
    const int delay = cfg.method == 1 ? (nr2 ? 2048000 / rate : 0) : (filter ? filter->delayMs() : 0);
    if (active != lastActive || (active && (modelInUse != lastModel || delay != lastDelay))) {
        lastActive = active;
        lastModel = modelInUse;
        lastDelay = delay;
        emit nnrState(active, modelInUse, delay);
    }

    audioPacket pkt;
    pkt.seq = seq;
    pkt.time = QTime::currentTime();
    pkt.sent = 0;
    std::memset(pkt.guid, 0, GUIDLEN);
    std::memcpy(pkt.guid, guid.constData(), size_t(qMin(guid.size(), int(GUIDLEN))));
    const int n = samples.size();
    pkt.data.resize(n * 2);
    auto* dst = reinterpret_cast<qint16*>(pkt.data.data());
    float peak = 0.0f;
    double sumSq = 0.0;
    for (int i = 0; i < n; ++i) {
        const float s = qBound(-1.0f, samples[i], 1.0f);
        dst[i] = qToLittleEndian<qint16>(qint16(std::lround(s * 32767.0f)));
        peak = qMax(peak, std::fabs(s));
        sumSq += double(s) * s;
    }
    pkt.amplitudePeak = peak;
    pkt.amplitudeRMS = n ? float(std::sqrt(sumSq / n)) : 0.0f;
    pkt.volume = 1.0;
    emit audio(pkt);
}
