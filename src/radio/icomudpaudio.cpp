#include "icomudpaudio.h"
#include "logcategories.h"

static inline quint32 makeExtendedSeq(quint32 prefix, quint16 seq16) {
    return (prefix << 16) | quint32(seq16);
}

// Audio stream
icomUdpAudio::icomUdpAudio(QHostAddress local, QHostAddress ip, quint16 audioPort, quint16 lport, audioSetup rxSetup, audioSetup txSetup)
{
    qInfo(logUdp()) << "Starting icomUdpAudio";
    this->localIP = local;
    this->port = audioPort;
    this->radioIP = ip;
    this->rxSetup = rxSetup;
    this->txSetup = txSetup;

    if (txSetup.sampleRate == 0) {
        enableTx = false;
    }

    init(lport); // Perform connection

    QUdpSocket::connect(udp, &QUdpSocket::readyRead, this, &icomUdpAudio::dataReceived);
 
    startAudio();

    watchdogTimer = new QTimer(this);
    connect(watchdogTimer, &QTimer::timeout, this, &icomUdpAudio::watchdog);
    watchdogTimer->start(WATCHDOG_PERIOD);

    areYouThereTimer = new QTimer(this);
    connect(areYouThereTimer, &QTimer::timeout, this, std::bind(&icomUdpBase::sendControl, this, false, 0x03, 0));
    areYouThereTimer->start(AREYOUTHERE_PERIOD);
}

icomUdpAudio::~icomUdpAudio()
{
    if (rxAudioThread != Q_NULLPTR) {
        qDebug(logUdp()) << "Stopping rxaudio thread";
        rxAudioThread->quit();
        rxAudioThread->wait();
    }

    if (txAudioThread != Q_NULLPTR) {
        qDebug(logUdp()) << "Stopping txaudio thread";
        txAudioThread->quit();
        txAudioThread->wait();
    }
    qDebug(logUdp()) << "icomUdpHandler successfully closed";
}

void icomUdpAudio::watchdog()
{
    static bool alerted = false;
    if (lastReceived.msecsTo(QTime::currentTime()) > 30000)
    {
        if (!alerted) {
            /* Just log it at the moment, maybe try signalling the control channel that it needs to
                try requesting civ/audio again? */

            qInfo(logUdp()) << " Audio Watchdog: no audio data received for 30s, restart required? last:" << lastReceived  ;
            alerted = true;
            if (rxAudioThread != Q_NULLPTR) {
                qDebug(logUdp()) << "Stopping rxaudio thread";
                rxAudioThread->quit();
                rxAudioThread->wait();
                rxAudioThread = Q_NULLPTR;
                rxaudio = Q_NULLPTR;
                rxAudioStarted = false;   // the next audio starts the local playback again
            }

            if (txAudioThread != Q_NULLPTR) {
                qDebug(logUdp()) << "Stopping txaudio thread";
                txAudioThread->quit();
                txAudioThread->wait();
                txAudioThread = Q_NULLPTR;
                txaudio = Q_NULLPTR;
            }            
        }
    }
    else
    {
        alerted = false;
    }
}


void icomUdpAudio::sendTxAudio()
{
    if (txaudio == Q_NULLPTR) {
        return;
    }

}

void icomUdpAudio::receiveAudioData(audioPacket audio) {
    // No TX on this stream (the server offered none), or no source: drop.
    if (!enableTx || (txaudio == Q_NULLPTR && !txSetup.externalTx)) {
        return;
    }
    if (audio.data.isEmpty()) return;
    lastExternalTx.restart();
    sendTxPayload(audio.data, false);
}

