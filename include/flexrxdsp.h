#ifndef FLEXRXDSP_H
#define FLEXRXDSP_H

// Punch list 52: the Flex's receive audio processing - NNR and the conversion
// to the browsers' PCM - on its own thread, away from flexCommander's thread,
// which owns the radio's command link. With NNR inside flexCommander's UDP
// loop (2026-10-29, NNR Premium on a busy PC) the loop barely ended and PTT /
// TUNE waited for seconds behind the audio; AetherSDR keeps the same split
// (RadioConnection / PanadapterStream / AudioEngine threads).
//
// flexCommander decodes and resamples each RX audio packet (cheap) and hands
// the block here with the time it did so; the blocks are processed in order.
//
// Punch list 57: NNR never cuts out on its own account. The thread runs at
// high priority (set by flexCommander). Falling behind is handled as a
// ladder, gentlest first:
//   1. Premium that cannot keep up (measured load, not a scheduling hiccup)
//      steps down to Standard - NNR stays on - and steps back up after a
//      stable minute (the wait doubles after a quick relapse, up to 10 min).
//   2. Only past kBypassMs behind does audio pass through unprocessed, with
//      the raw path delayed by the filter's own latency and a crossfade in
//      and out, so it is neither a jump in time nor a click.
// Every 10 s the log gives NNR's load (CPU time per second of audio).

#include <QObject>
#include <QVector>
#include <QByteArray>
#include <memory>

#include "audioconverter.h"
#include "nnrfilter.h"

class flexRxDsp : public QObject
{
    Q_OBJECT
public:
    struct Settings {
        int strength = 38, model = 0, alpha = 100, knee = 100, tau = 200,
            maxGain = 120, attack = 0, release = 0;
    };
    explicit flexRxDsp(const QByteArray& guid, QObject* parent = nullptr);
    ~flexRxDsp() override;

    static constexpr qint64 kBehindMs = 200;       // behind this far: look at the load
    static constexpr qint64 kBypassMs = 600;       // behind this far: pass through
    static constexpr qint64 kCaughtUpMs = 50;      // back within this: NNR again
    static constexpr double kStepDownLoad = 0.70;  // Premium using this much of real time
    static constexpr double kStepUpLoad = 0.35;    // Standard using this little: try Premium
    static constexpr qint64 kStepUpWaitMs = 60000;
    static constexpr qint64 kStepUpWaitMaxMs = 600000;

    // All of these run on the worker's thread (posted by flexCommander).
    void process(QVector<float> samples, int rate, bool nnrWanted, qint64 queuedAtMs, quint32 seq);
    void applySettings(const Settings& s);
    void reset();
    // flexCommander already told the page NNR paused (on TX): start over as
    // inactive, so resuming and the next stop are both reported.
    void pause();

signals:
    void audio(const audioPacket& pkt);
    // ~5 per second while NNR runs (the page's NR GAIN).
    void nnrGain(float gain, bool active);
    // NNR running or not, the model actually in use, its delay, and whether
    // Premium was stepped down to Standard for load; sent on change.
    void nnrState(bool active, int modelInUse, int delayMs, bool steppedDown);
    // The filter could not start at this rate: flexCommander turns NNR off.
    void nnrFailed(int rate);

private:
    void applyToFilter();
    void setModelInUse(int model);
    QVector<float> delayedRaw(const QVector<float>& in);
    void logLoad(qint64 now);

    QByteArray guid;
    Settings cfg;
    std::unique_ptr<nnrFilter> filter;
    int modelInUse = 0;
    qint64 gainAt = 0;
    bool lastActive = false;
    int lastModel = -1, lastDelay = -1;
    bool lastStepped = false;

    // Load: CPU time of filter->process over the audio it covered.
    double loadEma = 0.0;
    double winProcSec = 0.0, winAudioSec = 0.0, winPeak = 0.0;
    int winBypassed = 0;
    qint64 winStartMs = 0;

    // Step-down ladder.
    bool steppedDown = false;
    qint64 stepDownAtMs = 0, stepUpAtMs = 0;
    qint64 stepUpWaitMs = kStepUpWaitMs;

    // Bypass with a latency-aligned raw path and crossfades.
    bool bypass = false;
    int warmupLeft = 0;          // samples NNR runs unheard after a bypass
    bool fadeToNnr = false;      // next processed block fades raw -> NNR
    bool rewarm = false;         // NNR was paused: warm up unheard before it is heard
    QVector<float> rawLine;      // the last `delay` samples of input
};

#endif // FLEXRXDSP_H
