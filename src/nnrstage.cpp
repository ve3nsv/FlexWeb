// See include/nnrstage.h.
#include "nnrstage.h"

#include <QDebug>
#include <QVector>
#include <QtGlobal>

nnrStage::nnrStage() { cfg.load(); }
nnrStage::~nnrStage() = default;

bool nnrStage::set(const QString& key, int value)
{
    if (!cfg.set(key, value)) return false;
    chain.applyConfig(cfg);
    cfg.save();
    return true;
}

bool nnrStage::process(QByteArray& pcm, int rate, bool allowed)
{
    const int kind = cfg.on && allowed && rxNr::rateOk(cfg.kind, rate) ? cfg.kind : -1;
    // Off and already faded out: nothing to do.
    if (kind < 0 && !chain.current() && !wasActive) return false;
    const int n = pcm.size() / 2;
    qint16* s = reinterpret_cast<qint16*>(pcm.data());
    QVector<float> v(n);
    for (int i = 0; i < n; ++i) v[i] = s[i] / 32768.0f;
    const int before = chain.kind();
    chain.run(v, kind, rate, cfg);
    for (int i = 0; i < n; ++i) s[i] = qint16(qBound(-32768, int(v[i] * 32767.0f), 32767));
    if (chain.failed()) {
        qWarning().noquote() << QString("%1 could not start at %2 Hz").arg(rxNr::name(cfg.kind)).arg(rate);
        cfg.on = 0;                 // and the page is told (state() below)
        wasActive = chain.current() != nullptr;
        return true;
    } else if (chain.current() && chain.kind() != before) {
        qInfo().noquote() << QString("%1 on (receive audio)%2, delay %3 ms")
            .arg(rxNr::name(chain.kind()))
            .arg(chain.kind() == rxNrConfig::NNR ? (chain.nnrModelInUse() ? ", model Premium" : ", model Standard") : "")
            .arg(chain.current()->delayMs());
    }
    const bool nowActive = chain.current() != nullptr;
    if (nowActive == wasActive) return false;
    wasActive = nowActive;
    return true;
}

float nnrStage::gain() const { return chain.current() ? chain.current()->gain() : 1.0f; }

QJsonObject nnrStage::state() const
{
    QJsonObject st;
    cfg.toJson(st);
    st["nnrModelInUse"] = chain.current() && chain.kind() == rxNrConfig::NNR ? chain.nnrModelInUse() : cfg.model;
    st["nnrActive"] = wasActive;
    st["nnrDelayMs"] = chain.current() ? chain.current()->delayMs() : 0;
    return st;
}
