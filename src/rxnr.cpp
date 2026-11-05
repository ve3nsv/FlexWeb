// Receive noise reduction: NNR / NR2 / RN2 and the switching between them.
// See include/rxnr.h.
#include "rxnr.h"

#include "nnrfilter.h"
#include "dsp/spectralnr.h"
#ifdef WFWEB_RN2   // RN2 (Xiph RNNoise) is not in the build yet: punch list 60
#include "rnnoise.h"
#include "audio/resampler/speex_resampler.h"
#endif

#include <QDebug>
#include <QFile>
#include <QSettings>
#include <QStringList>
#include <QtGlobal>
#include <algorithm>
#include <cmath>
#include <mutex>

namespace {

QSettings* store()
{
    const QString dir = qEnvironmentVariable("WFWEB_CONFIG_DIR");
    return dir.isEmpty() ? new QSettings("wfweb", "nnr") : new QSettings(dir + "/nnr.ini", QSettings::IniFormat);
}

float rms(const QVector<float>& v)
{
    if (v.isEmpty()) return 0.0f;
    double s = 0.0;
    for (float x : v) s += double(x) * x;
    return float(std::sqrt(s / v.size()));
}

float gainOf(float before, float after)
{
    if (before < 1e-6f) return 1.0f;
    return qBound(0.0f, after / before, 1.0f);
}

// ---- NR2: AetherSDR's SpectralNR (WDSP emnr.c) -----------------------------
class nr2Filter : public rxDenoiser
{
public:
    explicit nr2Filter(int rate) : sampleRate(rate)
    {
        // AetherSDR's geometry, 1024 points / 4x overlap at 24 kHz, scaled to
        // the rate and kept a power of two (the built-in FFT is radix 2).
        const double want = 1024.0 * rate / 24000.0;
        fft = 256;
        while (fft < 8192 && fft * 1.5 < want) fft *= 2;
        nr = std::make_unique<AetherSDR::SpectralNR>(fft, rate, 4, false);
    }
    bool isValid() const override { return nr && !nr->hasPlanFailed(); }
    int rate() const override { return sampleRate; }
    void process(QVector<float>& s) override
    {
        if (s.isEmpty()) return;
        const float before = rms(s);
        out.resize(s.size());
        nr->process(s.constData(), out.data(), s.size());
        s = out;
        lastGain = gainOf(before, rms(s));
    }
    void reset() override { nr->reset(); lastGain = 1.0f; }
    float gain() const override { return lastGain; }
    int delayMs() const override { return int(std::lround(1000.0 * fft / sampleRate)); }
    AetherSDR::SpectralNR* spectral() { return nr.get(); }
private:
    int sampleRate, fft;
    std::unique_ptr<AetherSDR::SpectralNR> nr;
    QVector<float> out;
    float lastGain = 1.0f;
};

#ifdef WFWEB_RN2
// ---- RN2: Xiph RNNoise ------------------------------------------------------
// The model: weights.bin (tools/make-rnnoise-weights.sh) in the server's Qt
// resources, read once; RNNoise keeps pointers into it, so it lives for the
// process.
RNNModel* rnnoiseModel()
{
    static RNNModel* model = nullptr;
    static QByteArray blob;
    static std::once_flag once;
    std::call_once(once, [] {
        QFile f(QStringLiteral(":/rnnoise/weights.bin"));
        if (!f.open(QIODevice::ReadOnly)) {
            qWarning() << "RN2: the RNNoise weights are missing from this build";
            return;
        }
        blob = f.readAll();
        model = rnnoise_model_from_buffer(blob.constData(), int(blob.size()));
    });
    return model;
}

class rn2Filter : public rxDenoiser
{
public:
    static constexpr int kFrame = 480;     // 10 ms at RNNoise's 48 kHz
    static constexpr int kRate = 48000;

