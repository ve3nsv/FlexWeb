// Flex receive audio processing on its own thread. See include/flexrxdsp.h.

#include "flexrxdsp.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QMutexLocker>
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

// Producer-side mailbox: at most 200 ms / 32 blocks; only one drain event.
void flexRxDsp::enqueue(QVector<float> samples, int rate, bool wanted, qint64 at, quint32 seq)
{
    if (rate <= 0) return;
    QMutexLocker lock(&pendingMutex);
    const double duration = 1000.0 * samples.size() / rate;
    if (duration > 200) { ++droppedBlocks; return; }
    while (!pending.empty() && (pendingMs + duration > 200 || pending.size() >= 32)) {
        pendingMs -= 1000.0 * pending.front().samples.size() / pending.front().rate;
        pending.pop_front(); ++droppedBlocks;
    }
    pending.push_back({std::move(samples), rate, wanted, at, seq}); pendingMs += duration;
    if (!drainScheduled) {
        drainScheduled = true;
        QMetaObject::invokeMethod(this, [this] { drain(); }, Qt::QueuedConnection);
    }
}

void flexRxDsp::drain()
{
    Block b; double dropped;
    {
        QMutexLocker lock(&pendingMutex);
        if (pending.empty()) { drainScheduled = false; return; }
        b = std::move(pending.front()); pending.pop_front();
        pendingMs -= 1000.0 * b.samples.size() / b.rate;
        dropped = droppedBlocks;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const int waitMs = int(qMax<qint64>(0, now-b.at));
    QElapsedTimer elapsed; elapsed.start();
    // Fail open before overload grows: preserve raw audio rather than spend
    // another neural/spectral processing block while already behind.
    process(std::move(b.samples), b.rate, b.wanted && eligible.load() && waitMs < 100, b.at, b.seq);
    if (now-statsAt >= 1000) {
        statsAt = now;
        emit processingStats(waitMs, int(elapsed.nsecsElapsed()/1000), dropped);
    }
    QMetaObject::invokeMethod(this, [this] { drain(); }, Qt::QueuedConnection);
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
                const qint64 now = QDateTime::currentMSecsSinceEpoch();
                const bool reportGain = now - gainAt >= 200;
                double before = 0; if (reportGain) for (float x : samples) before += double(x)*x;
                QVector<float> output(samples.size());
                nr2->process(samples.constData(), output.data(), samples.size()); samples.swap(output);
                if (reportGain) {
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
