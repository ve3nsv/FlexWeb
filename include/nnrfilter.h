// Neural noise reduction for the receive audio (FlexWeb, FUNC > RX DSP > NNR).
//
// WDSP 2.10's NNR (Warren Pratt, NR0V), the block AetherSDR's NnrFilter
// drives, vendored as a subset under resources/wdsp-nnr. One mono instance on
// the server's RX audio, so everyone listening hears the same cleaned audio.
// A speech model: it treats a steady carrier as noise, so flexCommander keeps
// it out of CW and data modes.
#pragma once

#include <QVector>

class nnrFilter
{
public:
    // Rates NNR can run at: a whole multiple of its 16 kHz network rate.
    static bool rateSupported(int rate) { return rate > 0 && rate % 16000 == 0; }

    explicit nnrFilter(int rate);
    ~nnrFilter();
    nnrFilter(const nnrFilter&) = delete;
    nnrFilter& operator=(const nnrFilter&) = delete;

    bool isValid() const { return nnr != nullptr; }
    int rate() const { return sampleRate; }

    // In place, any length; the output runs one block (256 samples) late.
    void process(QVector<float>& samples);
    void reset();

    // The controls (AetherSDR's ranges): strength 0..100 maps to a mask floor
    // of -10..-50 dB; model 0 Standard, 1 Premium.
    void setStrength(int strength);
    int setModel(int model);           // returns the slot actually in use
    void setAlpha(double v);           // 0..4
    void setKnee(double dB);           // 0..40
    void setTau(double s);             // 0.05..30
    void setMaxGain(double dB);        // 0..24
    void setSmooth(double attackMs, double releaseMs);   // 0..500

    // post / pre level of the last process() call, 0..1 (AetherSDR's NR GAIN)
    float gain() const { return lastGain; }
    int delayMs() const;

private:
    static constexpr int kBlock = 256;
    int sampleRate = 0;
    void* nnr = nullptr;
    QVector<double> blockIn, blockOut;    // interleaved I/Q, Q silent
    QVector<float> acc, outQ;
    float lastGain = 1.0f;
};