// The browser only sends audio while keyed, so the TX stream used to start
// from nothing on every over. wfview captures its mic all the time and the
// RS-BA1 / wfview server always has a running TX stream; keep ours running
// the same way with silence between the browser's frames (punch list 72).
// LPCM 16-bit mono (codec 0x04, the default) only - zeros are not silence
// in the other codecs.
void icomUdpAudio::sendTxKeepalive()
{
    if (!enableTx || !txSetup.externalTx || txSetup.codec != 0x04 || !txSetup.sampleRate) return;
    // Real audio is flowing, or paused for less than a jitter hiccup: a
    // silence inserted mid-over would only push the voice later.
    if (lastExternalTx.isValid() && lastExternalTx.elapsed() < 250) {
        keepaliveClock.invalidate();
        return;
    }
    // Paced by the clock, not by the timer's ticks (15.6 ms grain on
    // Windows): 20 ms of silence for every 20 ms that has passed.
    if (!keepaliveClock.isValid()) { keepaliveClock.start(); keepaliveSentMs = 0; }
    const QByteArray chunk(int(txSetup.sampleRate / 50) * 2, '\0');
    int n = 0;
    while (keepaliveClock.elapsed() - keepaliveSentMs >= 20 && n++ < 5) {
        sendTxPayload(chunk, true);
        keepaliveSentMs += 20;
    }
    if (keepaliveClock.elapsed() - keepaliveSentMs > 200) keepaliveSentMs = keepaliveClock.elapsed();   // a stall: do not burst
}

void icomUdpAudio::sendTxPayload(const QByteArray& data, bool silence)
{
    if (remoteId == 0) return;   // stream not open yet (see icomUdpCivData::send)
    audioPacket audio;
    audio.data = data;
    {
        int len = 0;

        while (len < audio.data.length()) {
            QByteArray partial = audio.data.mid(len, 1364);
            audio_packet p;
            memset(p.packet, 0x0, sizeof(p)); // We can't be sure it is initialized with 0x00!
            p.len = (quint32)sizeof(p) + partial.length();
            p.sentid = myId;
            p.rcvdid = remoteId;
            if (partial.length() == 0xa0) {
                p.ident = 0x9781;
            }
            else {
                p.ident = 0x0080; // TX audio is always this?
            }
            p.datalen = (quint16)qToBigEndian((quint16)partial.length());
            p.sendseq = (quint16)qToBigEndian((quint16)sendAudioSeq); // THIS IS BIG ENDIAN!
            QByteArray tx = QByteArray::fromRawData((const char*)p.packet, sizeof(p));
            tx.append(partial);
            len = len + partial.length();
            //qInfo(logUdp()) << "Sending audio packet length: " << tx.length();
            sendTrackedPacket(tx);
            sendAudioSeq++;
            ++txReportPackets;
            txReportBytes += partial.length();
            if (silence) txReportSilentBytes += partial.length();
        }
        // What actually left for the radio / RS-BA1 server, every 5 s while
        // sending (punch list 72: the log could not show it before).
        if (!txReportTimer.isValid()) txReportTimer.start();
        if (txReportTimer.elapsed() >= 5000) {
            const qint64 voice = qint64(txReportBytes - txReportSilentBytes);
            // Only while voice went out: the keepalive alone is not news.
            if (voice > 0)
                qInfo(logUdp()).noquote() << QString("LAN TX audio: %1 packets, %2 bytes (%3 ms of voice, the rest silence) sent to %4:%5 in the last %6 s")
                    .arg(txReportPackets).arg(txReportBytes)
                    .arg(txSetup.sampleRate ? voice * 1000 / (2 * qint64(txSetup.sampleRate)) : 0)
                    .arg(radioIP.toString()).arg(port).arg(txReportTimer.elapsed() / 1000);
            txReportPackets = 0;
            txReportBytes = 0;
            txReportSilentBytes = 0;
            txReportTimer.restart();
        }
    }
}

void icomUdpAudio::changeLatency(quint16 value)
{
    emit haveChangeLatency(value);
}

void icomUdpAudio::setVolume(quint8 value)
{
    emit haveSetVolume(value);
}

void icomUdpAudio::getRxLevels(quint16 amplitudePeak, quint16 amplitudeRMS, quint16 latency, quint16 current, bool under, bool over) {

    emit haveRxLevels(amplitudePeak, amplitudeRMS, latency, current, under, over);
}

