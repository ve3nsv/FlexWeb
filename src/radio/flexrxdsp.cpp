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
    chain.applyConfig(cfg);
    chain.setNnrStepDown(steppedDown);
    loadEma = 0.0;   // new settings / model: measure afresh
}

void flexRxDsp::reset()
{
    chain.reset();
    rawLine.clear();
}

void flexRxDsp::pause()
{
    // TX: the next receive audio warms the reducer up unheard, then fades in.
    chain.reset();
    rawLine.clear();
    bypass = false;
    lastActive = false;
}

// The input delayed by the filter's latency, so raw and processed audio line
// up: a pass-through then neither jumps in time nor combs in a crossfade.
QVector<float> flexRxDsp::delayedRaw(const QVector<float>& in)
{
    rxDenoiser* f = chain.current();
    const int d = f ? f->rate() * f->delayMs() / 1000 : 0;
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
        const int k = chain.kind();
        const QString what = k == rxNrConfig::NNR
            ? QString("NNR %1").arg(chain.nnrModelInUse() ? QStringLiteral("Premium") : QStringLiteral("Standard"))
            : QString::fromLatin1(rxNr::name(k));
        qInfo(logRig()).noquote() << QString("Flex: %1%2 load %3 % of real time (worst block %4 %)%5")
            .arg(what)
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
    const int kind = nnrWanted ? cfg.kind : -1;
    const qint64 behind = now - queuedAtMs;
    const double blockSec = samples.isEmpty() ? 0.0 : double(samples.size()) / rate;

    if (chain.current() && kind >= 0 && kind == chain.kind() && chain.current()->rate() == rate) {
        // Running: the NNR overload ladder, then pass-through when far behind.
        const bool nnr = kind == rxNrConfig::NNR;
        // 1. Premium that really cannot keep up steps down to Standard.
        if (nnr && !steppedDown && cfg.model == 1 && chain.nnrModelInUse() == 1
            && (loadEma > 0.85 || (behind > kBehindMs && loadEma > kStepDownLoad))) {
            if (stepUpAtMs && now - stepUpAtMs < 120000)
                stepUpWaitMs = qMin(stepUpWaitMs * 2, kStepUpWaitMaxMs);   // a quick relapse
            else if (stepUpAtMs && now - stepUpAtMs > 600000)
                stepUpWaitMs = kStepUpWaitMs;                              // it held a while
            steppedDown = true;
            stepDownAtMs = now;
            const int load = int(std::lround(100.0 * loadEma));
            chain.setNnrStepDown(true);
            loadEma = 0.0;
            qWarning(logRig()).noquote() << QString("Flex: NNR Premium too slow on this PC (%1 % of real time) - "
                                                    "using Standard; Premium again in %2 s if there is room")
                                                .arg(load).arg(stepUpWaitMs / 1000);
        }
        // ... and goes back up after a stable wait with room to spare.
        if (nnr && steppedDown && cfg.model == 1 && now - stepDownAtMs >= stepUpWaitMs
            && loadEma > 0.0 && loadEma < kStepUpLoad && behind < kCaughtUpMs) {
            steppedDown = false;
            stepUpAtMs = now;
            chain.setNnrStepDown(false);
            loadEma = 0.0;
            qInfo(logRig()) << "Flex: NNR back to Premium";
        }
        // 2. Far behind: pass through, aligned and crossfaded.
        const QVector<float> raw = delayedRaw(samples);
        if (!bypass && behind > kBypassMs) {
            bypass = true;
            qWarning(logRig()).noquote() << QString("Flex: %1 %2 ms behind - receive audio passed through "
                                                    "until it catches up").arg(rxNr::name(kind)).arg(behind);
            QVector<float> p = samples;
            chain.run(p, kind, rate, cfg);
            const int n = samples.size();
            for (int i = 0; i < n; ++i) {
                const float t = n > 1 ? float(i) / (n - 1) : 1.0f;
                samples[i] = p[i] * (1.0f - t) + raw[i] * t;
            }
            ++winBypassed;
        } else if (bypass) {
            if (behind < kCaughtUpMs) {
                bypass = false;
                chain.reset();       // stale state: runs unheard for its latency, then fades in
                qInfo(logRig()).noquote() << QString("Flex: %1 caught up").arg(rxNr::name(kind));
            }
            samples = raw;
            ++winBypassed;
        } else {
            QElapsedTimer t;
            t.start();
            chain.run(samples, kind, rate, cfg);
            const double procSec = t.nsecsElapsed() / 1e9;
            if (blockSec > 0.0) {
                const double l = procSec / blockSec;
                loadEma = loadEma == 0.0 ? l : loadEma * 0.95 + l * 0.05;
                winProcSec += procSec;
                winAudioSec += blockSec;
                winPeak = qMax(winPeak, l);
            }
            if (now - gainAt >= 200 && chain.current()) {
                gainAt = now;
                emit nnrGain(chain.current()->gain(), true);
            }
        }
        logLoad(now);
    } else {
        // Starting, switching kind or rate, or stopping: the chain crossfades.
        chain.run(samples, kind, rate, cfg);
        rawLine.clear();
        bypass = false;
        loadEma = 0.0;
        if (!chain.failed()) {
            failReported = false;
        } else if (!failReported) {         // once, until the request changes
            failReported = true;
            qWarning(logRig()).noquote() << QString("Flex: %1 could not start at %2 Hz").arg(rxNr::name(cfg.kind)).arg(rate);
            emit nnrFailed(rate);
        }
        if (!chain.failed() && chain.current() && chain.kind() != lastKind) {
            qInfo(logRig()).noquote() << QString("Flex: %1 on%2, delay %3 ms (own thread, high priority)")
                .arg(rxNr::name(chain.kind()))
                .arg(chain.kind() == rxNrConfig::NNR ? (chain.nnrModelInUse() ? ", model Premium" : ", model Standard") : "")
                .arg(chain.current()->delayMs());
        }
        lastKind = chain.kind();
    }

    const bool active = chain.current() != nullptr;
    const int delay = active ? chain.current()->delayMs() : 0;
    const int modelInUse = chain.nnrModelInUse();
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
