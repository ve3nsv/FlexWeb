// Receive noise reduction (punch list 59 / 60): NR2 and RN2 lower a
// noise-only signal, keep a voice-like one, and switching NNR -> NR2 -> RN2
// -> off through rxNrChain leaves no gap and no click. Built and run by
// tests/test_rxnr.py (the RNNoise weights come from a binary .rcc of
// resources/rnnoise.qrc, registered below).
#include "rxnr.h"

#include <QCoreApplication>
#include <QResource>
#include <QVector>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    std::printf(__VA_ARGS__); std::printf("\n"); ++failures; } } while (0)

static double rmsOf(const QVector<float>& v, int from, int to)
{
    double s = 0.0;
    for (int i = from; i < to; ++i) s += double(v[i]) * v[i];
    return to > from ? std::sqrt(s / (to - from)) : 0.0;
}

static QVector<float> whiteNoise(int n, float sigma, unsigned seed)
{
    std::mt19937 g(seed);
    std::normal_distribution<float> d(0.0f, sigma);
    QVector<float> v(n);
    for (float& x : v) x = d(g);
    return v;
}

// Speech-like: syllables of 120-250 ms (voiced, 110-190 Hz with a pitch
// glide and 12 harmonics, rising and falling) with short gaps, in phrases of
// ~1.6 s with 0.5 s pauses, over light noise. A steady tone would not do: a
// spectral NR rightly learns anything stationary as noise within seconds.
// `voiced` marks the samples inside a syllable.
static QVector<float> voice(int n, int rate, unsigned seed, QVector<char>* voiced = nullptr)
{
    QVector<float> v = whiteNoise(n, 0.004f, seed);
    if (voiced) voiced->fill(0, n);
    std::mt19937 g(seed + 1);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    int i = int(0.1 * rate);
    while (i < n) {
        const int phraseEnd = std::min(n, i + int((1.3 + 0.6 * u(g)) * rate));
        while (i < phraseEnd) {
            const int len = int((0.12 + 0.13 * u(g)) * rate);
            const double f0 = 110.0 + 80.0 * u(g), glide = (u(g) - 0.5) * 40.0;
            double ph = 0.0;
            for (int k = 0; k < len && i + k < n; ++k) {
                const double x = double(k) / len;
                const double env = std::sin(M_PI * x);              // rise and fall
                ph += 2 * M_PI * (f0 + glide * x) / rate;
                double s = 0.0;
                for (int h = 1; h <= 12; ++h) s += std::sin(h * ph + h) / h * (1.0 + 0.5 * std::sin(h * 0.7 + x * 3));
                v[i + k] += float(0.1 * env * s);
                if (voiced && env > 0.5) (*voiced)[i + k] = 1;
            }
            i += len + int((0.04 + 0.06 * u(g)) * rate);
        }
        i += int(0.5 * rate);
    }
    return v;
}

// Through a reducer the way a host feeds it: 20 ms blocks.
static QVector<float> through(rxDenoiser& d, const QVector<float>& in, int block)
{
    QVector<float> out;
    for (int i = 0; i < in.size(); i += block) {
        QVector<float> b = in.mid(i, block);
        d.process(b);
        out += b;
    }
    return out;
}

static void noiseIsLowered(int kind, int rate)
{
    rxNrConfig cfg;
    auto d = rxNr::make(kind, rate);
    CHECK(d, "%s did not start at %d Hz", rxNr::name(kind), rate);
    if (!d) return;
    rxNr::apply(d.get(), kind, cfg, 0);
    const QVector<float> in = whiteNoise(rate * 4, 0.1f, 7);
    const QVector<float> out = through(*d, in, rate / 50);
    CHECK(out.size() == in.size(), "%s: %d samples in, %d out", rxNr::name(kind), int(in.size()), int(out.size()));
    const double a = rmsOf(in, rate * 3, rate * 4), b = rmsOf(out, rate * 3, rate * 4);
    std::printf("%s @%d: noise only %.4f -> %.4f (%.1f dB), delay %d ms\n", rxNr::name(kind), rate, a, b,
                20 * std::log10(std::max(b, 1e-9) / a), d->delayMs());
    CHECK(b < 0.5 * a, "%s @%d Hz did not lower noise: %.4f -> %.4f", rxNr::name(kind), rate, a, b);
    // ... and lets a voice through (not silenced with the noise)
    auto v = rxNr::make(kind, rate);
    rxNr::apply(v.get(), kind, cfg, 0);
    const QVector<float> vin = voice(rate * 4, rate, 9);
    const QVector<float> vout = through(*v, vin, rate / 50);
    const double va = rmsOf(vin, rate * 1, rate * 4), vb = rmsOf(vout, rate * 1, rate * 4);
    std::printf("%s @%d: voice %.4f -> %.4f\n", rxNr::name(kind), rate, va, vb);
    CHECK(vb > 0.3 * va, "%s @%d Hz removed the voice: %.4f -> %.4f", rxNr::name(kind), rate, va, vb);
}