    explicit rn2Filter(int rate) : sampleRate(rate)
    {
        if (RNNModel* m = rnnoiseModel()) st = rnnoise_create(m);
        if (rate != kRate) {
            int err = 0;
            up = speex_resampler_init(1, static_cast<spx_uint32_t>(rate), kRate, 5, &err);
            down = speex_resampler_init(1, kRate, static_cast<spx_uint32_t>(rate), 5, &err);
        }
        prime();
    }
    ~rn2Filter() override
    {
        if (st) rnnoise_destroy(st);
        if (up) speex_resampler_destroy(up);
        if (down) speex_resampler_destroy(down);
    }
    bool isValid() const override { return st && (sampleRate == kRate || (up && down)); }
    int rate() const override { return sampleRate; }
    void setDryMix(float d) { dry = qBound(0.0f, d, 0.5f); }

    void process(QVector<float>& s) override
    {
        const int n = s.size();
        if (!n) return;
        const float before = rms(s);
        // to 48 kHz, scaled to RNNoise's 16-bit range
        QVector<float> in48;
        if (up) {
            in48.resize(int(std::ceil(n * double(kRate) / sampleRate)) + 64);
            spx_uint32_t inLen = static_cast<spx_uint32_t>(n), outLen = static_cast<spx_uint32_t>(in48.size());
            speex_resampler_process_float(up, 0, s.constData(), &inLen, in48.data(), &outLen);
            in48.resize(int(outLen));
        } else {
            in48 = s;
        }
        for (float x : in48) acc48.append(x * 32768.0f);
        // whole 10 ms frames
        QVector<float> out48;
        int done = 0;
        float frameIn[kFrame], frameOut[kFrame];
        while (acc48.size() - done >= kFrame) {
            std::copy(acc48.constBegin() + done, acc48.constBegin() + done + kFrame, frameIn);
            rnnoise_process_frame_with_dry_mix(st, frameOut, frameIn, dry);
            for (float x : frameOut) out48.append(x / 32768.0f);
            done += kFrame;
        }
        acc48.remove(0, done);
        // back to the stream's rate, behind the fixed latency
        if (down && !out48.isEmpty()) {
            QVector<float> o(int(std::ceil(out48.size() * double(sampleRate) / kRate)) + 64);
            spx_uint32_t inLen = static_cast<spx_uint32_t>(out48.size()), outLen = static_cast<spx_uint32_t>(o.size());
            speex_resampler_process_float(down, 0, out48.constData(), &inLen, o.data(), &outLen);
            o.resize(int(outLen));
            outFifo += o;
        } else {
            outFifo += out48;
        }
        if (outFifo.size() < n) outFifo.insert(0, n - outFifo.size(), 0.0f);   // never short
        std::copy(outFifo.constBegin(), outFifo.constBegin() + n, s.begin());
        outFifo.remove(0, n);
        lastGain = gainOf(before, rms(s));
    }
    void reset() override
    {
        if (st) { rnnoise_destroy(st); st = nullptr; }
        if (RNNModel* m = rnnoiseModel()) st = rnnoise_create(m);
        if (up) speex_resampler_reset_mem(up);
        if (down) speex_resampler_reset_mem(down);
        acc48.clear();
        prime();
        lastGain = 1.0f;
    }
    float gain() const override { return lastGain; }
    int delayMs() const override { return int(std::lround(1000.0 * latency / sampleRate)); }
private:
    // The output starts one frame plus the resamplers' delay behind, so a
    // block always has its samples ready (fixed latency, whatever the block).
    void prime()
    {
        latency = int(std::ceil(double(kFrame) * sampleRate / kRate));
        if (up) latency += int(speex_resampler_get_output_latency(up)) * sampleRate / kRate + 1;
        if (down) latency += int(speex_resampler_get_input_latency(down)) * sampleRate / kRate + 1;
        outFifo = QVector<float>(latency, 0.0f);
    }
    int sampleRate;
    DenoiseState* st = nullptr;
    SpeexResamplerState* up = nullptr;
    SpeexResamplerState* down = nullptr;
    QVector<float> acc48, outFifo;
    int latency = 0;
    float dry = 0.0f;
    float lastGain = 1.0f;
};

#endif // WFWEB_RN2

void crossfade(QVector<float>& into, const QVector<float>& from)
{
    // `into` becomes from -> into over the block (equal-gain linear ramp).
    const int n = into.size();
    for (int i = 0; i < n; ++i) {
        const float t = n > 1 ? float(i) / (n - 1) : 1.0f;
        into[i] = from[i] * (1.0f - t) + into[i] * t;
    }
}

} // namespace

