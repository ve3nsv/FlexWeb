// Flex receive audio processing on its own thread. See include/flexrxdsp.h.

#include "flexrxdsp.h"

#include <QDateTime>
#include <QElapsedTimer>
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
    const bool modelChanged = s.model != cfg.model;
    cfg = s;
    if (modelChanged) {
        // The operator chose a model: their choice, the ladder starts over.
        steppedDown = false;
        stepUpWaitMs = kStepUpWaitMs;
    }
    applyToFilter();
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
    setModelInUse(steppedDown ? 0 : cfg.model);
}

void flexRxDsp::setModelInUse(int model)
{
    if (!filter) return;
    modelInUse = filter->setModel(model);
    loadEma = 0.0;   // a new model: measure afresh
}

void flexRxDsp::reset()
{
    if (filter) filter->reset();
    rawLine.clear();
}

// The input delayed by the filter's latency, so raw and processed audio line
// up: a pass-through then neither jumps in time nor combs in a crossfade.
QVector<float> flexRxDsp::delayedRaw(const QVector<float>& in)
{
    const int d = filter ? filter->rate() * filter->delayMs() / 1000 : 0;
    if (d <= 0) return in;
    if (rawLine.size() != d) rawLine = QVector<float>(d, 0.0f);
    QVector<float> joined = rawLine + in;
    QVector<float> out = joined.mid(0, in.size());
    rawLine = joined.mid(joined.size() - d);
    return out;
}

void flexRxDsp::logLoad(qint64 now)
{
    if (winStartMs == 0) { winStartMs = now; return; }
    if (now - winStartMs < 10000) return;
    if (winAudioSec > 0.0) {
        qInfo(logRig()).noquote() << QString("Flex: NNR %1%2 load %3 % of real time (worst block %4 %)%5")
            .arg(modelInUse ? QStringLiteral("Premium") : QStringLiteral("Standard"))
            .arg(steppedDown ? QStringLiteral(" (stepped down from Premium)") : QString())
            .arg(int(std::lround(100.0 * winProcSec / winAudioSec)))
            .arg(int(std::lround(100.0 * winPeak)))
            .arg(winBypassed ? QString(", %1 blocks passed through").arg(winBypassed) : QString());
    }
    winStartMs = now;
    winProcSec = winAudioSec = winPeak = 0.0;
    winBypassed = 0;
}

