#include "flexcommander.h"
#include <QJsonArray>

#include <QDebug>
#include <QSettings>
#include <QUuid>
#include <QtEndian>
#include <cmath>
#include <cstdlib>
#include <algorithm>

#include "rigidentities.h"
#include "logcategories.h"
#include "resampler/speex_resampler.h"

// FlexRadio support for wfweb. See include/flexcommander.h for the overview.
//
// Protocol knowledge (connect sequence, stream setup, status keys, meter
// scaling, filter placement) is taken from AetherSDR
// (https://github.com/aethersdr/AetherSDR), GPLv3.

using namespace flexProtocol;

namespace {

// wfweb mode register <-> SmartSDR mode name. The registers are this
// commander's own numbering; rigs/FLEX-6000.rig lists the same ones.
struct flexModeMap { quint8 reg; const char* name; };
const flexModeMap kModes[] = {
    {1, "LSB"}, {2, "USB"}, {3, "CW"}, {4, "AM"}, {5, "SAM"}, {6, "FM"},
    {7, "NFM"}, {8, "DIGU"}, {9, "DIGL"}, {10, "RTTY"}, {11, "DFM"},
};

QString normalizedId(const QString& id)
{
    QString t = id.trimmed();
    if (t.startsWith(QLatin1String("0x"), Qt::CaseInsensitive))
        t = t.mid(2);
    bool ok = false;
    const quint32 v = t.toUInt(&ok, 16);
    return ok ? QString("0x%1").arg(v, 8, 16, QChar('0')).toUpper().replace("0X", "0x") : QString();
}

quint32 parseStreamId(const QString& body)
{
    const QMap<QString, QString> kvs = parseKVs(body);
    QString t = kvs.contains("stream") ? kvs.value("stream")
              : kvs.contains("id") ? kvs.value("id") : body.trimmed();
    if (t.startsWith(QLatin1String("0x"), Qt::CaseInsensitive))
        t = t.mid(2);
    bool ok = false;
    const quint32 v = t.toUInt(&ok, 16);
    return ok ? v : 0;
}

inline int to100(int v255) { return qBound(0, int(std::lround(v255 * 100.0 / 255.0)), 100); }
inline int to255(int v100) { return qBound(0, int(std::lround(v100 * 255.0 / 100.0)), 255); }

bool truthy(const QString& v) { return v == QLatin1String("1") || v.compare("true", Qt::CaseInsensitive) == 0; }

} // namespace

flexCommander::flexCommander(rigCommander* parent) : rigCommander(parent)
{
    qInfo(logRig()) << "creating instance of flexCommander()";
}

flexCommander::flexCommander(quint8 guid[GUIDLEN], rigCommander* parent) : rigCommander(parent)
{
    qInfo(logRig()) << "creating instance of flexCommander() with GUID";
    memcpy(this->guid, guid, GUIDLEN);
}

flexCommander::~flexCommander()
{
    qInfo(logRig()) << "closing instance of flexCommander()";
    closeComm();
    emit requestRadioSelection(QList<radio_cap_packet>());
    queue->setRigCaps(Q_NULLPTR);
    if (rxResampler) speex_resampler_destroy(rxResampler);
    if (txResampler) speex_resampler_destroy(txResampler);
}

void flexCommander::process()
{
    // Thread entry; all work happens in commSetup() and the socket slots.
}

void flexCommander::commSetup(rigTypedef rigList, quint16 rigCivAddr, QString rigSerialPort, quint32 rigBaudRate, QString vsp, quint16 tcp, quint8 wf)
{
    Q_UNUSED(rigList) Q_UNUSED(rigCivAddr) Q_UNUSED(rigBaudRate) Q_UNUSED(vsp) Q_UNUSED(tcp) Q_UNUSED(wf)
    qWarning(logRig()) << "FlexRadio has no serial control port; connect over LAN instead (--lan <ip>)."
                       << "Requested port:" << rigSerialPort;
    emit havePortError(errorType(true, rigSerialPort,
        "FlexRadio is controlled over the network.\nUse --lan <radio IP> (or --lan auto) with --manufacturer 3."));
}

void flexCommander::commSetup(rigTypedef rigList, quint16 rigCivAddr, udpPreferences prefs, audioSetup rxSetup, audioSetup txSetup, QString vsp, quint16 tcpPort)
{
    Q_UNUSED(rigCivAddr) Q_UNUSED(vsp) Q_UNUSED(tcpPort)
    this->rigList = rigList;
    this->prefs = prefs;
    this->rxSetup = rxSetup;
    this->txSetup = txSetup;
    usingNativeLAN = true;
    closing = false;

    // A reconnect from the web UI calls commSetup() again on the same object.
    closeComm();
    closing = false;

    const QString host = prefs.ipAddress.trimmed();
    useDiscovery = host.isEmpty() || host.compare("auto", Qt::CaseInsensitive) == 0;
    radioAddress = useDiscovery ? QHostAddress() : QHostAddress(host);
    if (!useDiscovery && radioAddress.isNull()) {
        // Not a literal address: let QTcpSocket resolve the name.
        radioAddress = QHostAddress();
    }
    // The SmartSDR API port is fixed. wfweb's LAN control port defaults to
    // the Icom 50001; honour it only when someone set it on purpose.
    radioPort = (prefs.controlLANPort != 0 && prefs.controlLANPort != 50001) ? prefs.controlLANPort : 4992;
    // Same for the radio's VITA-49 port (DAX TX goes there) and --lan-audio.
    radioUdpPort = (prefs.audioLANPort != 0 && prefs.audioLANPort != 50003) ? prefs.audioLANPort : 4991;

    // RX audio resampler: the radio's 24 kHz -> the rate the web audio path
    // was set up for (setupAudio(codec, rxSetup.sampleRate)).
    const quint32 outRate = rxSetup.sampleRate ? rxSetup.sampleRate : 48000;
    const quint32 inRate = txSetup.sampleRate ? txSetup.sampleRate : outRate;
    int err = 0;
    if (rxResampler) speex_resampler_destroy(rxResampler);
    if (txResampler) speex_resampler_destroy(txResampler);
    rxResampler = speex_resampler_init(1, DAX_RATE, outRate, 5, &err);
    txResampler = speex_resampler_init(1, inRate, DAX_RATE, 5, &err);
    qInfo(logRig()) << "Flex audio: radio" << DAX_RATE << "Hz <-> web" << outRate << "/" << inRate << "Hz";

    // Browser mic audio arrives on rigCommander::receiveWebTxAudio, which
    // re-emits it as sendWebTxAudio for the transport to pick up.
    disconnect(this, &rigCommander::sendWebTxAudio, this, &flexCommander::webTxAudio);
    connect(this, &rigCommander::sendWebTxAudio, this, &flexCommander::webTxAudio);

    // Commands from the queue. UniqueConnection: a reconnect calls
    // commSetup() again and must not deliver every command twice.
    connect(queue, SIGNAL(haveCommand(funcs,QVariant,uchar)), this, SLOT(receiveCommand(funcs,QVariant,uchar)), Qt::UniqueConnection);

    udp = new QUdpSocket(this);
    if (!udp->bind(QHostAddress(QHostAddress::AnyIPv4), 0)) {
        qWarning(logRig()) << "Flex: cannot bind UDP socket:" << udp->errorString();
    }
    connect(udp, &QUdpSocket::readyRead, this, &flexCommander::udpReadyRead);

    tcp = new QTcpSocket(this);
    tcp->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    connect(tcp, &QTcpSocket::connected, this, &flexCommander::tcpConnected);
    connect(tcp, &QTcpSocket::disconnected, this, &flexCommander::tcpDisconnected);
    connect(tcp, &QTcpSocket::readyRead, this, &flexCommander::tcpReadyRead);
#if (QT_VERSION < QT_VERSION_CHECK(5,15,0))
    connect(tcp, QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::error), this, &flexCommander::tcpError);
#else
    connect(tcp, &QAbstractSocket::errorOccurred, this, &flexCommander::tcpError);
#endif

    pingTimer = new QTimer(this);
    pingTimer->setInterval(1000);
    connect(pingTimer, &QTimer::timeout, this, &flexCommander::pingTick);

    reconnectTimer = new QTimer(this);
    reconnectTimer->setSingleShot(true);
    reconnectTimer->setInterval(3000);
    connect(reconnectTimer, &QTimer::timeout, this, &flexCommander::reconnectTick);

    startConnect();
}