// ---- settings ---------------------------------------------------------------

void rxNrConfig::load()
{
    std::unique_ptr<QSettings> s(store());
    on = s->value("on", 0).toInt();
    kind = qBound(0, s->value("kind", 0).toInt(), 2);
    strength = s->value("strength", 38).toInt();
    model = s->value("model", 0).toInt();
    alpha = s->value("alpha", 100).toInt();
    knee = s->value("knee", 100).toInt();
    tau = s->value("tau", 200).toInt();
    maxGain = s->value("maxGain", 120).toInt();
    attack = s->value("attack", 0).toInt();
    release = s->value("release", 0).toInt();
    nr2Method = s->value("nr2Method", 2).toInt();
    nr2Npe = s->value("nr2Npe", 0).toInt();
    nr2Ae = s->value("nr2Ae", 1).toInt();
    nr2Max = s->value("nr2Max", 100).toInt();
    nr2Floor = s->value("nr2Floor", 0).toInt();
    nr2Smooth = s->value("nr2Smooth", 85).toInt();
    nr2Qspp = s->value("nr2Qspp", 20).toInt();
    nr2Fill = s->value("nr2Fill", 0).toInt();
    nr2FillLevel = s->value("nr2FillLevel", 15).toInt();
    rn2Dry = s->value("rn2Dry", 0).toInt();
}

void rxNrConfig::save() const
{
    std::unique_ptr<QSettings> s(store());
    s->setValue("on", on);
    s->setValue("kind", kind);
    s->setValue("strength", strength);
    s->setValue("model", model);
    s->setValue("alpha", alpha);
    s->setValue("knee", knee);
    s->setValue("tau", tau);
    s->setValue("maxGain", maxGain);
    s->setValue("attack", attack);
    s->setValue("release", release);
    s->setValue("nr2Method", nr2Method);
    s->setValue("nr2Npe", nr2Npe);
    s->setValue("nr2Ae", nr2Ae);
    s->setValue("nr2Max", nr2Max);
    s->setValue("nr2Floor", nr2Floor);
    s->setValue("nr2Smooth", nr2Smooth);
    s->setValue("nr2Qspp", nr2Qspp);
    s->setValue("nr2Fill", nr2Fill);
    s->setValue("nr2FillLevel", nr2FillLevel);
    s->setValue("rn2Dry", rn2Dry);
}

static const QStringList& nrKeys()
{
    static const QStringList keys = {
        "nnr", "nrKind", "nnrStrength", "nnrModel", "nnrAlpha", "nnrKnee", "nnrTau", "nnrMaxGain",
        "nnrAttack", "nnrRelease", "nnrReset",
        "nr2Method", "nr2Npe", "nr2Ae", "nr2Max", "nr2Floor", "nr2Smooth", "nr2Qspp", "nr2Fill",
        "nr2FillLevel", "nr2Reset", "rn2Dry", "rn2Reset"};
    return keys;
}

bool rxNrConfig::isKey(const QString& key) { return nrKeys().contains(key); }

