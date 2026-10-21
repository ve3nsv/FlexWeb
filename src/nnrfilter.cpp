// See include/nnrfilter.h.

#include "nnrfilter.h"

#include <QtGlobal>
#include <cmath>

extern "C" {
#include "../resources/wdsp-nnr/comm.h"     // the WDSP NNR subset
}

namespace {
// What the models were trained for (AetherSDR NnrFilter; WDSP RXA.c).
constexpr int kNetworkRate = 16000;
constexpr int kFftSize = 512;
constexpr int kOverlap = 2;
constexpr int kLookahead = 1;
double maskFloor(int strength) { return -10.0 - 40.0 * qBound(0, strength, 100) / 100.0; }
}

nnrFilter::nnrFilter(int rate) : sampleRate(rate)
{
    if (!rateSupported(rate))
        return;
    blockIn.fill(0.0, 2 * kBlock);
    blockOut.fill(0.0, 2 * kBlock);
    nnr = create_nnr(1, 0, kBlock, blockIn.data(), blockOut.data(), rate, kNetworkRate,
                     kFftSize, kOverlap, kLookahead, maskFloor(38), 1);
    outQ.fill(0.0f, kBlock);           // one block of latency, so every call returns its full length
}

nnrFilter::~nnrFilter()
{
    if (nnr)
        destroy_nnr(static_cast<NNR>(nnr));
}

void nnrFilter::reset()
{
    if (!nnr)
        return;
    flush_nnr(static_cast<NNR>(nnr));
    acc.clear();
    outQ.fill(0.0f, kBlock);
}

void nnrFilter::process(QVector<float>& samples)
{
    if (!nnr || samples.isEmpty())
        return;
    double pre = 0.0, post = 0.0;
    for (float s : samples) pre += double(s) * s;
    acc += samples;
    int done = 0;
    while (acc.size() - done >= kBlock) {
        for (int i = 0; i < kBlock; ++i) {
            blockIn[2 * i] = acc[done + i];
            blockIn[2 * i + 1] = 0.0;
        }
        xnnr(static_cast<NNR>(nnr), 0);
        for (int i = 0; i < kBlock; ++i)
            outQ.append(float(blockOut[2 * i]));
        done += kBlock;
    }
    if (done) acc.remove(0, done);
    const int n = qMin(samples.size(), outQ.size());
    for (int i = 0; i < n; ++i) {
        samples[i] = outQ[i];
        post += double(samples[i]) * samples[i];
    }
    outQ.remove(0, n);
    if (pre > 1e-9)
        lastGain = float(qBound(0.0, std::sqrt(post / pre), 1.0));
}

void nnrFilter::setStrength(int strength) { if (nnr) setMaskFloor_nnr(static_cast<NNR>(nnr), maskFloor(strength)); }
int nnrFilter::setModel(int model) { return nnr ? setModel_nnr(static_cast<NNR>(nnr), qBound(0, model, 1)) : 0; }
void nnrFilter::setAlpha(double v) { if (nnr) setAlpha_nnr(static_cast<NNR>(nnr), qBound(0.0, v, 4.0)); }
void nnrFilter::setKnee(double dB) { if (nnr) setAlphaKnee_nnr(static_cast<NNR>(nnr), qBound(0.0, dB, 40.0)); }
void nnrFilter::setTau(double s) { if (nnr) setTau_nnr(static_cast<NNR>(nnr), qBound(0.05, s, 30.0)); }
void nnrFilter::setMaxGain(double dB) { if (nnr) setMaxGain_nnr(static_cast<NNR>(nnr), qBound(0.0, dB, 24.0)); }
void nnrFilter::setSmooth(double a, double r)
{
    if (nnr) setSmooth_nnr(static_cast<NNR>(nnr), qBound(0.0, a, 500.0), qBound(0.0, r, 500.0));
}

int nnrFilter::delayMs() const
{
    return nnr ? int(1000.0 * (getDelay_nnr(static_cast<NNR>(nnr)) + kBlock) / sampleRate) : 0;
}
