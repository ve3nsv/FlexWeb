#include "radioreach.h"

#include <QHostAddress>
#include <QHostInfo>
#include <QPointer>
#include <QRandomGenerator>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QtEndian>
#include <QDebug>

static const int kProbeMs = 3000;

RadioReach::RadioReach(QObject *parent) : QObject(parent)
{
    timer_.setInterval(30000);
    connect(&timer_, &QTimer::timeout, this, [this]() { probeAll(); });
}

void RadioReach::setTargets(const QVector<Target> &targets)
{
    bool same = targets.size() == targets_.size();
    for (int i = 0; same && i < targets.size(); ++i)
        same = targets[i].name == targets_[i].name && targets[i].type == targets_[i].type
            && targets[i].host == targets_[i].host && targets[i].port == targets_[i].port;
    if (same) return;
    targets_ = targets;
    state_ = QVector<QString>(targets.size());
    why_ = QVector<QString>(targets.size());
    logged_ = QVector<QString>(targets.size());
    gen_ = QVector<quint64>(targets.size(), 0);
    if (active_) QTimer::singleShot(500, this, [this]() { if (active_) probeAll(); });
}

void RadioReach::setActive(bool on)
{
    if (on == active_) return;
    active_ = on;
    if (on) {
        timer_.start();
        QTimer::singleShot(500, this, [this]() { if (active_) probeAll(); });
        return;
    }
    timer_.stop();
    for (int i = 0; i < gen_.size(); ++i) { gen_[i] = 0; state_[i].clear(); why_[i].clear(); }
}

void RadioReach::probeAll()
{
    if (!active_) return;
    for (int i = 0; i < targets_.size(); ++i) probe(i);
}

void RadioReach::probe(int i)
{
    const Target &t = targets_[i];
    const quint64 gen = nextGen_++;
    gen_[i] = gen;
    if (linkUp && linkUp(i)) { result(i, gen, QStringLiteral("up"), QString()); return; }
    if (t.type == QLatin1String("icomlan") && !t.host.isEmpty()) probeIcom(i, gen);
    else if (t.type == QLatin1String("flex") && !t.host.isEmpty()
             && t.host.compare(QLatin1String("auto"), Qt::CaseInsensitive) != 0) probeFlex(i, gen);
    // smartlink / flex "auto": nothing to try, the state stays "".
}

void RadioReach::withAddress(int i, quint64 gen, std::function<void(const QHostAddress &)> fn)
{
    const QHostAddress direct(targets_[i].host);
    if (!direct.isNull()) { fn(direct); return; }
    QPointer<RadioReach> self(this);
    QHostInfo::lookupHost(targets_[i].host, this, [self, i, gen, fn](const QHostInfo &info) {
        if (!self || i >= self->gen_.size() || self->gen_[i] != gen) return;
        for (const QHostAddress &a : info.addresses())
            if (a.protocol() == QAbstractSocket::IPv4Protocol) { fn(a); return; }
        if (!info.addresses().isEmpty()) { fn(info.addresses().first()); return; }
        self->result(i, gen, QStringLiteral("down"), QStringLiteral("no address"));
    });
}

void RadioReach::probeIcom(int i, quint64 gen)
{
    const quint16 port = targets_[i].port ? targets_[i].port : 50001;
    withAddress(i, gen, [this, i, gen, port](const QHostAddress &addr) {
        auto *sock = new QUdpSocket(this);
        if (!sock->bind(QHostAddress(addr.protocol() == QAbstractSocket::IPv6Protocol ? QHostAddress::AnyIPv6 : QHostAddress::AnyIPv4), 0)) {
            sock->deleteLater();
            return;
        }
        // control_packet: len, type, seq, sentid, rcvdid (little-endian)
        QByteArray pkt(16, '\0');
        qToLittleEndian<quint32>(16, pkt.data());
        qToLittleEndian<quint16>(0x03, pkt.data() + 4);
        qToLittleEndian<quint32>(QRandomGenerator::global()->generate() | 1u, pkt.data() + 8);
        auto done = std::make_shared<bool>(false);
        connect(sock, &QUdpSocket::readyRead, this, [this, sock, i, gen, done]() {
            while (sock->hasPendingDatagrams()) {
                QByteArray d(int(sock->pendingDatagramSize()), '\0');
                sock->readDatagram(d.data(), d.size());
                if (*done || d.size() < 16 || qFromLittleEndian<quint16>(d.constData() + 4) != 0x04) continue;
                *done = true;
                result(i, gen, QStringLiteral("up"), QString());
                sock->deleteLater();
            }
        });
        for (int k = 0; k < 3; ++k)
            QTimer::singleShot(k * 600, sock, [sock, pkt, addr, port, done]() {
                if (!*done) sock->writeDatagram(pkt, addr, port);
            });
        QTimer::singleShot(kProbeMs, sock, [this, sock, i, gen, done]() {
            if (*done) return;
            *done = true;
            result(i, gen, QStringLiteral("down"), QStringLiteral("no answer"));
            sock->deleteLater();
        });
    });
}

void RadioReach::probeFlex(int i, quint64 gen)
{
    const quint16 port = targets_[i].port ? targets_[i].port : 4992;
    withAddress(i, gen, [this, i, gen, port](const QHostAddress &addr) {
        auto *sock = new QTcpSocket(this);
        auto done = std::make_shared<bool>(false);
        auto finish = [this, sock, i, gen, done](const QString &state, const QString &why) {
            if (*done) return;
            *done = true;
            result(i, gen, state, why);
            sock->abort();
            sock->deleteLater();
        };
        connect(sock, &QTcpSocket::connected, this, [finish]() { finish(QStringLiteral("up"), QString()); });
        auto onError = [finish](QAbstractSocket::SocketError e) {
            finish(QStringLiteral("down"), e == QAbstractSocket::ConnectionRefusedError
                       ? QStringLiteral("refused") : QStringLiteral("no answer"));
        };
#if QT_VERSION < QT_VERSION_CHECK(5, 15, 0)
        connect(sock, QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::error), this, onError);
#else
        connect(sock, &QAbstractSocket::errorOccurred, this, onError);
#endif
        QTimer::singleShot(kProbeMs, sock, [finish]() { finish(QStringLiteral("down"), QStringLiteral("no answer")); });
        sock->connectToHost(addr, port);
    });
}

void RadioReach::result(int i, quint64 gen, const QString &state, const QString &why)
{
    if (i >= gen_.size() || gen_[i] != gen) return;
    if (state_[i] == state && why_[i] == why) return;
    state_[i] = state;
    why_[i] = why;
    // The log gets changes only, not every browser's first check.
    const Target &t = targets_[i];
    const QString key = state + QLatin1Char('/') + why;
    if (logged_[i] != key) {
        if (state == QLatin1String("down"))
            qWarning().noquote() << QString("Radio check: \"%1\" (%2) is not answering (%3)").arg(t.name, t.host, why);
        else if (!logged_[i].isEmpty())
            qInfo().noquote() << QString("Radio check: \"%1\" (%2) is answering again").arg(t.name, t.host);
        else
            qInfo().noquote() << QString("Radio check: \"%1\" (%2) is answering").arg(t.name, t.host);
        logged_[i] = key;
    }
    if (changed) changed();
}