bool rxNrConfig::set(const QString& key, int v)
{
    const rxNrConfig d;
    if (key == "nnr") on = v ? 1 : 0;
    else if (key == "nrKind") kind = qBound(0, v, 2);
    else if (key == "nnrStrength") strength = qBound(0, v, 100);
    else if (key == "nnrModel") model = qBound(0, v, 1);
    else if (key == "nnrAlpha") alpha = qBound(0, v, 400);
    else if (key == "nnrKnee") knee = qBound(0, v, 400);
    else if (key == "nnrTau") tau = qBound(5, v, 3000);
    else if (key == "nnrMaxGain") maxGain = qBound(0, v, 240);
    else if (key == "nnrAttack") attack = qBound(0, v, 500);
    else if (key == "nnrRelease") release = qBound(0, v, 500);
    else if (key == "nnrReset") {
        strength = d.strength; alpha = d.alpha; knee = d.knee; tau = d.tau;
        maxGain = d.maxGain; attack = d.attack; release = d.release;
    }
    else if (key == "nr2Method") nr2Method = qBound(0, v, 3);
    else if (key == "nr2Npe") nr2Npe = qBound(0, v, 2);
    else if (key == "nr2Ae") nr2Ae = v ? 1 : 0;
    else if (key == "nr2Max") nr2Max = qBound(50, v, 200);
    else if (key == "nr2Floor") nr2Floor = qBound(0, v, 15);
    else if (key == "nr2Smooth") nr2Smooth = qBound(50, v, 98);
    else if (key == "nr2Qspp") nr2Qspp = qBound(5, v, 50);
    else if (key == "nr2Fill") nr2Fill = v ? 1 : 0;
    else if (key == "nr2FillLevel") nr2FillLevel = qBound(0, v, 100);
    else if (key == "nr2Reset") {
        nr2Method = d.nr2Method; nr2Npe = d.nr2Npe; nr2Ae = d.nr2Ae; nr2Max = d.nr2Max;
        nr2Floor = d.nr2Floor; nr2Smooth = d.nr2Smooth; nr2Qspp = d.nr2Qspp;
        nr2Fill = d.nr2Fill; nr2FillLevel = d.nr2FillLevel;
    }
    else if (key == "rn2Dry") rn2Dry = qBound(0, v, 50);
    else if (key == "rn2Reset") rn2Dry = d.rn2Dry;
    else return false;
    return true;
}

void rxNrConfig::toJson(QJsonObject& o) const
{
    o["nnr"] = on;
    o["nrKind"] = kind;
    o["nnrStrength"] = strength;
    o["nnrModel"] = model;
    o["nnrAlpha"] = alpha;
    o["nnrKnee"] = knee;
    o["nnrTau"] = tau;
    o["nnrMaxGain"] = maxGain;
    o["nnrAttack"] = attack;
    o["nnrRelease"] = release;
    o["nr2Method"] = nr2Method;
    o["nr2Npe"] = nr2Npe;
    o["nr2Ae"] = nr2Ae;
    o["nr2Max"] = nr2Max;
    o["nr2Floor"] = nr2Floor;
    o["nr2Smooth"] = nr2Smooth;
    o["nr2Qspp"] = nr2Qspp;
    o["nr2Fill"] = nr2Fill;
    o["nr2FillLevel"] = nr2FillLevel;
    o["rn2Dry"] = rn2Dry;
}

// ---- factory ----------------------------------------------------------------

const char* rxNr::name(int kind)
{
    switch (kind) {
    case rxNrConfig::NR2: return "NR2";
    case rxNrConfig::RN2: return "RN2";
    default: return "NNR";
    }
}

bool rxNr::rateOk(int kind, int rate)
{
    if (kind == rxNrConfig::NNR) return nnrFilter::rateSupported(rate);
#ifndef WFWEB_RN2
    if (kind == rxNrConfig::RN2) return false;
#endif
    return rate >= 8000 && rate <= 96000;
}

std::unique_ptr<rxDenoiser> rxNr::make(int kind, int rate)
{
    if (!rateOk(kind, rate)) return nullptr;
    std::unique_ptr<rxDenoiser> d;
    if (kind == rxNrConfig::NR2) d = std::make_unique<nr2Filter>(rate);
#ifdef WFWEB_RN2
    else if (kind == rxNrConfig::RN2) d = std::make_unique<rn2Filter>(rate);
#endif
    else if (kind == rxNrConfig::RN2) return nullptr;
    else d = std::make_unique<nnrFilter>(rate);
    if (!d->isValid()) return nullptr;
    return d;
}

