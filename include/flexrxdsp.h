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
// When NNR falls more than kOverloadMs behind, blocks pass through
// unprocessed until it has caught up (logged once per episode) instead of
// building a backlog.

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

    static constexpr qint64 kOverloadMs = 200;    // this far behind: pass audio through
    static constexpr qint64 kCaughtUpMs = 50;     // back within this: NNR again

    // All of these run on the worker's thread (posted by flexCommander).
    void process(QVector<float> samples, int rate, bool nnrWanted, qint64 queuedAtMs, quint32 seq);
    void applySettings(const Settings& s);
    void reset();

signals:
    void audio(const audioPacket& pkt);
    // ~5 per second while NNR runs (the page's NR GAIN).
    void nnrGain(float gain, bool active);
    // NNR running or not, the model actually in use and its delay; sent on change.
    void nnrState(bool active, int modelInUse, int delayMs);
    // The filter could not start at this rate: flexCommander turns NNR off.
    void nnrFailed(int rate);

private:
    void applyToFilter();
    QByteArray guid;
    Settings cfg;
    std::unique_ptr<nnrFilter> filter;
    int modelInUse = 0;
    qint64 gainAt = 0;
    bool lastActive = false;
    int lastModel = -1, lastDelay = -1;
    bool overloaded = false;
    int passedThrough = 0;
};

#endif // FLEXRXDSP_H
