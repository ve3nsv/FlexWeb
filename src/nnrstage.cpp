#include "nnrstage.h"
#include "nnrfilter.h"

#include <QDebug>
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

float nnrStage::gain() const { return f ? f->gain() : 1.0f; }

QJsonObject nnrStage::state() const
{
    QJsonObject st;
    st["nnr"] = cfg.on;
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
    st["nnrDelayMs"] = f ? f->delayMs() : 0;
    return st;
}