void icomUdpAudio::getTxLevels(quint16 amplitudePeak, quint16 amplitudeRMS, quint16 latency, quint16 current, bool under, bool over) {
    emit haveTxLevels(amplitudePeak, amplitudeRMS, latency, current, under, over);
}

void icomUdpAudio::dataReceived()
{

    while (udp->hasPendingDatagrams()) {
        QNetworkDatagram datagram = udp->receiveDatagram();
        QByteArray r = datagram.data();

        switch (r.length())
        {
        case (16): // Response to control packet handled in icomUdpBase
        {
            //control_packet_t in = (control_packet_t)r.constData();
            break;
        }
        default:
            {
                /* Audio packets start as follows:
                        PCM 16bit and PCM8/uLAW stereo: 0x44,0x02 for first packet and 0x6c,0x05 for second.
                        uLAW 8bit/PCM 8bit 0xd8,0x03 for all packets
                        PCM 16bit stereo 0x6c,0x05 first & second 0x70,0x04 third


                */
            control_packet_t in = (control_packet_t)r.constData();

            if (in->type != 0x01 && in->len >= 0x20) {

                if (in->seq == 0) {
                    seqPrefix++;
                }

                if (!rxAudioStarted)
                    startAudio();

                const int excess = pingLatenessMs - (pingBaselineMs + rxSetup.latency);

                if (excess > 0) {
                    qDebug(logUdp()) << "Audio latency high:"
                                     << "lateness" << pingLatenessMs
                                     << "baseline" << pingBaselineMs
                                     << "excess" << excess;

                    if (++latencyCounter > 5) {
                        latencyCounter = 0;
                        if (rxSetup.externalRx) {
                            // The browser's jitter buffer (rx-jitter.js) catches up
                            // smoothly - resampling, a crossfaded skip at a quiet
                            // moment. Dropping a packet here only cut a hard 20 ms
                            // hole into the audio (112 in 47 min over RS-BA1 on
                            // the WAN, punch list 75a). Logged, not dropped.
                            if (!lateLogTimer.isValid() || lateLogTimer.elapsed() > 10000) {
                                qInfo(logUdp()).noquote() << QString("LAN audio: the network is running %1 ms late "
                                                                     "(the browser's buffer absorbs it)").arg(excess);
                                lateLogTimer.restart();
                            }
                        } else {
                            qInfo(logUdp()) << "Latency sustained -> flushing audio";
                            //flushAudio();   // clear queue / reset decoder
                            break;
                        }
                    }
                } else {
                    latencyCounter = 0;
                }
                audioPacket tempAudio;
                tempAudio.seq  = quint32(seqPrefix << 16) | quint32(in->seq) ;
                tempAudio.time = QTime::currentTime();
                tempAudio.sent = 0;
                tempAudio.data = r.mid(0x18);
                emit haveAudioData(tempAudio);
                lastReceived = QTime::currentTime();
            }
            break;
        }
        }

        icomUdpBase::dataReceived(r); // Call parent function to process the rest.
        r.clear();
        datagram.clear();
    }
}