void flexCommander::startConnect()
{
    if (closing)
        return;
    if (useDiscovery && radioAddress.isNull()) {
        if (discovery == nullptr) {
            discovery = new QUdpSocket(this);
            if (!discovery->bind(QHostAddress(QHostAddress::AnyIPv4), 4992, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
                qWarning(logRig()) << "Flex: cannot listen for discovery on UDP 4992:" << discovery->errorString();
                emit havePortError(errorType(true, "UDP 4992", "Cannot listen for FlexRadio discovery.\nGive the radio's IP address with --lan <ip>."));
                return;
            }
            connect(discovery, &QUdpSocket::readyRead, this, &flexCommander::discoveryReadyRead);
        }
        setStatus("Flex: waiting for a radio discovery broadcast on UDP 4992", false);
        return;
    }
    const QString host = radioAddress.isNull() ? prefs.ipAddress.trimmed() : radioAddress.toString();
    setStatus(QString("Flex: connecting to %1:%2").arg(host).arg(radioPort), false);
    tcp->abort();
    tcp->connectToHost(host, radioPort);
}

void flexCommander::discoveryReadyRead()
{
    while (discovery && discovery->hasPendingDatagrams()) {
        QHostAddress sender;
        QByteArray d;
        d.resize(int(discovery->pendingDatagramSize()));
        discovery->readDatagram(d.data(), d.size(), &sender);
        const QMap<QString, QString> kv = parseDiscovery(d);
        if (!kv.contains("serial") && !kv.contains("model"))
            continue;
        QHostAddress addr(kv.value("ip"));
        if (addr.isNull())
            addr = sender;
        if (!radioAddress.isNull())
            continue;
        radioAddress = addr;
        if (kv.contains("port"))
            radioPort = kv.value("port").toUShort();
        qInfo(logRig()).noquote() << QString("Flex: discovered %1 \"%2\" (serial %3, firmware %4) at %5")
            .arg(kv.value("model"), kv.value("nickname", kv.value("name")), kv.value("serial"),
                 kv.value("version"), addr.toString());
        discovery->close();
        discovery->deleteLater();
        discovery = nullptr;
        startConnect();
        return;
    }
}

void flexCommander::closeComm()
{
    closing = true;
    if (reconnectTimer) reconnectTimer->stop();
    if (pingTimer) pingTimer->stop();

    if (tcp != nullptr) {
        if (tcp->state() == QAbstractSocket::ConnectedState && handshakeDone) {
            // Release what we own; the radio would also reclaim it when the
            // socket drops, but other clients see it go at once this way.
            if (transmitting) tcp->write(buildCommand(nextSeq++, "xmit 0"));
            if (rxStreamId) tcp->write(buildCommand(nextSeq++, QString("stream remove 0x%1").arg(rxStreamId, 0, 16)));
            if (txStreamId) tcp->write(buildCommand(nextSeq++, QString("stream remove 0x%1").arg(txStreamId, 0, 16)));
            tcp->waitForBytesWritten(300);
        }
        tcp->disconnect(this);
        // A graceful close: abort() would reset the connection and the radio
        // could drop the commands above unread.
        tcp->disconnectFromHost();
        if (tcp->state() != QAbstractSocket::UnconnectedState)
            tcp->waitForDisconnected(300);
        tcp->abort();
        tcp->deleteLater();
        tcp = nullptr;
    }
    if (udp != nullptr) {
        udp->disconnect(this);
        udp->close();
        udp->deleteLater();
        udp = nullptr;
    }
    if (discovery != nullptr) {
        discovery->disconnect(this);
        discovery->close();
        discovery->deleteLater();
        discovery = nullptr;
    }
    if (pingTimer) { pingTimer->deleteLater(); pingTimer = nullptr; }
    if (reconnectTimer) { reconnectTimer->deleteLater(); reconnectTimer = nullptr; }
    // Drop the queue's pointer to our caps, so the setRigCaps() of the next
    // session re-announces the rig (the queue only signals a pointer change).
    if (haveRigCaps && queue != Q_NULLPTR) {
        queue->setRigCaps(Q_NULLPTR);
        haveRigCaps = false;
    }
    resetSession();
}

void flexCommander::resetSession()
{
    lineBuffer.clear();
    pending.clear();
    nextSeq = 1;
    clientHandle = 0;
    txRequested = false;
    onAir = false;
    txPending.clear();
    handshakeDone = false;
    rigIdentified = false;
    infoDone = false;
    sliceId = -1;
    sliceCreatePending = false;
    panId.clear();
    panStreamId = 0;
    rxStreamId = 0;
    txStreamId = 0;
    panConfigured = false;
    panStates.clear();
    transmitting = false;
    meters.clear();
    fft.reset();
    noiseFloorDbm = -999.0f;
    udpSeen.clear();
    udpStats.clear();
    txAccumulator.clear();
}

void flexCommander::setStatus(const QString& message, bool connected)
{
    Q_UNUSED(connected)
    networkStatus s {};
    s.message = message;
    emit haveStatusUpdate(s);
}

// ─── TCP ────────────────────────────────────────────────────────────────────

void flexCommander::tcpConnected()
{
    qInfo(logRig()) << "Flex: TCP connected to" << tcp->peerAddress().toString() << tcp->peerPort();
    if (radioAddress.isNull())
        radioAddress = tcp->peerAddress();
    lineBuffer.clear();
}

void flexCommander::tcpDisconnected()
{
    const bool wasUp = handshakeDone;
    qInfo(logRig()) << "Flex: TCP connection closed";
    if (pingTimer) pingTimer->stop();
    if (wasUp && transmitting) {
        transmitting = false;
        report(funcTransceiverStatus, QVariant::fromValue<bool>(false));
    }
    resetSession();
    if (!closing) {
        setStatus("Flex: connection to the radio lost, retrying", false);
        if (reconnectTimer) reconnectTimer->start();
    }
}

void flexCommander::tcpError(QAbstractSocket::SocketError err)
{
    if (closing)
        return;
    qWarning(logRig()) << "Flex: TCP error" << err << (tcp ? tcp->errorString() : QString());
    if (tcp && tcp->state() != QAbstractSocket::ConnectedState) {
        setStatus(QString("Flex: cannot connect to %1:%2 (%3), retrying")
                  .arg(radioAddress.isNull() ? prefs.ipAddress : radioAddress.toString())
                  .arg(radioPort).arg(tcp->errorString()), false);
        if (reconnectTimer) reconnectTimer->start();
    }
}

void flexCommander::reconnectTick()
{
    if (closing || tcp == nullptr)
        return;
    if (tcp->state() == QAbstractSocket::UnconnectedState)
        startConnect();
}

void flexCommander::pingTick()
{
    if (handshakeDone)
        sendCmd("ping");
}

quint32 flexCommander::sendCmd(const QString& cmd, responseFn cb)
{
    if (tcp == nullptr || tcp->state() != QAbstractSocket::ConnectedState)
        return 0;
    const quint32 seq = nextSeq++;
    if (cb)
        pending.insert(seq, cb);
    if (!cmd.startsWith(QLatin1String("ping")))
        qDebug(logRigTraffic()).noquote() << "Flex >" << seq << cmd;
    tcp->write(buildCommand(seq, cmd));
    return seq;
}

void flexCommander::tcpReadyRead()
{
    lineBuffer.append(tcp->readAll());
    int nl;
    while ((nl = lineBuffer.indexOf('\n')) >= 0) {
        const QByteArray line = lineBuffer.left(nl);
        lineBuffer.remove(0, nl + 1);
        if (!line.trimmed().isEmpty())
            processLine(QString::fromUtf8(line));
    }
    // Guard against a peer that never sends a newline.
    if (lineBuffer.size() > 1 << 20)
        lineBuffer.clear();
}

void flexCommander::processLine(const QString& line)
{
    if (!line.contains(QLatin1String("|meter ")) && !line.startsWith(QLatin1String("R")))
        qDebug(logRigTraffic()).noquote() << "Flex <" << line.trimmed();
    else if (line.startsWith(QLatin1String("R")) && !line.contains(QLatin1String("|0|")))
        qDebug(logRigTraffic()).noquote() << "Flex <" << line.trimmed();
    const message msg = parseLine(line);
    switch (msg.type) {
    case msgType::Version:
        firmware = msg.object;
        qInfo(logRig()) << "Flex: radio protocol version" << firmware;
        break;

    case msgType::Handle: {
        clientHandle = msg.handle;
        qInfo(logRig()).noquote() << QString("Flex: client handle 0x%1").arg(clientHandle, 8, 16, QChar('0'));

        // Same order as FlexLib / AetherSDR: name ourselves, subscribe,
        // then claim GUI-client status (needed to own a pan and a slice).
        sendCmd("keepalive enable");
        sendCmd("sub radio all");
        sendCmd("sub tx all");
        sendCmd("sub atu all");
        sendCmd("sub slice all");
        sendCmd("sub pan all");
        sendCmd("sub meter all");
        // Needed to hear "client <us> disconnected forced=1" when another
        // client kicks us: the radio does not always close the socket.
        sendCmd("sub client all");
        // The two graphic equalisers answer with an "eq rxsc/txsc" status.
        sendCmd("eq rxsc info");
        sendCmd("eq txsc info");

        // A stable client id lets the radio restore our slice/pan after a
        // reconnect when its persistence option is on. Derived from the
        // GUID servermain gives each rig.
        const QUuid id = QUuid::fromRfc4122(QByteArray(reinterpret_cast<const char*>(guid), GUIDLEN));
        const QString clientId = id.toString().mid(1, 36).toUpper();
        sendCmd(QString("client gui %1").arg(clientId), [this](quint32 code, const QString& body) {
            if (code != 0) {
                qWarning(logRig()).noquote() << QString("Flex: client gui refused (0x%1) %2 — retrying without client id")
                                                .arg(code, 8, 16, QChar('0')).arg(body);
                sendCmd("client gui", [this](quint32 c2, const QString& b2) {
                    if (c2 != 0) {
                        qWarning(logRig()).noquote() << QString("Flex: client gui refused (0x%1) %2").arg(c2, 8, 16, QChar('0')).arg(b2);
                        emit havePortError(errorType(true, radioAddress.toString(),
                            "The radio refused wfweb as a GUI client.\nIt may already have the maximum number of clients connected."));
                        return;
                    }
                    handshakeDone = true;
                });
            } else {
                handshakeDone = true;
            }
            // Everything below is queued behind client gui on the socket,
            // so it runs whether or not the id variant was accepted.
            if (pingTimer) pingTimer->start();
            if (udp)
                sendCmd(QString("client udpport %1").arg(udp->localPort()));
            sendCmd("info", [this](quint32 c, const QString& b) {
                infoDone = true;
                if (c != 0) {
                    identify();
                    return;
                }
                for (const QString& kv : b.split(',')) {
                    const int eq = kv.indexOf('=');
                    if (eq < 0) continue;
                    const QString key = kv.left(eq).trimmed();
                    const QString val = kv.mid(eq + 1).trimmed().remove('\\').remove('"');
                    if (key == "model") radioModel = val;
                    else if (key == "name" || key == "nickname") radioNickname = val;
                }
                qInfo(logRig()).noquote() << "Flex: radio is" << radioModel
                                          << (radioNickname.isEmpty() ? QString() : "\"" + radioNickname + "\"");
                setStatus(QString("Flex: connected to %1 at %2").arg(radioModel, radioAddress.toString()), true);
                identify();
            });
            sendCmd("ant list", [this](quint32 c, const QString& b) {
                if (c == 0) antList = b.split(',', Qt::SkipEmptyParts);
            });
            sendCmd("slice list");
            createAudioStreams();
            // Give a restored session's slice status time to arrive before
            // deciding to create our own receiver.
            QTimer::singleShot(800, this, &flexCommander::adoptOrCreateSlice);
            emit commReady();
        });
        break;
    }

    case msgType::Response: {
        auto it = pending.find(msg.sequence);
        if (it != pending.end()) {
            responseFn cb = it.value();
            pending.erase(it);
            cb(msg.resultValid ? msg.resultCode : 0xFFFFFFFFu, msg.object);
        } else if (msg.resultValid && msg.resultCode != 0) {
            qDebug(logRig()).noquote() << "Flex: command" << msg.sequence << "failed:" << msg.raw;
        }
        break;
    }

    case msgType::Status:
        handleStatus(msg);
        break;

    case msgType::Message:
        qInfo(logRig()).noquote() << "Flex radio message:" << msg.object;
        break;

    default:
        break;
    }
}

void flexCommander::createAudioStreams()
{
    // Slice audio for the browser, uncompressed float32 stereo at 24 kHz.
    sendCmd("stream create type=remote_audio_rx compression=none", [this](quint32 code, const QString& body) {
        if (code == 0) {
            rxStreamId = parseStreamId(body);
            qInfo(logRig()).noquote() << QString("Flex: RX audio stream 0x%1").arg(rxStreamId, 8, 16, QChar('0'));
        } else {
            qWarning(logRig()).noquote() << QString("Flex: cannot create RX audio stream (0x%1) %2").arg(code, 8, 16, QChar('0')).arg(body);
        }
    });
    // Browser mic -> radio. DAX TX is what the radio keys from when
    // "transmit dax=1", which wfweb selects as the LAN mod input.
    sendCmd("stream create type=dax_tx", [this](quint32 code, const QString& body) {
        if (code == 0) {
            txStreamId = parseStreamId(body);
            qInfo(logRig()).noquote() << QString("Flex: DAX TX stream 0x%1").arg(txStreamId, 8, 16, QChar('0'));
        } else {
            qWarning(logRig()).noquote() << QString("Flex: cannot create DAX TX stream (0x%1) %2 — browser TX audio unavailable")
                                            .arg(code, 8, 16, QChar('0')).arg(body);
        }
    });
}

void flexCommander::adoptOrCreateSlice()
{
    if (sliceId >= 0 || sliceCreatePending || !tcp || tcp->state() != QAbstractSocket::ConnectedState)
        return;
    sliceCreatePending = true;

    const double startMHz = freqHz ? freqHz / 1e6 : 14.200;
    const QString startMode = mode.isEmpty() ? QString("USB") : mode;

    auto createSlice = [this, startMHz, startMode]() {
        sendCmd(QString("slice create pan=%1 freq=%2 mode=%3").arg(panId).arg(startMHz, 0, 'f', 6).arg(startMode),
                [this](quint32 code, const QString& body) {
            sliceCreatePending = false;
            if (code != 0) {
                qWarning(logRig()).noquote() << QString("Flex: slice create failed (0x%1) %2").arg(code, 8, 16, QChar('0')).arg(body);
                emit havePortError(errorType(true, radioAddress.toString(),
                    "The radio could not give wfweb a receiver (slice).\nClose a slice in another client and reconnect."));
                return;
            }
            bool ok = false;
            const int id = body.trimmed().toInt(&ok);
            if (!ok)
                return;
            // Its status (with our client_handle) may already have arrived
            // and made it ours; either way make it the active TX slice.
            if (sliceId < 0)
                sliceId = id;
            if (id == sliceId) {
                qInfo(logRig()) << "Flex: created slice" << sliceId;
                sendCmd(QString("slice set %1 active=1 tx=1").arg(sliceId));
            }
        });
    };

    if (!panId.isEmpty()) {
        createSlice();
        return;
    }
    sendCmd("display panafall create x=100 y=100", [this, createSlice](quint32 code, const QString& body) {
        if (code != 0) {
            sliceCreatePending = false;
            qWarning(logRig()).noquote() << QString("Flex: panadapter create failed (0x%1) %2").arg(code, 8, 16, QChar('0')).arg(body);
            emit havePortError(errorType(true, radioAddress.toString(),
                "The radio could not give wfweb a panadapter.\nClose a panadapter in another client and reconnect."));
            return;
        }
        const QMap<QString, QString> kvs = parseKVs(body);
        QString id = kvs.contains("pan") ? kvs.value("pan") : body.trimmed().split(',').value(0);
        adoptPan(normalizedId(id));
        qInfo(logRig()) << "Flex: created panadapter" << panId;
        createSlice();
    });
}

void flexCommander::adoptPan(const QString& id)
{
    panId = id;
    panStreamId = panId.mid(2).toUInt(nullptr, 16);
    panConfigured = false;
    fft.reset();
    if (panStates.contains(panId))
        handlePanStatus(panId, panStates.value(panId));
    configurePan();
}

void flexCommander::recenterPan()
{
    // wfweb's scope is CENTER mode: the VFO sits in the middle and the band
    // moves under it, as on an Icom. A Flex panadapter only scrolls when the
    // slice leaves the screen, so keep re-centring it on the slice. Changes
    // are coalesced, so a spinning knob sends at most one move per 50 ms.
    if (panId.isEmpty() || !freqHz)
        return;
    if (std::fabs(panCenterMHz - freqHz / 1e6) < 1e-6)
        return;
    if (recenterPending)
        return;
    recenterPending = true;
    QTimer::singleShot(50, this, [this]() {
        recenterPending = false;
        if (panId.isEmpty() || !freqHz || std::fabs(panCenterMHz - freqHz / 1e6) < 1e-6)
            return;
        panCenterMHz = freqHz / 1e6;   // the radio does not always echo it
        sendCmd(QString("display pan set %1 center=%2").arg(panId).arg(panCenterMHz, 0, 'f', 6));
    });
}

void flexCommander::configurePan()
{
    if (panId.isEmpty() || panConfigured)
        return;
    panConfigured = true;
    // One FFT bin per wfweb scope point. The vertical scale is a wide, fine
    // dBm range; handleFft() turns it into wfweb's counts against the noise
    // floor. Before the caps load, rigCaps holds no sizes yet.
    const int width = (haveRigCaps && rigCaps.spectLenMax) ? int(rigCaps.spectLenMax) : 1024;
    sendCmd(QString("display pan set %1 xpixels=%2 ypixels=%3").arg(panId).arg(width).arg(kPanYPixels));
    sendCmd(QString("display pan set %1 min_dbm=%2 max_dbm=%3").arg(panId).arg(kPanMinDbm).arg(kPanMaxDbm));
    sendCmd(QString("display pan set %1 fps=%2 average=40").arg(panId).arg(scopeFps));
    panXPixels = width;
    panYPixels = kPanYPixels;
    panMinDbm = kPanMinDbm;
    panMaxDbm = kPanMaxDbm;
    fft.reset();
    recenterPan();
}

// ─── Status ────────────────────────────────────────────────────────────────

void flexCommander::handleStatus(const message& msg)
{
    const QString& obj = msg.object;

    if (obj.startsWith(QLatin1String("slice "))) {
        bool ok = false;
        const int id = obj.mid(6).trimmed().toInt(&ok);
        if (ok) handleSliceStatus(id, msg.kvs);
        return;
    }
    if (obj.startsWith(QLatin1String("display pan "))) {
        handlePanStatus(normalizedId(obj.mid(12).trimmed()), msg.kvs);
        return;
    }
    // Only the live transmit object: "transmit band N ..." lines carry each
    // band's stored settings (rfpower etc.) and would make the page jump.
    if (obj == QLatin1String("transmit")) {
        handleTransmitStatus(msg.kvs);
        return;
    }
    if (obj.compare(QLatin1String("eq rxsc"), Qt::CaseInsensitive) == 0
        || obj.compare(QLatin1String("eq txsc"), Qt::CaseInsensitive) == 0) {
        handleEqStatus(obj.endsWith(QLatin1String("txsc"), Qt::CaseInsensitive), msg.kvs);
        return;
    }
    // "client 0x1234ABCD disconnected forced=1 duplicate_client_id=0 ..."
    if (obj.startsWith(QLatin1String("client 0x")) && obj.endsWith(QLatin1String(" disconnected"))) {
        bool ok = false;
        const quint32 h = obj.mid(9, obj.indexOf(' ', 9) - 9).toUInt(&ok, 16);   // after "client 0x"
        if (ok && h == clientHandle && clientHandle != 0) {
            const bool forced = truthy(msg.kvs.value("forced"));
            const bool dup = truthy(msg.kvs.value("duplicate_client_id"));
            if (forced || dup) {
                const QString why = dup
                    ? QStringLiteral("another client is using wfweb's client id")
                    : QStringLiteral("disconnected by another client on the radio");
                qWarning(logRig()).noquote() << "Flex:" << why << "- not reconnecting";
                // No automatic retry: whoever kicked us meant it. servermain
                // closes the session (closeComm) when it gets evicted().
                closing = true;
                if (reconnectTimer) reconnectTimer->stop();
                setStatus(QStringLiteral("Flex: ") + why, false);
                emit evicted(why);
            }
        }
        return;
    }
    if (obj.startsWith(QLatin1String("client "))) return;   // other clients come and go
    if (obj == QLatin1String("interlock")) {
        if (msg.kvs.contains("state")) {
            const QString st = msg.kvs.value("state").toUpper();
            const bool tx = (st == "TRANSMITTING" || st == "PTT_REQUESTED");
            const bool nowOnAir = (st == "TRANSMITTING");
            if (nowOnAir && !onAir) {
                onAir = true;
                // Release what was said while the radio was keying up, in
                // order and at once; the radio's DAX buffer absorbs it.
                if (!txPending.isEmpty() && udp != nullptr) {
                    for (const QByteArray& pkt : txPending)
                        udp->writeDatagram(pkt, radioAddress, radioUdpPort);
                }
                qInfo(logRig()).noquote() << QString("Flex: on the air %1 ms after xmit 1, sent %2 ms of held audio")
                    .arg(txKeyTimer.isValid() ? txKeyTimer.elapsed() : -1)
                    .arg(txPending.size() * DAX_TX_FRAMES * 1000 / DAX_RATE);
                txPending.clear();
            } else if (!nowOnAir) {
                onAir = false;
            }
            if (tx != transmitting) {
                transmitting = tx;
                if (tx) { lastAlc = 0.0; txPeakWindow = 0.0f; }
                report(funcTransceiverStatus, QVariant::fromValue<bool>(transmitting));
            }
        }
        return;
    }
    if (obj == QLatin1String("atu")) {
        atuPresent = true;
        if (msg.kvs.contains("status")) {
            atuStatus = msg.kvs.value("status");
            reportCurrent(funcTunerStatus);
        }
        return;
    }
    if (obj == QLatin1String("cwx") || obj == QLatin1String("cw")) {
        if (msg.kvs.contains("wpm")) { cwSpeed = msg.kvs.value("wpm").toInt(); reportCurrent(funcKeySpeed); }
        return;
    }
    if (obj.isEmpty() && msg.raw.contains(QLatin1String("|meter "))) {
        handleMeterStatus(msg.raw.mid(msg.raw.indexOf(QLatin1String("|meter ")) + 7));
        return;
    }
    if (obj.startsWith(QLatin1String("meter"))) {
        const int pipe = msg.raw.indexOf('|');
        handleMeterStatus(msg.raw.mid(pipe + 1).mid(6));
        return;
    }
    if (obj == QLatin1String("radio")) {
        if (msg.kvs.contains("nickname")) radioNickname = msg.kvs.value("nickname");
        return;
    }
}

void flexCommander::handleSliceStatus(int id, const QMap<QString, QString>& kvs)
{
    // Removal of our slice: forget it and get another one.
    if (id == sliceId && (kvs.contains("in_use") && kvs.value("in_use") == "0")) {
        qInfo(logRig()) << "Flex: our slice" << id << "was closed";
        sliceId = -1;
        QTimer::singleShot(500, this, &flexCommander::adoptOrCreateSlice);
        return;
    }

    if (sliceId < 0) {
        // Adopt a slice the radio says is ours (a restored session or the
        // reply to our own slice create arriving before its R line).
        if (!kvs.contains("client_handle"))
            return;
        const quint32 owner = kvs.value("client_handle").toUInt(nullptr, 16);
        if (owner != clientHandle || kvs.value("in_use") == "0")
            return;
        sliceId = id;
        sliceCreatePending = false;
        qInfo(logRig()) << "Flex: using slice" << sliceId;
    } else if (id != sliceId) {
        return;
    }

    if (kvs.contains("pan")) {
        const QString p = normalizedId(kvs.value("pan"));
        if (!p.isEmpty() && p != panId)
            adoptPan(p);
    }

    bool modeChanged = false;
    if (kvs.contains("RF_frequency") || kvs.contains("freq")) {
        const double mhz = (kvs.contains("RF_frequency") ? kvs.value("RF_frequency") : kvs.value("freq")).toDouble();
        freqHz = quint64(std::llround(mhz * 1e6));
        reportCurrent(funcSelectedFreq);
        recenterPan();
    }
    if (kvs.contains("mode")) {
        mode = kvs.value("mode").toUpper();
        modeChanged = true;
    }
    if (kvs.contains("filter_lo") || kvs.contains("filter_hi")) {
        if (kvs.contains("filter_lo")) filterLo = kvs.value("filter_lo").toInt();
        if (kvs.contains("filter_hi")) filterHi = kvs.value("filter_hi").toInt();
        filterNum = filterNumForWidth(mode, filterHi - filterLo);
        reportCurrent(funcFilterWidth);
        modeChanged = true;
    }
    if (modeChanged)
        reportCurrent(funcSelectedMode);

    if (kvs.contains("audio_level")) { audioLevel = kvs.value("audio_level").toInt(); reportCurrent(funcAfGain); }
    if (kvs.contains("agc_threshold")) { agcThreshold = kvs.value("agc_threshold").toInt(); reportCurrent(funcRfGain); }
    if (kvs.contains("agc_mode")) { agcMode = kvs.value("agc_mode").toLower(); reportCurrent(funcAGC); }
    if (kvs.contains("nb")) { nb = truthy(kvs.value("nb")); reportCurrent(funcNoiseBlanker); }
    if (kvs.contains("nr")) { nr = truthy(kvs.value("nr")); reportCurrent(funcNoiseReduction); }
    if (kvs.contains("anf")) { anf = truthy(kvs.value("anf")); reportCurrent(funcAutoNotch); }
    if (kvs.contains("squelch")) squelchOn = truthy(kvs.value("squelch"));
    if (kvs.contains("squelch_level")) squelchLevel = kvs.value("squelch_level").toInt();
    if (kvs.contains("squelch") || kvs.contains("squelch_level")) reportCurrent(funcSquelch);
    if (kvs.contains("rxant")) { rxAnt = kvs.value("rxant"); reportCurrent(funcAntenna); publishDsp(); }
    if (kvs.contains("txant")) { txAnt = kvs.value("txant"); publishDsp(); }
    if (kvs.contains("ant_list")) { rxAntList = kvs.value("ant_list").split(',', Qt::SkipEmptyParts); publishDsp(); }
    if (kvs.contains("tx_ant_list")) { txAntList = kvs.value("tx_ant_list").split(',', Qt::SkipEmptyParts); publishDsp(); }
    if (kvs.contains("RF_frequency") || kvs.contains("freq") || kvs.contains("ant_list")) applyBandAntennas();
    if (kvs.contains("rit_on")) { ritOn = truthy(kvs.value("rit_on")); reportCurrent(funcRitStatus); }
    if (kvs.contains("rit_freq")) { ritFreq = kvs.value("rit_freq").toInt(); reportCurrent(funcRitFreq); }
}

void flexCommander::handlePanStatus(const QString& id, const QMap<QString, QString>& kvs)
{
    if (id.isEmpty())
        return;
    // A pan's first status usually arrives before the R line that tells us
    // its id, so keep every pan's state and apply it once the id is ours.
    QMap<QString, QString>& known = panStates[id];
    for (auto it = kvs.constBegin(); it != kvs.constEnd(); ++it)
        known.insert(it.key(), it.value());
    if (id != panId)
        return;
    if (kvs.contains("center") || kvs.contains("bandwidth") || kvs.contains("x_pixels") || kvs.contains("y_pixels"))
        qInfo(logRig()).noquote() << "Flex: panadapter" << panId
                                  << (kvs.contains("center") ? "center=" + kvs.value("center") : QString())
                                  << (kvs.contains("bandwidth") ? "bandwidth=" + kvs.value("bandwidth") : QString())
                                  << (kvs.contains("x_pixels") ? "x_pixels=" + kvs.value("x_pixels") : QString())
                                  << (kvs.contains("y_pixels") ? "y_pixels=" + kvs.value("y_pixels") : QString());
    if (kvs.contains("center")) panCenterMHz = kvs.value("center").toDouble();
    if (kvs.contains("bandwidth")) panBandwidthMHz = kvs.value("bandwidth").toDouble();
    if (kvs.contains("x_pixels")) panXPixels = kvs.value("x_pixels").toInt();
    // The vertical scale is ours: configurePan() set it, and the radio does
    // not echo a client's own size/range changes back (FLEX-6400 kept
    // reporting the pan's old 50x20 and -140..-40 while sending 1024-bin
    // frames on the new scale). Only take the radio's word before that.
    if (!panConfigured) {
        if (kvs.contains("y_pixels")) panYPixels = kvs.value("y_pixels").toInt();
        if (kvs.contains("min_dbm")) panMinDbm = kvs.value("min_dbm").toDouble();
        if (kvs.contains("max_dbm")) panMaxDbm = kvs.value("max_dbm").toDouble();
    }
    if (kvs.contains("rfgain")) {
        panRfGain = kvs.value("rfgain").toInt();
        reportCurrent(funcPreamp);
        reportCurrent(funcAttenuator);
    }
    if (kvs.contains("bandwidth"))
        reportCurrent(funcScopeSpan);
}

void flexCommander::handleTransmitStatus(const QMap<QString, QString>& kvs)
{
    if (kvs.contains("rfpower")) { rfPower = kvs.value("rfpower").toInt(); reportCurrent(funcRFPower); }
    if (kvs.contains("mic_level")) micLevel = kvs.value("mic_level").toInt();
    if (kvs.contains("miclevel")) micLevel = kvs.value("miclevel").toInt();
    if (kvs.contains("dax")) { daxTx = truthy(kvs.value("dax")); reportCurrent(funcDATAOffMod); }
    if (kvs.contains("mon")) { monitor = truthy(kvs.value("mon")); reportCurrent(funcMonitor); }
    if (kvs.contains("mon_gain_sb")) { monitorGain = kvs.value("mon_gain_sb").toInt(); reportCurrent(funcMonitorGain); }
    if (kvs.contains("speech_processor_enable")) { processor = truthy(kvs.value("speech_processor_enable")); reportCurrent(funcCompressor); }
    bool dsp = false;
    if (kvs.contains("speech_processor_enable") || kvs.contains("miclevel") || kvs.contains("mic_level")) dsp = true;
    if (kvs.contains("speech_processor_level")) { procLevel = qBound(0, kvs.value("speech_processor_level").toInt(), 2); dsp = true; }
    if (kvs.contains("compander")) { dexpOn = truthy(kvs.value("compander")); dsp = true; }
    if (kvs.contains("compander_level")) { dexpLevel = qBound(0, kvs.value("compander_level").toInt(), 100); dsp = true; }
    if (kvs.contains("lo")) { txFilterLo = kvs.value("lo").toInt(); dsp = true; }
    if (kvs.contains("hi")) { txFilterHi = kvs.value("hi").toInt(); dsp = true; }
    if (dsp) publishDsp();
    if (kvs.contains("tune")) tuning = truthy(kvs.value("tune"));
    if (kvs.contains("speed")) { cwSpeed = kvs.value("speed").toInt(); reportCurrent(funcKeySpeed); }
    if (kvs.contains("pitch")) { cwPitch = kvs.value("pitch").toInt(); reportCurrent(funcCwPitch); }
}

void flexCommander::handleMeterStatus(const QString& body)
{
    // "7.src=SLC#7.num=0#7.nam=LEVEL#7.unit=dBm#7.low=-150.0#7.hi=20.0#..."
    // or "7 removed".
    if (body.contains(QLatin1String("removed"))) {
        bool ok = false;
        const int idx = body.split(' ', Qt::SkipEmptyParts).value(0).toInt(&ok);
        if (ok) meters.remove(quint16(idx));
        return;
    }
    for (const QString& token : body.split('#', Qt::SkipEmptyParts)) {
        const int dot = token.indexOf('.');
        const int eq = token.indexOf('=', dot);
        if (dot < 0 || eq < 0) continue;
        bool ok = false;
        const quint16 idx = quint16(token.left(dot).toUInt(&ok));
        if (!ok) continue;
        const QString key = token.mid(dot + 1, eq - dot - 1);
        const QString val = token.mid(eq + 1).trimmed();
        meterDef& m = meters[idx];
        if (key == "src") m.src = val;
        else if (key == "num") m.num = val.toInt();
        else if (key == "nam") m.name = val;
        else if (key == "unit") m.unit = val;
    }
}

// ─── UDP / VITA-49 ─────────────────────────────────────────────────────────

void flexCommander::udpReadyRead()
{
    while (udp && udp->hasPendingDatagrams()) {
        QByteArray d;
        d.resize(int(udp->pendingDatagramSize()));
        udp->readDatagram(d.data(), d.size());
        const vitaPacket p = parseVita(d);
        if (!p.valid || !p.hasClassId || p.oui != FLEX_OUI) {
            ++udpStats["other"];
            continue;
        }

        // Diagnostics: name each stream the first time it shows up, and
        // summarise what arrived every 10 s (see logUdpStats).
        const quint64 key = (quint64(p.pcc) << 32) | p.streamId;
        if (!udpSeen.contains(key)) {
            udpSeen.insert(key);
            qInfo(logRig()).noquote() << QString("Flex: first UDP packet pcc=0x%1 stream=0x%2 bytes=%3%4")
                .arg(p.pcc, 4, 16, QChar('0')).arg(p.streamId, 8, 16, QChar('0')).arg(d.size())
                .arg(p.pcc == PCC_FFT ? (p.streamId == panStreamId ? " (our panadapter)" : QString(" (not our pan %1)").arg(panId))
                     : (p.streamId == rxStreamId && rxStreamId) ? " (our RX audio)" : "");
        }
        if (!udpStatsTimer.isValid())
            udpStatsTimer.start();
        if (udpStatsTimer.elapsed() >= 10000) {
            logUdpStats();
            udpStatsTimer.restart();
        }

        switch (p.pcc) {
        case PCC_FFT:
            ++udpStats[p.streamId == panStreamId ? "fft" : "fft-other-pan"];
            if (p.streamId == panStreamId) handleFft(d, p);
            break;
        case PCC_WATERFALL:
            ++udpStats["waterfall"];
            break;
        case PCC_METER:
            ++udpStats["meter"];
            handleMeters(d, p);
            break;
        case PCC_IF_NARROW:
        case PCC_IF_NARROW_REDUCED:
            ++udpStats["audio"];
            if (rxStreamId == 0 || p.streamId == rxStreamId) handleRxAudio(d, p);
            break;
        default:
            ++udpStats[QString("pcc-0x%1").arg(p.pcc, 4, 16, QChar('0'))];
            break;
        }
    }
}

void flexCommander::logUdpStats()
{
    QStringList parts;
    for (auto it = udpStats.constBegin(); it != udpStats.constEnd(); ++it)
        parts << QString("%1=%2").arg(it.key()).arg(it.value());
    qInfo(logRig()).noquote() << "Flex: UDP in last 10 s:" << (parts.isEmpty() ? QString("nothing") : parts.join(' '))
                              << QString("| scope frames sent %1, dropped %2 (pan %3 center %4 bw %5 MHz, %6x%7, caps %8)")
                                     .arg(scopeFramesSent).arg(scopeFramesDropped).arg(panId.isEmpty() ? "none" : panId)
                                     .arg(panCenterMHz, 0, 'f', 6).arg(panBandwidthMHz, 0, 'f', 6)
                                     .arg(panXPixels).arg(panYPixels).arg(haveRigCaps ? "yes" : "no");
    udpStats.clear();
    scopeFramesSent = 0;
    scopeFramesDropped = 0;
}

void flexCommander::handleFft(const QByteArray& d, const vitaPacket& p)
{
    if (!fft.feed(d, p))
        return;
    if (!haveRigCaps || panBandwidthMHz <= 0.0) {
        ++scopeFramesDropped;
        return;
    }
    ++scopeFramesSent;

    const QVector<quint16>& bins = fft.bins();
    if (bins.isEmpty())
        return;
    const int ampMax = rigCaps.spectAmpMax ? int(rigCaps.spectAmpMax) : 160;
    const int yMax = (panYPixels > 1 ? panYPixels : kPanYPixels) - 1;
    const double range = panMaxDbm - panMinDbm;

    // Bin pixel -> dBm (pixel 0 = max_dbm).
    QVector<float> dbm(bins.size());
    for (int i = 0; i < bins.size(); ++i) {
        const int y = qBound(0, int(bins[i]), yMax);
        dbm[i] = float(panMaxDbm - (yMax > 0 ? double(y) / yMax : 1.0) * range);
    }

    // wfweb's scope and waterfall are drawn for an Icom, whose wave data
    // spans about 45 dB over 0..spectAmpMax with the noise floor near the
    // bottom. Track the floor (median bin, smoothed) and place it 6 dB up
    // that scale, so any band and antenna look like the rig the page expects.
    QVector<float> sorted = dbm;
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    const float median = sorted[sorted.size() / 2];
    noiseFloorDbm = (noiseFloorDbm < -900.0f) ? median : noiseFloorDbm + 0.1f * (median - noiseFloorDbm);
    const float base = noiseFloorDbm - 6.0f;
    const float countsPerDb = float(ampMax) / 45.0f;

    scopeData sd;
    sd.valid = true;
    sd.receiver = 0;
    sd.mode = 0;            // center mode: the pan follows the slice
    sd.fixedEdge = 0;
    sd.oor = false;
    sd.startFreq = panCenterMHz - panBandwidthMHz / 2.0;
    sd.endFreq = panCenterMHz + panBandwidthMHz / 2.0;
    sd.data.resize(bins.size());
    for (int i = 0; i < bins.size(); ++i) {
        const int amp = int(std::lround((dbm[i] - base) * countsPerDb));
        sd.data[i] = char(uchar(qBound(0, amp, ampMax)));
    }
    queue->receiveValue(funcScopeWaveData, QVariant::fromValue(sd), 0);
}

void flexCommander::handleMeters(const QByteArray& d, const vitaPacket& p)
{
    bool sChanged = false, txChanged = false;
    for (const auto& mv : parseMeters(d, p)) {
        auto it = meters.constFind(mv.first);
        if (it == meters.constEnd())
            continue;
        const meterDef& m = it.value();
        const float v = meterScale(m.unit, mv.second);

        if (m.src == QLatin1String("SLC") && m.num == sliceId && m.name == QLatin1String("LEVEL")) {
            // dBm -> dB relative to S9, which is what wfweb's S-meter scale
            // speaks (S9 = -73 dBm on HF, -93 dBm above 30 MHz).
            const double s9 = (freqHz > 30000000ULL) ? -93.0 : -73.0;
            lastSMeter = double(v) - s9;
            sChanged = true;
        } else if (m.src.startsWith(QLatin1String("TX")) && m.name == QLatin1String("FWDPWR")) {
            lastPower = (m.unit == QLatin1String("dBm")) ? std::pow(10.0, (double(v) - 30.0) / 10.0) : double(v);
            if (lastPower < 0.05) lastPower = 0.0;
            txChanged = true;
        } else if (m.src.startsWith(QLatin1String("TX")) && m.name == QLatin1String("SWR")) {
            lastSwr = qMax(1.0, double(v));
            txChanged = true;
        } else if (m.name == QLatin1String("COMPPEAK")) {
            lastComp = qMax(0.0, -double(v));
            txChanged = true;
        } else if (m.src == QLatin1String("RAD") && (m.name == QLatin1String("+13.8A") || m.name == QLatin1String("+13.8B"))) {
            lastVolts = double(v);
        } else if (m.name == QLatin1String("PACURRENT") || m.name == QLatin1String("CURRENT")) {
            lastAmps = double(v);
        }
    }

    // Meter packets come at up to ~20 Hz per meter, each carrying only some
    // meters; the page wants ~10 Hz. The first reading after a quiet spell
    // goes at once, later ones within 100 ms are held and the latest is sent
    // when the window ends. (A plain "drop inside 100 ms" lost the S-meter
    // whenever a packet of other meters had just opened the window, so the
    // meter stalled for several updates at a time.)
    meterSDirty = meterSDirty || sChanged;
    meterTxDirty = meterTxDirty || txChanged;
    if (meterFlushTimer == nullptr) {
        meterFlushTimer = new QTimer(this);
        meterFlushTimer->setSingleShot(true);
        meterFlushTimer->setInterval(100);
        connect(meterFlushTimer, &QTimer::timeout, this, [this]() {
            if (meterSDirty || meterTxDirty) {
                flushMeters();
                meterFlushTimer->start();   // keep the 100 ms spacing
            }
        });
    }
    if (!meterFlushTimer->isActive()) {
        flushMeters();
        meterFlushTimer->start();
    }
}

void flexCommander::flushMeters()
{
    if (meterSDirty && !transmitting)
        queue->receiveValue(funcSMeter, QVariant::fromValue<double>(lastSMeter), 0);
    if (meterTxDirty && transmitting) {
        queue->receiveValue(funcPowerMeter, QVariant::fromValue<double>(lastPower), 0);
        queue->receiveValue(funcSWRMeter, QVariant::fromValue<double>(lastSwr), 0);
        queue->receiveValue(funcALCMeter, QVariant::fromValue<double>(lastAlc), 0);
        queue->receiveValue(funcCompMeter, QVariant::fromValue<double>(lastComp), 0);
    }
    meterSDirty = false;
    meterTxDirty = false;
}

void flexCommander::handleRxAudio(const QByteArray& d, const vitaPacket& p)
{
    QVector<float> in = decodeAudio(d, p);
    if (in.isEmpty() || rxResampler == nullptr)
        return;

    const quint32 outRate = rxSetup.sampleRate ? rxSetup.sampleRate : 48000;
    QVector<float> out(int(in.size() * (double(outRate) / DAX_RATE)) + 32);
    spx_uint32_t inLen = static_cast<spx_uint32_t>(in.size());
    spx_uint32_t outLen = static_cast<spx_uint32_t>(out.size());
    speex_resampler_process_float(rxResampler, 0, in.constData(), &inLen, out.data(), &outLen);

    audioPacket pkt;
    pkt.seq = rxSeq++;
    pkt.time = QTime::currentTime();
    pkt.sent = 0;
    memcpy(pkt.guid, guid, GUIDLEN);
    pkt.data.resize(int(outLen) * 2);
    auto* dst = reinterpret_cast<qint16*>(pkt.data.data());
    float peak = 0.0f;
    double sumSq = 0.0;
    for (spx_uint32_t i = 0; i < outLen; ++i) {
        const float s = qBound(-1.0f, out[int(i)], 1.0f);
        dst[i] = qToLittleEndian<qint16>(qint16(std::lround(s * 32767.0f)));
        peak = qMax(peak, std::fabs(s));
        sumSq += double(s) * s;
    }
    pkt.amplitudePeak = peak;
    pkt.amplitudeRMS = outLen ? float(std::sqrt(sumSq / outLen)) : 0.0f;
    pkt.volume = 1.0;
    emit haveAudioData(pkt);
}

void flexCommander::webTxAudio(const audioPacket& data)
{
    // Browser mic (LPCM int16 LE mono at txSetup.sampleRate) -> DAX TX.
    if (txStreamId == 0 || udp == nullptr || radioAddress.isNull() || txResampler == nullptr)
        return;
    if (data.data.size() < 2)
        return;

    const int n = data.data.size() / 2;
    const auto* src = reinterpret_cast<const qint16*>(data.data.constData());
    // modLevel is wfweb's 0..255 "USB mod level"; 75 % is unity gain, the
    // level the FT8/JS8 panels set.
    const float gain = float(modLevel) / 191.0f;
    QVector<float> in(n);
    for (int i = 0; i < n; ++i)
        in[i] = float(qFromLittleEndian<qint16>(src[i])) / 32768.0f * gain;

    const quint32 inRate = txSetup.sampleRate ? txSetup.sampleRate : 48000;
    QVector<float> out(int(n * (double(DAX_RATE) / inRate)) + 32);
    spx_uint32_t inLen = static_cast<spx_uint32_t>(n);
    spx_uint32_t outLen = static_cast<spx_uint32_t>(out.size());
    speex_resampler_process_float(txResampler, 0, in.constData(), &inLen, out.data(), &outLen);
    out.resize(int(outLen));
    txAccumulator += out;

    // The drive level, reported as ALC (see txPeakWindow). Measured before
    // the clamp below, so audio driven past full scale reads above 1.0.
    for (float s : out) txPeakWindow = qMax(txPeakWindow, std::fabs(s));
    if (!txPeakTimer.isValid() || txPeakTimer.elapsed() >= 100) {
        const double db = 20.0 * std::log10(qMax(double(txPeakWindow), 1e-6));
        lastAlc = qBound(0.0, (db + 2.0) / 2.0, 2.0);
        txPeakWindow = 0.0f;
        txPeakTimer.start();
        if (transmitting)
            queue->receiveValue(funcALCMeter, QVariant::fromValue<double>(lastAlc), 0);
    }

    float stereo[DAX_TX_FRAMES * 2];
    while (txAccumulator.size() >= DAX_TX_FRAMES) {
        for (int i = 0; i < DAX_TX_FRAMES; ++i) {
            const float s = qBound(-1.0f, txAccumulator[i], 1.0f);
            stereo[i * 2] = s;
            stereo[i * 2 + 1] = s;
        }
        txAccumulator.remove(0, DAX_TX_FRAMES);
        const QByteArray pkt = buildDaxTxPacket(txStreamId, txPacketCount++, stereo, DAX_TX_FRAMES);
        if (onAir) {
            udp->writeDatagram(pkt, radioAddress, radioUdpPort);
        } else if (txRequested) {
            // Keyed but not on the air yet: hold it (see txPending).
            txPending.append(pkt);
            if (txPending.size() > kMaxPendingPackets) txPending.removeFirst();
        }
        // Neither: receiving, and a running mic has nowhere to go.
    }
}

// ─── wfweb side ────────────────────────────────────────────────────────────

void flexCommander::report(funcs func, const QVariant& value)
{
    if (queue != Q_NULLPTR && value.isValid())
        queue->receiveValue(func, value, 0);
}

QString flexCommander::flexModeForReg(quint8 reg) const
{
    for (const auto& m : kModes)
        if (m.reg == reg) return QString::fromLatin1(m.name);
    return QString();
}

modeInfo flexCommander::modeFor(const QString& flexMode, int filter) const
{
    quint8 reg = 0;
    for (const auto& m : kModes)
        if (flexMode == QLatin1String(m.name)) reg = m.reg;
    for (const modeInfo& mi : rigCaps.modes) {
        if (mi.reg == reg) {
            modeInfo out = mi;
            out.filter = quint8(filter);
            out.data = 0;
            out.VFO = activeVFO;
            return out;
        }
    }
    modeInfo unknown;
    unknown.mk = modeUnknown;
    unknown.name = flexMode;
    unknown.filter = quint8(filter);
    unknown.data = 0;
    return unknown;
}

int flexCommander::filterNumForWidth(const QString& m, int width) const
{
    for (int f = 1; f <= 3; ++f)
        if (presetWidth(m, f) == width) return f;
    return filterNum;   // a custom width keeps the slot the page last chose
}

bool flexCommander::reportCurrent(funcs func)
{
    // Answer for the state the radio last pushed. Returns false for funcs
    // this commander has no value for.
    switch (func) {
    case funcSelectedFreq:
    case funcFreq:
    case funcFreqGet: {
        if (!freqHz) return true;
        freqt f;
        f.Hz = freqHz;
        f.MHzDouble = freqHz / 1e6;
        f.VFO = activeVFO;
        report(funcSelectedFreq, QVariant::fromValue<freqt>(f));
        return true;
    }
    case funcSelectedMode:
    case funcMode:
    case funcModeGet:
        if (mode.isEmpty() || !haveRigCaps) return true;
        report(funcSelectedMode, QVariant::fromValue<modeInfo>(modeFor(mode, filterNum)));
        return true;
    case funcFilterWidth:
        if (filterHi - filterLo > 0)
            report(funcFilterWidth, QVariant::fromValue<ushort>(ushort(filterHi - filterLo)));
        return true;
    case funcAfGain:
        report(funcAfGain, QVariant::fromValue<ushort>(ushort(to255(audioLevel))));
        return true;
    case funcRfGain:
        report(funcRfGain, QVariant::fromValue<ushort>(ushort(to255(agcThreshold))));
        return true;
    case funcRFPower:
        report(funcRFPower, QVariant::fromValue<ushort>(ushort(to255(rfPower))));
        return true;
    case funcUSBModLevel:
        report(funcUSBModLevel, QVariant::fromValue<ushort>(ushort(modLevel)));
        return true;
    case funcSquelch:
        report(funcSquelch, QVariant::fromValue<ushort>(ushort(squelchOn ? to255(squelchLevel) : 0)));
        return true;
    case funcAGC: {
        const uchar v = agcMode == "fast" ? 1 : agcMode == "med" ? 2 : agcMode == "slow" ? 3 : 0;
        report(funcAGC, QVariant::fromValue<uchar>(v));
        return true;
    }
    case funcNoiseBlanker:
        report(funcNoiseBlanker, QVariant::fromValue<bool>(nb));
        return true;
    case funcNoiseReduction:
        report(funcNoiseReduction, QVariant::fromValue<bool>(nr));
        return true;
    case funcAutoNotch:
        report(funcAutoNotch, QVariant::fromValue<bool>(anf));
        return true;
    case funcPreamp:
        // Preamp slots 0/1/2/3 = pan rfgain 0/+10/+20/+30 dB (the radio
        // snaps to the nearest step its front end has).
        report(funcPreamp, QVariant::fromValue<uchar>(uchar(panRfGain > 0 ? qBound(1, (panRfGain + 5) / 10, 3) : 0)));
        return true;
    case funcAttenuator:
        report(funcAttenuator, QVariant::fromValue<uchar>(uchar(panRfGain < 0 ? 1 : 0)));
        return true;
    case funcAntenna: {
        antennaInfo a;
        a.rx = rxAnt.startsWith(QLatin1String("RX_"));
        a.antenna = quint8(rxAnt == QLatin1String("ANT2") ? 1 : 0);
        report(funcAntenna, QVariant::fromValue<antennaInfo>(a));
        return true;
    }
    case funcRitStatus:
        report(funcRitStatus, QVariant::fromValue<bool>(ritOn));
        return true;
    case funcRitFreq:
        report(funcRitFreq, QVariant::fromValue<short>(short(ritFreq)));
        return true;
    case funcTransceiverStatus:
        report(funcTransceiverStatus, QVariant::fromValue<bool>(transmitting));
        return true;
    case funcTunerStatus: {
        if (!atuPresent) return true;
        const QString s = atuStatus.toUpper();
        uchar v = 0;
        if (s.contains("IN_PROGRESS")) v = 2;
        else if (s.contains("SUCCESS") || s.contains("OK") || s == "TUNE_MANUAL_BYPASS") v = 1;
        if (s.contains("BYPASS")) v = 0;
        report(funcTunerStatus, QVariant::fromValue<uchar>(v));
        return true;
    }
    case funcKeySpeed:
        report(funcKeySpeed, QVariant::fromValue<ushort>(ushort(cwSpeed)));
        return true;
    case funcCwPitch:
        report(funcCwPitch, QVariant::fromValue<ushort>(ushort(cwPitch)));
        return true;
    case funcMonitor:
        report(funcMonitor, QVariant::fromValue<bool>(monitor));
        return true;
    case funcMonitorGain:
        report(funcMonitorGain, QVariant::fromValue<ushort>(ushort(to255(monitorGain))));
        return true;
    case funcCompressor:
        report(funcCompressor, QVariant::fromValue<bool>(processor));
        return true;
    case funcDATAOffMod: {
        for (const rigInput& in : rigCaps.inputs) {
            if ((daxTx && in.type == inputLAN) || (!daxTx && in.type == inputMic)) {
                report(funcDATAOffMod, QVariant::fromValue<rigInput>(in));
                break;
            }
        }
        return true;
    }
    case funcScopeSpan: {
        if (panBandwidthMHz <= 0.0) return true;
        // Span entries are half widths (the Icom "±" convention).
        const quint32 hz = quint32(std::lround(panBandwidthMHz * 1e6 / 2.0));
        centerSpanData best;
        bool found = false;
        for (const centerSpanData& s : rigCaps.scopeCenterSpans) {
            if (!found || std::llabs(qint64(s.freq) - hz) < std::llabs(qint64(best.freq) - hz)) {
                best = s;
                found = true;
            }
        }
        if (found) report(funcScopeSpan, QVariant::fromValue<centerSpanData>(best));
        return true;
    }
    case funcScopeMode:
        report(funcScopeMode, QVariant::fromValue<uchar>(0));
        return true;
    case funcScopeOnOff:
    case funcScopeDataOutput:
        report(func, QVariant::fromValue<bool>(true));
        return true;
    case funcSMeter:
        report(funcSMeter, QVariant::fromValue<double>(lastSMeter));
        return true;
    case funcPowerMeter:
        report(funcPowerMeter, QVariant::fromValue<double>(transmitting ? lastPower : 0.0));
        return true;
    case funcSWRMeter:
        report(funcSWRMeter, QVariant::fromValue<double>(transmitting ? lastSwr : 1.0));
        return true;
    case funcALCMeter:
        report(funcALCMeter, QVariant::fromValue<double>(transmitting ? lastAlc : 0.0));
        return true;
    case funcCompMeter:
        report(funcCompMeter, QVariant::fromValue<double>(transmitting ? lastComp : 0.0));
        return true;
    case funcVdMeter:
        report(funcVdMeter, QVariant::fromValue<double>(lastVolts));
        return true;
    case funcIdMeter:
        report(funcIdMeter, QVariant::fromValue<double>(lastAmps));
        return true;
    default:
        return false;
    }
}

void flexCommander::sliceSet(const QString& kv)
{
    if (sliceId >= 0)
        sendCmd(QString("slice set %1 %2").arg(sliceId).arg(kv));
}

void flexCommander::applyFilterWidth(int widthHz)
{
    if (sliceId < 0 || mode.isEmpty() || modeHasFixedFilter(mode))
        return;
    const filterEdges e = edgesForWidth(mode, widthHz);
    if (e.hi > e.lo) {
        sendCmd(QString("filt %1 %2 %3").arg(sliceId).arg(e.lo).arg(e.hi));
        filterLo = e.lo;
        filterHi = e.hi;
        reportCurrent(funcFilterWidth);
    }
}

void flexCommander::setTransmit(bool on)
{
    txRequested = on;
    if (on) {
        txKeyTimer.start();
    } else {
        txPending.clear();
    }
    if (on && sliceId >= 0)
        sendCmd(QString("slice set %1 tx=1").arg(sliceId));
    sendCmd(QString("xmit %1").arg(on ? 1 : 0), [this, on](quint32 code, const QString& body) {
        if (code != 0) {
            qWarning(logRig()).noquote() << QString("Flex: xmit %1 refused (0x%2) %3").arg(on ? 1 : 0)
                                            .arg(code, 8, 16, QChar('0')).arg(body);
            report(funcTransceiverStatus, QVariant::fromValue<bool>(transmitting));
        }
    });
    if (!on)
        txAccumulator.clear();
}

void flexCommander::receiveCommand(funcs func, QVariant value, uchar receiver)
{
    Q_UNUSED(receiver)

    if (func == funcTransceiverId) {
        // servermain's probe. The radio identifies itself in its "info"
        // reply, which already loaded the caps; this only covers a probe
        // that lands after that reply.
        if (infoDone)
            identify();
        return;
    }

    if (!handshakeDone)
        return;   // nothing to talk to yet; the queue will ask again

    // GET: everything is pushed by the radio, so answer from state.
    if (!value.isValid()) {
        reportCurrent(func);
        return;
    }

    // SET. The radio does not echo a client's own changes back to it, so
    // every set also updates the local state and reports it; otherwise the
    // page's read-back 200 ms later would show the old value (NB/NR/ANF
    // stuck on while showing off, on a FLEX-6400).
    switch (func) {
    case funcSelectedFreq:
    case funcFreq:
    case funcFreqSet: {
        const freqt f = value.value<freqt>();
        if (f.Hz == 0 || sliceId < 0) return;
        sendCmd(QString("slice tune %1 %2").arg(sliceId).arg(f.Hz / 1e6, 0, 'f', 6));
        freqHz = f.Hz;
        reportCurrent(funcSelectedFreq);
        recenterPan();
        applyBandAntennas();
        return;
    }
    case funcSelectedMode:
    case funcMode:
    case funcModeSet: {
        const modeInfo m = value.value<modeInfo>();
        const QString fm = flexModeForReg(m.reg);
        if (fm.isEmpty() || sliceId < 0) return;
        const int fil = (m.filter >= 1 && m.filter <= 3) ? m.filter : filterNum;
        if (fm != mode)
            sliceSet(QString("mode=%1").arg(fm));
        filterNum = fil;
        mode = fm;
        applyFilterWidth(presetWidth(fm, fil));
        reportCurrent(funcSelectedMode);
        return;
    }
    case funcFilterWidth:
        applyFilterWidth(value.toInt());
        filterNum = filterNumForWidth(mode, filterHi - filterLo);
        reportCurrent(funcSelectedMode);
        return;
    case funcAfGain:
        sliceSet(QString("audio_level=%1").arg(to100(value.toInt())));
        audioLevel = to100(value.toInt());
        reportCurrent(funcAfGain);
        return;
    case funcRfGain:
        sliceSet(QString("agc_threshold=%1").arg(to100(value.toInt())));
        agcThreshold = to100(value.toInt());
        reportCurrent(funcRfGain);
        return;
    case funcSquelch: {
        const int lvl = to100(value.toInt());
        sliceSet(QString("squelch=%1 squelch_level=%2").arg(lvl > 0 ? 1 : 0).arg(lvl));
        squelchOn = lvl > 0;
        squelchLevel = lvl;
        reportCurrent(funcSquelch);
        return;
    }
    case funcAGC: {
        const int v = value.toInt();
        agcMode = v == 1 ? "fast" : v == 2 ? "med" : v == 3 ? "slow" : "off";
        sliceSet(QString("agc_mode=%1").arg(agcMode));
        reportCurrent(funcAGC);
        return;
    }
    case funcNoiseBlanker:
        nb = value.toInt() != 0;
        sliceSet(QString("nb=%1").arg(nb ? 1 : 0));
        reportCurrent(funcNoiseBlanker);
        return;
    case funcNoiseReduction:
        nr = value.toInt() != 0;
        sliceSet(QString("nr=%1").arg(nr ? 1 : 0));
        reportCurrent(funcNoiseReduction);
        return;
    case funcAutoNotch:
        anf = value.toInt() != 0;
        sliceSet(QString("anf=%1").arg(anf ? 1 : 0));
        reportCurrent(funcAutoNotch);
        return;
    case funcRitStatus:
        ritOn = value.toBool();
        sliceSet(QString("rit_on=%1").arg(ritOn ? 1 : 0));
        reportCurrent(funcRitStatus);
        return;
    case funcRitFreq:
        ritFreq = value.toInt();
        sliceSet(QString("rit_freq=%1").arg(ritFreq));
        reportCurrent(funcRitFreq);
        return;
    case funcAntenna: {
        const antennaInfo a = value.value<antennaInfo>();
        const QString ant = a.rx ? QString("RX_A") : (a.antenna == 1 ? QString("ANT2") : QString("ANT1"));
        if (!antList.isEmpty() && !antList.contains(ant)) {
            qInfo(logRig()) << "Flex: antenna" << ant << "not on this radio" << antList;
            return;
        }
        sliceSet(QString("rxant=%1").arg(ant));
        if (!a.rx) sliceSet(QString("txant=%1").arg(ant));
        rxAnt = ant;
        reportCurrent(funcAntenna);
        return;
    }
    case funcPreamp:
        if (!panId.isEmpty())
            sendCmd(QString("display pan set %1 rfgain=%2").arg(panId).arg(qBound(0, value.toInt(), 3) * 10));
        panRfGain = qBound(0, value.toInt(), 3) * 10;
        reportCurrent(funcPreamp);
        reportCurrent(funcAttenuator);
        return;
    case funcAttenuator:
        if (!panId.isEmpty())
            sendCmd(QString("display pan set %1 rfgain=%2").arg(panId).arg(value.toInt() ? -10 : 0));
        panRfGain = value.toInt() ? -10 : 0;
        reportCurrent(funcPreamp);
        reportCurrent(funcAttenuator);
        return;
    case funcScopeSpan: {
        const centerSpanData s = value.value<centerSpanData>();
        if (!panId.isEmpty() && s.freq > 0) {
            sendCmd(QString("display pan set %1 bandwidth=%2").arg(panId).arg(2.0 * s.freq / 1e6, 0, 'f', 6));
            // The radio does not echo a client's own bandwidth change, and the
            // scope's frequency axis and passband marker are drawn from it:
            // without this the picture zoomed while the axis kept the old span.
            panBandwidthMHz = 2.0 * s.freq / 1e6;
            reportCurrent(funcScopeSpan);
        }
        return;
    }
    case funcScopeSpeed: {
        const int v = value.toInt();
        scopeFps = v == 0 ? 25 : v == 1 ? 15 : 8;
        if (!panId.isEmpty())
            sendCmd(QString("display pan set %1 fps=%2").arg(panId).arg(scopeFps));
        return;
    }
    case funcScopeOnOff:
    case funcScopeDataOutput:
    case funcScopeMode:
        reportCurrent(func);   // always on, always centre
        return;

    case funcTransceiverStatus:
        setTransmit(value.toBool());
        return;
    case funcRFPower:
        sendCmd(QString("transmit set rfpower=%1").arg(to100(value.toInt())));
        rfPower = to100(value.toInt());
        reportCurrent(funcRFPower);
        return;
    case funcUSBModLevel:
    case funcLANModLevel:
        modLevel = qBound(0, value.toInt(), 255);
        reportCurrent(funcUSBModLevel);
        return;
    case funcMicGain:
        micLevel = to100(value.toInt());
        sendCmd(QString("transmit set miclevel=%1").arg(micLevel));
        publishDsp();
        return;
    case funcDATAOffMod:
    case funcDATA1Mod: {
        // "LAN" = DAX (browser audio), anything else = the radio's mic input.
        const rigInput in = value.value<rigInput>();
        sendCmd(QString("transmit set dax=%1").arg(in.type == inputLAN ? 1 : 0));
        daxTx = in.type == inputLAN;
        reportCurrent(funcDATAOffMod);
        return;
    }
    case funcMonitor:
        monitor = value.toInt() != 0;
        sendCmd(QString("transmit set mon=%1").arg(monitor ? 1 : 0));
        reportCurrent(funcMonitor);
        return;
    case funcMonitorGain:
        monitorGain = to100(value.toInt());
        sendCmd(QString("transmit set mon_gain_sb=%1").arg(monitorGain));
        reportCurrent(funcMonitorGain);
        return;
    case funcCompressor:
        processor = value.toInt() != 0;
        sendCmd(QString("transmit set speech_processor_enable=%1").arg(processor ? 1 : 0));
        reportCurrent(funcCompressor);
        publishDsp();
        return;
    case funcTunerStatus: {
        const int v = value.toInt();
        if (v == 2) sendCmd("atu start");
        else if (v == 0) sendCmd("atu bypass");
        return;
    }
    case funcKeySpeed:
        sendCmd(QString("cw wpm %1").arg(qBound(5, value.toInt(), 100)));
        cwSpeed = qBound(5, value.toInt(), 100);
        reportCurrent(funcKeySpeed);
        return;
    case funcCwPitch:
        cwPitch = qBound(100, value.toInt(), 6000);
        sendCmd(QString("cw pitch %1").arg(cwPitch));
        reportCurrent(funcCwPitch);
        return;
    case funcSendCW: {
        QString text = value.toString();
        if (text.isEmpty()) {
            sendCmd("cwx clear");
            return;
        }
        // CWX wire encoding (FlexLib / AetherSDR CwxModel): space -> 0x7f,
        // double quote -> 0x1e, the whole string quoted.
        text.replace(' ', QChar(0x7f));
        text.replace('"', QChar(0x1e));
        sendCmd(QString("cwx send \"%1\"").arg(text));
        return;
    }
    case funcSelectVFO:
    case funcVFOASelect:
        return;   // one slice = VFO A

    default:
        qDebug(logRig()) << "Flex: unsupported set command" << funcString[func];
        queue->del(func, 0);
        return;
    }
}

// ─── Rig identification ────────────────────────────────────────────────────

void flexCommander::determineRigCaps()
{
    rigCaps.preamps.clear();
    rigCaps.attenuators.clear();
    rigCaps.inputs.clear();
    rigCaps.scopeCenterSpans.clear();
    rigCaps.bands.clear();
    rigCaps.modes.clear();
    rigCaps.commands.clear();
    rigCaps.commandsReverse.clear();
    rigCaps.antennas.clear();
    rigCaps.filters.clear();
    rigCaps.steps.clear();
    rigCaps.memParser.clear();
    rigCaps.satParser.clear();
    rigCaps.periodic.clear();
    rigCaps.roofing.clear();
    rigCaps.scopeModes.clear();
    rigCaps.ctcss.clear();
    rigCaps.dtcs.clear();
    for (int i = meterNone; i < meterUnknown; i++) {
        rigCaps.meters[i].clear();
        rigCaps.meterLines[i] = 0.0;
    }

    // One rig file covers the whole family; take whatever FlexRadio file is
    // installed (normally rigs/FLEX-6000.rig, CI-V address 1).
    if (rigList.isEmpty()) {
        qWarning(logRig()) << "Flex: no FlexRadio rig file (Manufacturer=3) found, cannot continue";
        emit havePortError(errorType(true, "rigs/FLEX-6000.rig", "No FlexRadio rig definition file is installed."));
        return;
    }
    rigCaps.modelID = rigList.contains(1) ? 1 : rigList.begin().key();
    rigCaps.filename = rigList.value(rigCaps.modelID).path;

    QSettings settings(rigCaps.filename, QSettings::Format::IniFormat);
#if (QT_VERSION < QT_VERSION_CHECK(6,0,0))
    settings.setIniCodec("UTF-8");
#endif
    if (!settings.childGroups().contains("Rig")) {
        qWarning(logRig()) << rigCaps.filename << "Cannot be loaded!";
        return;
    }
    const float rigVersion = settings.value("Version", "0.0").toString().toFloat();
    settings.beginGroup("Rig");

    rigCaps.manufacturer = manufFlexRadio;
    rigCaps.modelName = radioModel.isEmpty() ? settings.value("Model", "FlexRadio").toString() : radioModel;
    rigCaps.rigctlModel = settings.value("RigCtlDModel", 0).toInt();
    qInfo(logRig()) << QString("Loading Rig: %0 version %1 from %2")
                           .arg(rigCaps.modelName).arg(rigVersion, 0, 'f', 2).arg(rigCaps.filename);

    rigCaps.numReceiver = settings.value("NumberOfReceivers", 1).toUInt();
    rigCaps.numVFO = settings.value("NumberOfVFOs", 1).toUInt();
    rigCaps.spectSeqMax = settings.value("SpectrumSeqMax", 0).toUInt();
    rigCaps.spectAmpMax = settings.value("SpectrumAmpMax", 160).toUInt();
    rigCaps.spectLenMax = settings.value("SpectrumLenMax", 1024).toUInt();
    rigCaps.hasSpectrum = settings.value("HasSpectrum", true).toBool();
    rigCaps.hasLan = true;
    rigCaps.hasEthernet = true;
    rigCaps.hasWiFi = false;
    rigCaps.hasQuickSplitCommand = false;
    rigCaps.hasDD = false;
    rigCaps.hasDV = false;
    rigCaps.hasTransmit = settings.value("HasTransmit", true).toBool();
    rigCaps.hasFDcomms = true;
    rigCaps.hasCommand29 = false;
    rigCaps.memGroups = 0;
    rigCaps.memories = 0;
    rigCaps.memStart = 1;
    rigCaps.memFormat.clear();
    rigCaps.satMemories = 0;
    rigCaps.satFormat.clear();

    QHash<QString, funcs> funcsLookup;
    for (int i = 0; i < funcLastFunc; i++)
        if (!funcString[i].startsWith("+"))
            funcsLookup.insert(funcString[i].toUpper(), funcs(i));

    const int numCommands = settings.beginReadArray("Commands");
    for (int c = 0; c < numCommands; c++) {
        settings.setArrayIndex(c);
        const QString type = settings.value("Type", "").toString().toUpper();
        if (!funcsLookup.contains(type)) {
            qWarning(logRig()) << "**** Function" << settings.value("Type", "").toString() << "Not Found, rig file may be out of date?";
            continue;
        }
        const funcs func = funcsLookup.value(type);
        rigCaps.commands.insert(func, funcType(func, funcString[int(func)],
                                               settings.value("String", "").toByteArray(),
                                               settings.value("Min", 0).toInt(), settings.value("Max", 0).toInt(),
                                               settings.value("PadRight", false).toBool(),
                                               settings.value("Command29", false).toBool(),
                                               settings.value("GetCommand", true).toBool(),
                                               settings.value("SetCommand", true).toBool(),
                                               settings.value("Bytes", 0).toInt(),
                                               settings.value("Admin", false).toBool()));
    }
    settings.endArray();

    const int numPeriodic = settings.beginReadArray("Periodic");
    for (int c = 0; c < numPeriodic; c++) {
        settings.setArrayIndex(c);
        const QString cmd = settings.value("Command", "").toString().toUpper();
        if (funcsLookup.contains(cmd) && rigCaps.commands.contains(funcsLookup.value(cmd))) {
            rigCaps.periodic.append(periodicType(funcsLookup.value(cmd),
                                                 settings.value("Priority", "").toString(),
                                                 priorityMap[settings.value("Priority", "").toString()],
                                                 settings.value("VFO", -1).toInt()));
        }
    }
    settings.endArray();

    const int numModes = settings.beginReadArray("Modes");
    for (int c = 0; c < numModes; c++) {
        settings.setArrayIndex(c);
        rigCaps.modes.push_back(modeInfo(rigMode_t(settings.value("Num", 0).toUInt()),
                                         settings.value("Reg", 0).toString().toUInt(), settings.value("Name", "").toString(),
                                         settings.value("Min", 0).toInt(), settings.value("Max", 0).toInt()));
    }
    settings.endArray();

    const int numSpans = settings.beginReadArray("Spans");
    for (int c = 0; c < numSpans; c++) {
        settings.setArrayIndex(c);
        rigCaps.scopeCenterSpans.push_back(centerSpanData(uchar(settings.value("Num", 0).toUInt()),
                                                          settings.value("Name", "").toString(), settings.value("Freq", 0).toUInt()));
    }
    settings.endArray();

    const int numInputs = settings.beginReadArray("Inputs");
    for (int c = 0; c < numInputs; c++) {
        settings.setArrayIndex(c);
        rigCaps.inputs.append(rigInput(inputTypes(settings.value("Num", 0).toUInt()),
                                       settings.value("Reg", 0).toString().toInt(), settings.value("Name", "").toString()));
    }
    settings.endArray();

    const int numSteps = settings.beginReadArray("Tuning Steps");
    for (int c = 0; c < numSteps; c++) {
        settings.setArrayIndex(c);
        rigCaps.steps.push_back(stepType(settings.value("Num", 0).toString().toUInt(),
                                         settings.value("Name", "").toString(), settings.value("Hz", 0ULL).toULongLong()));
    }
    settings.endArray();

    const int numPreamps = settings.beginReadArray("Preamps");
    for (int c = 0; c < numPreamps; c++) {
        settings.setArrayIndex(c);
        rigCaps.preamps.push_back(genericType(settings.value("Num", 0).toString().toUInt(), settings.value("Name", 0).toString()));
    }
    settings.endArray();

    const int numAntennas = settings.beginReadArray("Antennas");
    for (int c = 0; c < numAntennas; c++) {
        settings.setArrayIndex(c);
        rigCaps.antennas.push_back(genericType(settings.value("Num", 0).toString().toUInt(), settings.value("Name", 0).toString()));
    }
    settings.endArray();

    const int numAttenuators = settings.beginReadArray("Attenuators");
    for (int c = 0; c < numAttenuators; c++) {
        settings.setArrayIndex(c);
        rigCaps.attenuators.push_back(genericType(settings.value("Num", 0).toString().toUInt(), settings.value("Name", 0).toString()));
    }
    settings.endArray();

    const int numFilters = settings.beginReadArray("Filters");
    for (int c = 0; c < numFilters; c++) {
        settings.setArrayIndex(c);
        rigCaps.filters.push_back(filterType(settings.value("Num", 0).toString().toUInt(),
                                             settings.value("Name", "").toString(), settings.value("Modes", 0).toUInt()));
    }
    settings.endArray();

    const int numBands = settings.beginReadArray("Bands");
    for (int c = 0; c < numBands; c++) {
        settings.setArrayIndex(c);
        const availableBands band = availableBands(settings.value("Num", 0).toInt());
        rigCaps.bands.push_back(bandType(settings.value("Region", "").toString(), band,
                                         uchar(settings.value("BSR", 0).toInt()),
                                         settings.value("Start", 0ULL).toULongLong(), settings.value("End", 0ULL).toULongLong(),
                                         settings.value("Range", 0.0).toDouble(), settings.value("MemoryGroup", -1).toInt(),
                                         char(settings.value("Bytes", 5).toInt()), settings.value("Antennas", true).toBool(),
                                         settings.value("Power", 0.0f).toFloat(),
                                         QColor(settings.value("Color", "#00000000").toString()),
                                         settings.value("Name", "None").toString(), settings.value("Offset", 0).toLongLong()));
        rigCaps.bsr[band] = 0;
    }
    settings.endArray();

    const int numMeters = settings.beginReadArray("Meters");
    for (int c = 0; c < numMeters; c++) {
        settings.setArrayIndex(c);
        const QString meterName = settings.value("Meter", "None").toString();
        for (int i = meterNone + 1; i < meterUnknown; i++) {
            if (meterName == meterString[i]) {
                if (settings.value("RedLine", false).toBool())
                    rigCaps.meterLines[i] = settings.value("ActualVal", 0.0).toDouble();
                rigCaps.meters[i].insert(settings.value("RigVal", 0).toInt(), settings.value("ActualVal", 0.0).toDouble());
                break;
            }
        }
    }
    settings.endArray();

    const int numScopeModes = settings.beginReadArray("ScopeModes");
    for (int c = 0; c < numScopeModes; c++) {
        settings.setArrayIndex(c);
        rigCaps.scopeModes.push_back(genericType(settings.value("Num", 0).toString().toUInt(), settings.value("Name", 0).toString()));
    }
    settings.endArray();
    settings.endGroup();

    memcpy(rigCaps.guid, this->guid, GUIDLEN);
    haveRigCaps = true;
    capsLoaded = true;
    queue->setRigCaps(&rigCaps);

    QList<radio_cap_packet> radios;
    radio_cap_packet r {};
    r.civ = rigCaps.modelID;
    r.baudrate = 0;
#ifdef Q_OS_WINDOWS
    strncpy_s(r.name, rigCaps.modelName.toLocal8Bit(), sizeof(r.name) - 1);
#else
    strncpy(r.name, rigCaps.modelName.toLocal8Bit(), sizeof(r.name) - 1);
#endif
    radios.append(r);
    emit requestRadioSelection(radios);
    emit setRadioUsage(0, true, true, QString("wfweb"), radioAddress.toString());

    qInfo(logRig()) << "---Rig FOUND" << rigCaps.modelName;
    emit discoveredRigID(rigCaps);
    emit haveRigID(rigCaps);

    // The pan may have been created before the caps told us its size.
    if (!panId.isEmpty()) {
        panConfigured = false;
        configurePan();
    }
    // Push everything we already know so the page fills in at once.
    for (funcs f : {funcSelectedFreq, funcSelectedMode, funcFilterWidth, funcAfGain, funcRfGain, funcRFPower,
                    funcSquelch, funcAGC, funcNoiseBlanker, funcNoiseReduction, funcAutoNotch, funcPreamp,
                    funcAttenuator, funcAntenna, funcTransceiverStatus, funcKeySpeed, funcDATAOffMod,
                    funcScopeSpan, funcScopeMode, funcUSBModLevel, funcMonitor, funcCompressor})
        reportCurrent(f);
}

// ─── Unused rigCommander slots ─────────────────────────────────────────────

void flexCommander::setPTTType(pttType_t ptt) { Q_UNUSED(ptt) }

void flexCommander::powerOn()
{
    // A Flex has no remote power-on over the API.
    qInfo(logRig()) << "Flex: power on is not available over the network";
}

void flexCommander::powerOff()
{
    // "radio shutdown" exists but turning a remote station off from a web
    // page by accident is worse than not offering it.
    qInfo(logRig()) << "Flex: power off from wfweb is disabled; use the radio or SmartSDR";
}

void flexCommander::setRigID(quint16 rigID)
{
    // servermain calls this instead of probing when a CI-V address was given.
    Q_UNUSED(rigID)
    if (infoDone)
        identify();
}

void flexCommander::identify()
{
    if (rigIdentified)
        return;
    rigIdentified = true;
    determineRigCaps();
}
void flexCommander::setCIVAddr(quint16 civAddr) { Q_UNUSED(civAddr) }
void flexCommander::handleNewData(const QByteArray& data) { Q_UNUSED(data) }
void flexCommander::receiveBaudRate(quint32 baudrate) { Q_UNUSED(baudrate) }

// ─── EQ and TX audio processing (FUNC > EQ, FUNC > PROC) ─────────────────────

namespace {
const char* const kEqBandKeys[8] = { "63Hz", "125Hz", "250Hz", "500Hz", "1000Hz", "2000Hz", "4000Hz", "8000Hz" };
}

void flexCommander::handleEqStatus(bool tx, const QMap<QString, QString>& kvs)
{
    eqState& eq = tx ? eqTx : eqRx;
    if (kvs.contains("mode")) eq.on = truthy(kvs.value("mode"));
    for (int i = 0; i < 8; ++i) {
        const QString key = QLatin1String(kEqBandKeys[i]);
        // The status spells the keys as the commands do ("63Hz"); accept any case.
        for (auto it = kvs.constBegin(); it != kvs.constEnd(); ++it)
            if (it.key().compare(key, Qt::CaseInsensitive) == 0)
                eq.bands[i] = qBound(-10, it.value().toInt(), 10);
    }
    publishDsp();
}

// Coalesced: a slider drag or a status burst sends one snapshot per 50 ms.
void flexCommander::publishDsp()
{
    if (dspPublishTimer == nullptr) {
        dspPublishTimer = new QTimer(this);
        dspPublishTimer->setSingleShot(true);
        dspPublishTimer->setInterval(50);
        connect(dspPublishTimer, &QTimer::timeout, this, [this]() {
            auto eqJson = [](const eqState& e) {
                QJsonObject o;
                o["on"] = e.on;
                QJsonArray b;
                for (int v : e.bands) b.append(v);
                o["bands"] = b;
                return o;
            };
            QJsonObject st;
            st["rxEq"] = eqJson(eqRx);
            st["txEq"] = eqJson(eqTx);
            st["proc"] = processor;
            st["procLevel"] = procLevel;
            st["dexp"] = dexpOn;
            st["dexpLevel"] = dexpLevel;
            st["micLevel"] = micLevel;
            st["txLo"] = txFilterLo;
            st["txHi"] = txFilterHi;
            st["rxAnt"] = rxAnt;
            st["txAnt"] = txAnt;
            st["rxAntList"] = QJsonArray::fromStringList(rxAntList);
            st["txAntList"] = QJsonArray::fromStringList(txAntList);
            emit flexDspState(st);
        });
    }
    if (!dspPublishTimer->isActive()) dspPublishTimer->start();
}

void flexCommander::setFlexDsp(QString key, int index, int value)
{
    if (tcp == nullptr || !handshakeDone) return;
    const bool rx = key.startsWith(QLatin1String("rx"));
    eqState& eq = rx ? eqRx : eqTx;
    const QString eqObj = rx ? QStringLiteral("eq rxsc") : QStringLiteral("eq txsc");

    if (key == "rxEqOn" || key == "txEqOn") {
        eq.on = value != 0;
        sendCmd(QString("%1 mode=%2").arg(eqObj).arg(eq.on ? 1 : 0));
    } else if (key == "rxEqBand" || key == "txEqBand") {
        if (index < 0 || index > 7) return;
        eq.bands[index] = qBound(-10, value, 10);
        sendCmd(QString("%1 %2=%3").arg(eqObj, QLatin1String(kEqBandKeys[index])).arg(eq.bands[index]));
    } else if (key == "rxEqFlat" || key == "txEqFlat") {
        QStringList parts;
        for (int i = 0; i < 8; ++i) {
            eq.bands[i] = 0;
            parts << QString("%1=0").arg(QLatin1String(kEqBandKeys[i]));
        }
        sendCmd(QString("%1 %2").arg(eqObj, parts.join(' ')));
    } else if (key == "proc") {
        processor = value != 0;
        sendCmd(QString("transmit set speech_processor_enable=%1").arg(processor ? 1 : 0));
        reportCurrent(funcCompressor);
    } else if (key == "procLevel") {
        procLevel = qBound(0, value, 2);
        sendCmd(QString("transmit set speech_processor_level=%1").arg(procLevel));
    } else if (key == "dexp") {
        dexpOn = value != 0;
        sendCmd(QString("transmit set compander=%1").arg(dexpOn ? 1 : 0));
    } else if (key == "dexpLevel") {
        dexpLevel = qBound(0, value, 100);
        sendCmd(QString("transmit set compander_level=%1").arg(dexpLevel));
    } else if (key == "micLevel") {
        micLevel = qBound(0, value, 100);
        sendCmd(QString("transmit set miclevel=%1").arg(micLevel));
    } else if (key == "rxAnt" || key == "txAnt") {
        // FUNC > ANT: value is an index into the slice's own list.
        const QStringList& list = key == "rxAnt" ? rxAntList : txAntList;
        if (value < 0 || value >= list.size()) return;
        if (key == "rxAnt") setSliceAntennas(list.at(value), QString());
        else setSliceAntennas(QString(), list.at(value));
    } else if (key == "txLo" || key == "txHi") {
        // The radio keeps at least 50 Hz between the edges; so do we.
        if (key == "txLo") txFilterLo = qBound(0, value, txFilterHi - 50);
        else txFilterHi = qBound(txFilterLo + 50, value, 10000);
        sendCmd(QString("transmit set filter_low=%1 filter_high=%2").arg(txFilterLo).arg(txFilterHi));
    } else {
        qWarning(logRig()) << "Flex: unknown DSP setting" << key;
        return;
    }
    publishDsp();
}

// ─── Antennas (FUNC > ANT) and the 2 m transverter port ──────────────────────

void flexCommander::setSliceAntennas(const QString& rx, const QString& tx)
{
    if (sliceId < 0) return;
    if (!rx.isEmpty() && rx != rxAnt) {
        sliceSet(QString("rxant=%1").arg(rx));
        rxAnt = rx;                 // not echoed to the client that set it
        reportCurrent(funcAntenna);
    }
    if (!tx.isEmpty() && tx != txAnt) {
        sliceSet(QString("txant=%1").arg(tx));
        txAnt = tx;
    }
    publishDsp();
}

void flexCommander::applyBandAntennas()
{
    const bool in2m = freqHz >= 144000000ULL && freqHz <= 148000000ULL;
    if (in2m == on2m) return;
    // The transverter port as this radio names it (XVTA on a FLEX-6400).
    auto xvtr = [](const QStringList& l) {
        if (l.contains(QStringLiteral("XVTA"))) return QStringLiteral("XVTA");
        for (const QString& a : l) if (a.startsWith(QLatin1String("XVT"))) return a;
        return QString();
    };
    const QString rx = xvtr(rxAntList), tx = xvtr(txAntList);
    if (in2m) {
        if (rx.isEmpty() && tx.isEmpty()) return;   // lists not in yet; retried on ant_list
        on2m = true;
        hfRxAnt = rxAnt;
        hfTxAnt = txAnt;
        qInfo(logRig()).noquote() << "Flex: 2 m - RX" << (rx.isEmpty() ? rxAnt : rx) << "TX" << (tx.isEmpty() ? txAnt : tx);
        setSliceAntennas(rx, tx);
    } else {
        on2m = false;
        // Back to HF: the antennas it had before 2 m, unless those were the
        // transverter port too (then the radio's first ordinary antenna).
        auto hf = [](const QString& saved, const QStringList& l) {
            if (!saved.isEmpty() && !saved.startsWith(QLatin1String("XVT"))) return saved;
            for (const QString& a : l) if (a.startsWith(QLatin1String("ANT"))) return a;
            return QString();
        };
        const QString r = hf(hfRxAnt, rxAntList), t = hf(hfTxAnt, txAntList);
        qInfo(logRig()).noquote() << "Flex: left 2 m - RX" << r << "TX" << t;
        setSliceAntennas(r, t);
    }
}