int rxNr::apply(rxDenoiser* d, int kind, const rxNrConfig& c, int nnrModel)
{
    if (!d) return 0;
    if (kind == rxNrConfig::NNR) {
        auto* f = static_cast<nnrFilter*>(d);
        f->setStrength(c.strength);
        f->setAlpha(c.alpha / 100.0);
        f->setKnee(c.knee / 10.0);
        f->setTau(c.tau / 100.0);
        f->setMaxGain(c.maxGain / 10.0);
        f->setSmooth(c.attack, c.release);
        return f->setModel(nnrModel);
    }
    if (kind == rxNrConfig::NR2) {
        auto* nr = static_cast<nr2Filter*>(d)->spectral();
        nr->setGainMethod(c.nr2Method);
        nr->setNpeMethod(c.nr2Npe);
        nr->setAeFilter(c.nr2Ae != 0);
        nr->setGainMax(c.nr2Max / 100.0f);
        nr->setGainFloor(c.nr2Floor / 100.0f);
        nr->setGainSmooth(c.nr2Smooth / 100.0f);
        nr->setQspp(c.nr2Qspp / 100.0f);
        nr->setPost2Run(c.nr2Fill != 0);
        nr->setPost2Nlevel(c.nr2FillLevel / 100.0f);
        return 0;
    }
#ifdef WFWEB_RN2
    static_cast<rn2Filter*>(d)->setDryMix(c.rn2Dry / 100.0f);
#endif
    return 0;
}

// ---- switching ----------------------------------------------------------------

void rxNrChain::reset()
{
    prev.reset();
    if (parked) parked->reset();
    fadeOut = false;
    if (cur) {
        cur->reset();
        fromRaw = true;
        warmupLeft = cur->rate() * cur->delayMs() / 1000 + cur->rate() / 20;
    }
}

void rxNrChain::run(QVector<float>& s, int kind, int rate, const rxNrConfig& cfg)
{
    cfgCache = cfg;
    failedFlag = false;
    int want = kind >= 0 && rxNr::rateOk(kind, rate) ? kind : -1;
    if (kind >= 0 && want < 0) failedFlag = true;
    if (want != curKind || (cur && rate != curRate)) {
        std::unique_ptr<rxDenoiser> next;
        if (want >= 0 && parked && parkedKind == want && parked->rate() == rate) {
            next = std::move(parked);
            parkedKind = -1;
        } else if (want >= 0) {
            next = rxNr::make(want, rate);
            if (!next) {
                qWarning().noquote() << QString("%1 could not start at %2 Hz").arg(rxNr::name(want)).arg(rate);
                failedFlag = true;
                want = -1;
            }
        }
        if (want != curKind || next) {
            if (next) {
                modelInUse = rxNr::apply(next.get(), want, cfg, nnrModelWanted());
                // the sound so far continues while the new reducer warms up
                prev = std::move(cur);
                prevKind = curKind;
                fromRaw = !prev;
                cur = std::move(next);
                warmupLeft = rate * cur->delayMs() / 1000 + s.size();
                fadeOut = false;
            } else if (cur) {
                prev = std::move(cur);     // off: one crossfade into the plain audio
                prevKind = curKind;
                fadeOut = true;
                warmupLeft = 0;
            }
            curKind = want;
            curRate = rate;
        }
    }

    QVector<float> plain = s, c, p;
    if (cur) { c = s; cur->process(c); }
    if (prev) { p = s; prev->process(p); }

    if (fadeOut) {
        s = plain;
        if (prev) crossfade(s, p);
        parked = std::move(prev);
        parkedKind = prevKind;
        fadeOut = false;
    } else if (cur && warmupLeft > 0) {
        s = (fromRaw || !prev) ? plain : p;
        warmupLeft -= s.size();
    } else if (cur) {
        if (prev || fromRaw) {               // the block after the warm-up
            s = c;
            crossfade(s, prev ? p : plain);
            prev.reset();
            fromRaw = false;
        } else {
            s = c;
        }
    }
}