void icomUdpAudio::startAudio() {
    rxAudioStarted = true;

    // The browser is the receive sink in the web build: no local playback
    // device (wfview's desktop path played the rig out of the server PC's
    // default output - "Output Cannot initialize audio device" when there
    // is none). Punch list 75b. -S keeps it.
    if (rxSetup.externalRx) {
        qInfo(logUdp()) << "icomUdpAudio: RX audio to the web server only (no local playback device)";
    }
    else {
    if (rxSetup.type == qtAudio) {
        rxaudio = new audioHandlerQtOutput();
    }
    else if (rxSetup.type == portAudio) {
        rxaudio = new audioHandlerPaOutput();
    }
    else if (rxSetup.type == rtAudio) {
        rxaudio = new audioHandlerRtOutput();
    }
#ifndef BUILD_WFSERVER
    else if (rxSetup.type == tciAudio) {
        rxaudio = new audioHandlerTciOutput();
    }
#endif
    else
    {
        qCritical(logAudio()) << "Unsupported Receive Audio Handler selected!";
        return;
    }

    rxAudioThread = new QThread(this);
    rxAudioThread->setObjectName("rxAudio()");

    rxaudio->moveToThread(rxAudioThread);

    rxAudioThread->start(QThread::TimeCriticalPriority);

    connect(this, SIGNAL(setupRxAudio(audioSetup)), rxaudio, SLOT(init(audioSetup)));

    // signal/slot not currently used.
    connect(this, SIGNAL(haveAudioData(audioPacket)), rxaudio, SLOT(incomingAudio(audioPacket)));
    connect(this, SIGNAL(haveChangeLatency(quint16)), rxaudio, SLOT(changeLatency(quint16)));
    connect(this, SIGNAL(haveSetVolume(quint8)), rxaudio, SLOT(setVolume(quint8)));
    connect(rxaudio, SIGNAL(haveLevels(quint16, quint16, quint16, quint16, bool, bool)), this, SLOT(getRxLevels(quint16, quint16, quint16, quint16, bool, bool)));
    connect(rxAudioThread, SIGNAL(finished()), rxaudio, SLOT(deleteLater()));
    }

    sendControl(false, 0x03, 0x00); // First connect packet

    // startAudio() runs again after the watchdog stopped the RX thread (30 s
    // without audio): one ping timer and one TX keepalive, not one more each
    // time - a second keepalive doubled the TX stream.
    if (!pingTimer) {
        pingTimer = new QTimer(this);
        connect(pingTimer, &QTimer::timeout, this, &icomUdpBase::sendPing);
        pingTimer->start(PING_PERIOD); // send ping packets every 100ms
    }

    if (enableTx && txSetup.externalTx) {
        if (!txKeepaliveTimer) {
            qInfo(logUdp()) << "icomUdpAudio: TX audio from the web server (no local capture device)"
                            << (txSetup.codec == 0x04 ? "- stream kept running with silence between overs, as wfview's" : "");
            txKeepaliveTimer = new QTimer(this);
            txKeepaliveTimer->setTimerType(Qt::PreciseTimer);
            connect(txKeepaliveTimer, &QTimer::timeout, this, &icomUdpAudio::sendTxKeepalive);
            txKeepaliveTimer->start(20);
        }
    }
    else if (enableTx) {
        if (txSetup.type == qtAudio) {
            txaudio = new audioHandlerQtInput();
        }
        else if (txSetup.type == portAudio) {
            txaudio = new audioHandlerPaInput();
        }
        else if (txSetup.type == rtAudio) {
            txaudio = new audioHandlerRtInput();
        }
#ifndef BUILD_WFSERVER
        else if (txSetup.type == tciAudio) {
            txaudio = new audioHandlerTciInput();
        }
#endif
        else
        {
            qCritical(logAudio()) << "Unsupported Transmit Audio Handler selected!";
            return;
        }

        txAudioThread = new QThread(this);
        txAudioThread->setObjectName("txAudio()");

        txaudio->moveToThread(txAudioThread);

        txAudioThread->start(QThread::TimeCriticalPriority);

        connect(this, SIGNAL(setupTxAudio(audioSetup)), txaudio, SLOT(init(audioSetup)));
        connect(txaudio, SIGNAL(haveAudioData(audioPacket)), this, SLOT(receiveAudioData(audioPacket)));
        connect(txaudio, SIGNAL(haveLevels(quint16, quint16, quint16, quint16, bool, bool)), this, SLOT(getTxLevels(quint16, quint16, quint16, quint16, bool, bool)));

        connect(txAudioThread, SIGNAL(finished()), txaudio, SLOT(deleteLater()));
        emit setupTxAudio(txSetup);
    }

    if (rxaudio) emit setupRxAudio(rxSetup);

}

