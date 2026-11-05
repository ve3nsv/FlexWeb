// Client-side receive noise reduction (punch list 57, 59, 60): one of NNR
// (WDSP neural), NR2 (WDSP spectral MMSE, AetherSDR's SpectralNR) or RN2
// (Xiph RNNoise) on the RX audio, shared by the Flex path (flexRxDsp, its own
// thread) and every other radio (nnrStage in the web server). One at a time,
// as AetherSDR: `on` switches the chosen `kind` on or off.
//
// Pure Qt5Core: tests/rxnr_test.cpp links it alone.
#pragma once

#include <QJsonObject>
#include <QString>
#include <QVector>
#include <memory>

// What every reducer offers its host. In place, any block length; the output
// runs delayMs() late (a fixed latency, so raw audio can be aligned with it).
class rxDenoiser
{
public:
    virtual ~rxDenoiser() = default;
    virtual bool isValid() const = 0;
    virtual int rate() const = 0;
    virtual void process(QVector<float>& samples) = 0;
    virtual void reset() = 0;
    virtual float gain() const = 0;       // post / pre level of the last block, 0..1
    virtual int delayMs() const = 0;
};

// Settings, persisted in QSettings("wfweb", "nnr") (WFWEB_CONFIG_DIR: nnr.ini)
// and sent to the page as the nnr* / nr2* / rn2* keys. Fixed point for the page.
struct rxNrConfig
{
    enum Kind { NNR = 0, NR2 = 1, RN2 = 2 };
    int on = 0, kind = NNR;
    // NNR (AetherSDR's ranges): strength 0..100, model 0 Standard / 1 Premium,
    // alpha x100, knee x10 dB, tau x100 s, maxGain x10 dB, attack / release ms
    int strength = 38, model = 0, alpha = 100, knee = 100, tau = 200, maxGain = 120, attack = 0, release = 0;
    // NR2 (AetherSDR's Nr2SettingsModel and its sliders): gain method 0 Linear
    // 1 Log 2 Gamma 3 Trained, noise estimator 0 OSMS 1 MMSE 2 NSTAT, AE filter,
    // reduction (gainMax) 50..200 x100, naturalness (gainFloor) 0..15 x100,
    // smoothing 50..98 x100, voice threshold (qspp) 5..50 x100, noise fill
    // (post2) on / level 0..100 x100
    int nr2Method = 2, nr2Npe = 0, nr2Ae = 1, nr2Max = 100, nr2Floor = 0, nr2Smooth = 85, nr2Qspp = 20,
        nr2Fill = 0, nr2FillLevel = 15;
    // RN2: noise floor (dry mix) 0..50 %
    int rn2Dry = 0;

    void load();
    void save() const;
    // A page key; false if it is not one of ours. nnrReset / nr2Reset /
    // rn2Reset restore that reducer's defaults (on / kind / NNR model stay).
    bool set(const QString& key, int value);
    void toJson(QJsonObject& o) const;
    static bool isKey(const QString& key);
};

namespace rxNr {
const char* name(int kind);                         // "NNR", "NR2", "RN2"
bool rateOk(int kind, int rate);                    // NNR: 16 kHz multiples; NR2 / RN2: 8..96 kHz
std::unique_ptr<rxDenoiser> make(int kind, int rate);
// The settings for this kind; returns the NNR model actually in use (0 otherwise).
int apply(rxDenoiser* d, int kind, const rxNrConfig& cfg, int nnrModel);
}

// The reducer a host runs, and its changes without a gap or a click: a new
// reducer (another kind, or on) runs unheard for its latency while the old
// sound continues, then one block crossfades into it; off crossfades the last
// processed block into the plain audio. Hosts that pass audio through on
// overload (flexRxDsp) do that around run().
class rxNrChain
{
public:
    // kind < 0: off. Processes `samples` in place.
    void run(QVector<float>& samples, int kind, int rate, const rxNrConfig& cfg);
    void applyConfig(const rxNrConfig& cfg) { cfgCache = cfg; if (cur) modelInUse = rxNr::apply(cur.get(), curKind, cfg, nnrModelWanted()); }
    // The NNR overload ladder (flexRxDsp) runs Standard in place of Premium.
    void setNnrStepDown(bool down) { stepDown = down; if (cur) modelInUse = rxNr::apply(cur.get(), curKind, cfgCache, nnrModelWanted()); }
    void reset();                    // stream restart: state cleared, the next audio warms up unheard
    rxDenoiser* current() const { return cur.get(); }
    int kind() const { return cur ? curKind : -1; }
    int nnrModelInUse() const { return modelInUse; }
    bool failed() const { return failedFlag; }    // the wanted kind could not start at this rate
private:
    int nnrModelWanted() const { return stepDown ? 0 : cfgCache.model; }
    std::unique_ptr<rxDenoiser> cur, prev;   // prev: the sound until cur has warmed up
    // The last reducer switched off (TX, a CW / data mode, off): taken back
    // when the same kind and rate return, so NR2 keeps its learned noise
    // floor and nothing is rebuilt per over. Its stale buffers play unheard
    // during the warm-up, as after reset().
    std::unique_ptr<rxDenoiser> parked;
    int parkedKind = -1;
    int curKind = -1, prevKind = -1, curRate = 0, wantKind = -1;
    int warmupLeft = 0;              // samples cur still runs unheard
    bool fromRaw = false;            // the sound before cur is the plain audio
    bool fadeOut = false;            // off: crossfade prev into the plain audio, then drop it
    bool stepDown = false, failedFlag = false;
    int modelInUse = 0;
    rxNrConfig cfgCache;
};