void flexRxDsp::process(QVector<float> samples, int rate, bool nnrWanted, qint64 queuedAtMs, quint32 seq)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (nnrWanted) {
        if (!filter || filter->rate() != rate) {
            filter.reset(new nnrFilter(rate));
            rawLine.clear();
            bypass = false;
            warmupLeft = 0;
            fadeToNnr = false;
            if (!filter->isValid()) {
                qWarning(logRig()) << "Flex: NNR could not start at" << rate << "Hz";
                filter.reset();
                emit nnrFailed(rate);
            } else {
                applyToFilter();
                qInfo(logRig()) << "Flex: NNR on, model" << (modelInUse ? "Premium" : "Standard")
                                << "delay" << filter->delayMs() << "ms (own thread, high priority)";
            }
        }
        if (filter && rewarm) {
            rewarm = false;
            warmupLeft = rate * filter->delayMs() / 1000 + samples.size();
        }
        if (filter) {
            const qint64 behind = now - queuedAtMs;
            const double blockSec = samples.isEmpty() ? 0.0 : double(samples.size()) / rate;

            // 1. Premium that really cannot keep up steps down to Standard.
            if (!steppedDown && cfg.model == 1 && modelInUse == 1
                && (loadEma > 0.85 || (behind > kBehindMs && loadEma > kStepDownLoad))) {
                if (stepUpAtMs && now - stepUpAtMs < 120000)
                    stepUpWaitMs = qMin(stepUpWaitMs * 2, kStepUpWaitMaxMs);   // a quick relapse
                else if (stepUpAtMs && now - stepUpAtMs > 600000)
                    stepUpWaitMs = kStepUpWaitMs;                              // it held a while
                steppedDown = true;
                stepDownAtMs = now;
                const int load = int(std::lround(100.0 * loadEma));
                setModelInUse(0);
                qWarning(logRig()).noquote() << QString("Flex: NNR Premium too slow on this PC (%1 % of real time) - "
                                                        "using Standard; Premium again in %2 s if there is room")
                                                    .arg(load).arg(stepUpWaitMs / 1000);
            }
            // ... and goes back up after a stable wait with room to spare.
            if (steppedDown && cfg.model == 1 && now - stepDownAtMs >= stepUpWaitMs
                && loadEma > 0.0 && loadEma < kStepUpLoad && behind < kCaughtUpMs) {
                steppedDown = false;
                stepUpAtMs = now;
                setModelInUse(1);
                qInfo(logRig()) << "Flex: NNR back to Premium";
            }

            // 2. Far behind: pass through, aligned and crossfaded.
            const QVector<float> raw = delayedRaw(samples);
            if (!bypass && behind > kBypassMs) {
                bypass = true;
                qWarning(logRig()).noquote() << QString("Flex: NNR %1 ms behind - receive audio passed through "
                                                        "until it catches up").arg(behind);
                // This block fades NNR -> raw.
                QVector<float> p = samples;
                filter->process(p);
                const int n = samples.size();
                for (int i = 0; i < n; ++i) {
                    const float t = n > 1 ? float(i) / (n - 1) : 1.0f;
                    samples[i] = p[i] * (1.0f - t) + raw[i] * t;
                }
                ++winBypassed;
            } else if (bypass) {
                if (behind < kCaughtUpMs) {
                    bypass = false;
                    // NNR runs unheard for its latency (its state is stale),
                    // then fades back in.
                    filter->reset();
                    warmupLeft = rate * filter->delayMs() / 1000 + samples.size();
                    qInfo(logRig()) << "Flex: NNR caught up";
                }
                samples = raw;
                ++winBypassed;
            } else {
                QVector<float> p = samples;
                QElapsedTimer t;
                t.start();
                filter->process(p);
                const double procSec = t.nsecsElapsed() / 1e9;
                if (blockSec > 0.0) {
                    const double l = procSec / blockSec;
                    loadEma = loadEma == 0.0 ? l : loadEma * 0.95 + l * 0.05;
                    winProcSec += procSec;
                    winAudioSec += blockSec;
                    winPeak = qMax(winPeak, l);
                }
                if (warmupLeft > 0) {
                    warmupLeft -= samples.size();
                    samples = raw;                     // still warming up: raw, aligned
                    if (warmupLeft <= 0) fadeToNnr = true;
                } else if (fadeToNnr) {
                    fadeToNnr = false;
                    const int n = samples.size();
                    for (int i = 0; i < n; ++i) {
                        const float f = n > 1 ? float(i) / (n - 1) : 1.0f;
                        samples[i] = raw[i] * (1.0f - f) + p[i] * f;
                    }
                } else {
                    samples = p;
                }
                if (now - gainAt >= 200) {
                    gainAt = now;
                    emit nnrGain(filter->gain(), true);
                }
            }
            logLoad(now);
        }
    } else if (filter) {
        filter->reset();
        rawLine.clear();
        bypass = false;
        warmupLeft = 0;
        fadeToNnr = false;
        rewarm = true;   // when it is wanted again: unheard warm-up, then fade in
    }

    const bool active = nnrWanted && filter;
    const int delay = filter ? filter->delayMs() : 0;
    if (active != lastActive || (active && (modelInUse != lastModel || delay != lastDelay || steppedDown != lastStepped))) {
        lastActive = active;
        lastModel = modelInUse;
        lastDelay = delay;
        lastStepped = steppedDown;
        emit nnrState(active, modelInUse, delay, steppedDown);
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