// NNR -> NR2 -> RN2 -> off -> NR2, a second each, through the chain: after
// the first warm-up no 10 ms window falls far below its neighbourhood (a
// gap), and no step between samples is much bigger than the signal's own (a
// click).
static void switchingIsSeamless(int rate)
{
    rxNrConfig cfg;
    rxNrChain chain;
    const int block = rate / 50;
    QVector<char> voiced;
    const QVector<float> in = voice(rate * 6, rate, 3, &voiced);
    const int kinds[] = {rxNrConfig::NNR, rxNrConfig::NR2, rxNrConfig::RN2, -1, rxNrConfig::NR2, rxNrConfig::NR2};
    QVector<float> out;
    for (int i = 0; i < in.size(); i += block) {
        QVector<float> b = in.mid(i, block);
        int kind = kinds[std::min(5, i / rate)];
        if (kind == rxNrConfig::NNR && !rxNr::rateOk(kind, rate)) kind = rxNrConfig::NR2;
        chain.run(b, kind, rate, cfg);
        CHECK(b.size() == block || i + block > in.size(), "block of %d became %d", block, int(b.size()));
        out += b;
    }
    float inStep = 0.0f, outStep = 0.0f;
    for (int i = 1; i < in.size(); ++i) inStep = std::max(inStep, std::fabs(in[i] - in[i - 1]));
    int worstAt = 0;
    for (int i = rate / 2; i < out.size(); ++i) {
        const float d = std::fabs(out[i] - out[i - 1]);
        if (d > outStep) { outStep = d; worstAt = i; }
    }
    std::printf("switching @%d: largest step in %.4f, out %.4f (at %.3f s)\n", rate, inStep, outStep,
                double(worstAt) / rate);
    CHECK(outStep < 2.0f * inStep, "a click at %.3f s: step %.4f vs the signal's %.4f",
          double(worstAt) / rate, outStep, inStep);
    // A gap: a voiced 10 ms window that comes out far weaker than it went in
    // (the reducers' delays are under 70 ms: compare with the input window
    // that delay earlier, the best of them).
    const int w = rate / 100;
    int gaps = 0;
    for (int k = rate / w; (k + 1) * w <= out.size(); ++k) {          // after the first second
        const int a = k * w;
        bool allVoiced = true;
        for (int j = a; j < a + w && allVoiced; ++j) allVoiced = voiced[std::max(0, j - rate / 14)] && voiced[j];
        if (!allVoiced) continue;
        double best = 0.0;
        for (int lagMs = 0; lagMs <= 70; lagMs += 5) {
            const int b = a - lagMs * rate / 1000;
            if (b >= 0) best = std::max(best, rmsOf(in, b, b + w));
        }
        const double o = rmsOf(out, a, a + w);
        if (o < 0.2 * best && gaps++ < 5)
            CHECK(false, "a gap at %.2f s: %.4f out of %.4f", double(a) / rate, o, best);
    }
    std::printf("switching @%d: %d gaps\n", rate, gaps);
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2 || !QResource::registerResource(QString::fromLocal8Bit(argv[1]))) {
        std::printf("usage: %s <rnnoise.rcc>\n", argv[0]);
        return 2;
    }
    for (int rate : {48000, 24000, 16000, 8000}) {
        if (rxNr::rateOk(rxNrConfig::NNR, rate)) noiseIsLowered(rxNrConfig::NNR, rate);
        noiseIsLowered(rxNrConfig::NR2, rate);
        noiseIsLowered(rxNrConfig::RN2, rate);
    }
    switchingIsSeamless(48000);
    switchingIsSeamless(24000);

    // the settings round-trip and stay in range
    rxNrConfig c;
    CHECK(c.set("nrKind", 7) && c.kind == 2, "nrKind not clamped: %d", c.kind);
    CHECK(c.set("nr2Max", 999) && c.nr2Max == 200, "nr2Max not clamped: %d", c.nr2Max);
    CHECK(c.set("rn2Dry", -3) && c.rn2Dry == 0, "rn2Dry not clamped: %d", c.rn2Dry);
    CHECK(!c.set("nope", 1), "an unknown key was taken");
    CHECK(rxNrConfig::isKey("nr2Fill") && !rxNrConfig::isKey("setPTT"), "isKey");

    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
