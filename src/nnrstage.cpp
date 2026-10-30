#include "nnrstage.h"
#include "nnrfilter.h"
#include "SpectralNR.h"

#include <QDebug>
#include <cmath>
#include <QSettings>
#include <QVector>
#include <QtGlobal>

namespace {
QSettings* store()
{
    // the Flex's store (flexcommander.cpp nnrStore), so both share settings
    const QString dir = qEnvironmentVariable("WFWEB_CONFIG_DIR");
    return dir.isEmpty() ? new QSettings("wfweb", "nnr") : new QSettings(dir + "/nnr.ini", QSettings::IniFormat);
}
}

nnrStage::nnrStage() { load(); }
nnrStage::~nnrStage() = default;

void nnrStage::load()
{
    std::unique_ptr<QSettings> s(store());
    cfg.on = s->value("on", 0).toInt();
    cfg.method = s->value("method", cfg.method).toInt();
    cfg.nr2Reduction = s->value("nr2Reduction", cfg.nr2Reduction).toInt();
    cfg.nr2Naturalness = s->value("nr2Naturalness", cfg.nr2Naturalness).toInt();
    cfg.nr2Smoothing = s->value("nr2Smoothing", cfg.nr2Smoothing).toInt();
    cfg.nr2Threshold = s->value("nr2Threshold", cfg.nr2Threshold).toInt();
    cfg.nr2Gain = s->value("nr2Gain", cfg.nr2Gain).toInt();
    cfg.nr2Noise = s->value("nr2Noise", cfg.nr2Noise).toInt();
    cfg.nr2Ae = s->value("nr2Ae", cfg.nr2Ae).toInt();
    cfg.strength = s->value("strength", 38).toInt();
    cfg.model = s->value("model", 0).toInt();
    cfg.alpha = s->value("alpha", 100).toInt();
    cfg.knee = s->value("knee", 100).toInt();
    cfg.tau = s->value("tau", 200).toInt();
    cfg.maxGain = s->value("maxGain", 120).toInt();
    cfg.attack = s->value("attack", 0).toInt();
    cfg.release = s->value("release", 0).toInt();
}

void nnrStage::save()
{
    std::unique_ptr<QSettings> s(store());
    s->setValue("on", cfg.on);
    s->setValue("method", cfg.method);
    s->setValue("nr2Reduction", cfg.nr2Reduction);
    s->setValue("nr2Naturalness", cfg.nr2Naturalness);
    s->setValue("nr2Smoothing", cfg.nr2Smoothing);
    s->setValue("nr2Threshold", cfg.nr2Threshold);
    s->setValue("nr2Gain", cfg.nr2Gain);
    s->setValue("nr2Noise", cfg.nr2Noise);
    s->setValue("nr2Ae", cfg.nr2Ae);
    s->setValue("strength", cfg.strength);
    s->setValue("model", cfg.model);
    s->setValue("alpha", cfg.alpha);
    s->setValue("knee", cfg.knee);
    s->setValue("tau", cfg.tau);
    s->setValue("maxGain", cfg.maxGain);
    s->setValue("attack", cfg.attack);
    s->setValue("release", cfg.release);
}

void nnrStage::apply()
{
    if (nr2) {
        nr2->setGainMax(cfg.nr2Reduction / 100.0f); nr2->setGainFloor(cfg.nr2Naturalness / 100.0f);
        nr2->setGainSmooth(cfg.nr2Smoothing / 100.0f); nr2->setQspp(cfg.nr2Threshold / 100.0f);
        nr2->setGainMethod(cfg.nr2Gain); nr2->setNpeMethod(cfg.nr2Noise); nr2->setAeFilter(cfg.nr2Ae != 0);
    }
    if (!f) return;
    f->setStrength(cfg.strength);
    f->setAlpha(cfg.alpha / 100.0);
    f->setKnee(cfg.knee / 10.0);
    f->setTau(cfg.tau / 100.0);
    f->setMaxGain(cfg.maxGain / 10.0);
    f->setSmooth(cfg.attack, cfg.release);
    modelInUse = f->setModel(cfg.model);
}

bool nnrStage::set(const QString& key, int value)
{
    if (key == "nnr") cfg.on = value ? 1 : 0;
    else if (key == "nrMethod") {
        const int method = qBound(0, value, 1);
        if (method != cfg.method) { if (nr2) nr2->reset(); if (f) f->reset(); wasActive = false; }
        cfg.method = method;
    }
    else if (key == "nr2Reduction") cfg.nr2Reduction = qBound(50, value, 200);
    else if (key == "nr2Naturalness") cfg.nr2Naturalness = qBound(0, value, 15);
    else if (key == "nr2Smoothing") cfg.nr2Smoothing = qBound(50, value, 98);
    else if (key == "nr2Threshold") cfg.nr2Threshold = qBound(5, value, 50);
    else if (key == "nr2Gain") cfg.nr2Gain = qBound(0, value, 3);
    else if (key == "nr2Noise") cfg.nr2Noise = qBound(0, value, 2);
    else if (key == "nr2Ae") cfg.nr2Ae = qBound(0, value, 1);
    else if (key == "nnrStrength") cfg.strength = qBound(0, value, 100);
    else if (key == "nnrModel") cfg.model = qBound(0, value, 1);
    else if (key == "nnrAlpha") cfg.alpha = qBound(0, value, 400);
    else if (key == "nnrKnee") cfg.knee = qBound(0, value, 400);
    else if (key == "nnrTau") cfg.tau = qBound(5, value, 3000);
    else if (key == "nnrMaxGain") cfg.maxGain = qBound(0, value, 240);
    else if (key == "nnrAttack") cfg.attack = qBound(0, value, 500);
    else if (key == "nnrRelease") cfg.release = qBound(0, value, 500);
    else if (key == "nnrReset") { const int on = cfg.on, model = cfg.model; cfg = settings(); cfg.on = on; cfg.model = model; }
    else return false;
    apply();
    save();
    return true;
}

bool nnrStage::process(QByteArray& pcm, int rate, bool allowed)
{
    const bool want = cfg.on && allowed && nnrFilter::rateSupported(rate);
    if (cfg.method == 1) {
        if (want) {
            if (!nr2 || nr2Rate != rate) { nr2.reset(new AetherSDR::SpectralNR(2048, rate, 4)); nr2Rate = rate; apply(); }
            if (!wasActive) nr2->resetTransient();
            const int n = pcm.size() / 2; QVector<float> in(n), out(n);
            qint16 *samples = reinterpret_cast<qint16*>(pcm.data());
            for (int i=0;i<n;++i) in[i] = samples[i] / 32768.0f;
            nr2->process(in.constData(), out.data(), n);
            double before=0, after=0; for(int i=0;i<n;++i) { before+=double(in[i])*in[i]; after+=double(out[i])*out[i]; }
            nr2Gain = before > 1e-12 ? float(qBound(0.0, std::sqrt(after/before), 1.0)) : 1.0f;
            for (int i=0;i<n;++i) samples[i] = qint16(qBound(-32768, int(out[i]*32767.0f), 32767));
        }
        const bool changed = want != wasActive; wasActive = want; return changed;
    }
    if (want) {
        if (!f || f->rate() != rate) {
            f.reset(new nnrFilter(rate));
            if (!f->isValid()) {
                qWarning() << "NNR could not start at" << rate << "Hz";
                f.reset();
                cfg.on = 0;
            } else {
                apply();
                qInfo() << "NNR on (receive audio), model" << (modelInUse ? "Premium" : "Standard")
                        << "delay" << f->delayMs() << "ms";
            }
        }
        if (f) {
            const int n = pcm.size() / 2;
            qint16* s = reinterpret_cast<qint16*>(pcm.data());
            QVector<float> v(n);
            for (int i = 0; i < n; ++i) v[i] = s[i] / 32768.0f;
            f->process(v);
            for (int i = 0; i < n; ++i) s[i] = qint16(qBound(-32768, int(v[i] * 32767.0f), 32767));
        }
    } else if (f) {
        f->reset();
    }
    const bool nowActive = want && f;
    if (nowActive == wasActive) return false;
    wasActive = nowActive;
    return true;
}

float nnrStage::gain() const { return cfg.method == 1 ? nr2Gain : (f ? f->gain() : 1.0f); }

QJsonObject nnrStage::state() const
{
    QJsonObject st;
    st["nnr"] = cfg.on;
    st["nrMethod"] = cfg.method;
    st["nr2Reduction"] = cfg.nr2Reduction;
    st["nr2Naturalness"] = cfg.nr2Naturalness;
    st["nr2Smoothing"] = cfg.nr2Smoothing;
    st["nr2Threshold"] = cfg.nr2Threshold;
    st["nr2Gain"] = cfg.nr2Gain;
    st["nr2Noise"] = cfg.nr2Noise;
    st["nr2Ae"] = cfg.nr2Ae;
    st["nnrStrength"] = cfg.strength;
    st["nnrModel"] = cfg.model;
    st["nnrModelInUse"] = f ? modelInUse : cfg.model;
    st["nnrAlpha"] = cfg.alpha;
    st["nnrKnee"] = cfg.knee;
    st["nnrTau"] = cfg.tau;
    st["nnrMaxGain"] = cfg.maxGain;
    st["nnrAttack"] = cfg.attack;
    st["nnrRelease"] = cfg.release;
    st["nnrActive"] = wasActive;
    st["nnrDelayMs"] = cfg.method == 1 ? (nr2Rate ? 2048000 / nr2Rate : 0) : (f ? f->delayMs() : 0);
    return st;
}
