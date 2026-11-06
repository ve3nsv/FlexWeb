// Copyright 2020-2024 Phil Taylor M0VSE
// This code is heavily based on "Kappanhang" by HA2NON, ES1AKOS and W6EL!

#include "icomudphandler.h"

#include <QElapsedTimer>
#include <memory>
#include "logcategories.h"

icomUdpHandler::icomUdpHandler(udpPreferences prefs, audioSetup rx, audioSetup tx) :
    lanHost(prefs.ipAddress),
    wantRadio(prefs.radioName.trimmed()),
    controlPort(prefs.controlLANPort),
    civPort(0),
    audioPort(0),
    civLocalPort(0),
    audioLocalPort(0),
    rxSetup(rx),
    txSetup(tx)
{
    this->port = this->controlPort;
    this->username = prefs.username;
    this->password = prefs.password;
    this->compName = prefs.clientName.mid(0,8) + "-wfweb";

    if (prefs.waterfallFormat == 2)
    {
        splitWf = true;
    }
    else
    {
        splitWf = false;
    }
    qInfo(logUdp()) << "Starting icomUdpHandler user:" << username << " rx latency:" << rxSetup.latency  << " tx latency:" << txSetup.latency << " rx sample rate: " << rxSetup.sampleRate <<
        " rx codec: " << rxSetup.codec << " tx sample rate: " << txSetup.sampleRate << " tx codec: " << txSetup.codec;

    // Try to set the IP address, if it is a hostname then perform a DNS lookup.
    if (!radioIP.setAddress(prefs.ipAddress))
    {
        QHostInfo remote = QHostInfo::fromName(prefs.ipAddress);
        for(const auto &addr: remote.addresses())
        {
            if (addr.protocol() == QAbstractSocket::IPv4Protocol) {
                radioIP = addr;
                qInfo(logUdp()) << "Got IP Address :" << prefs.ipAddress << ": " << addr.toString();
                break;
            }
        }
        if (radioIP.isNull())
        { 
            qInfo(logUdp()) << "Error obtaining IP Address for :" << prefs.ipAddress << ": " << remote.errorString();
            return;
        }
    }
    
    // Convoluted way to find the external IP address, there must be a better way????
    QString localhostname = QHostInfo::localHostName();
    QList<QHostAddress> hostList = QHostInfo::fromName(localhostname).addresses();
    for(const auto &address: hostList)
    {
        if (address.protocol() == QAbstractSocket::IPv4Protocol && address.isLoopback() == false)
        {
            localIP = QHostAddress(address.toString());
        }
    }

}

void icomUdpHandler::init()
{
    icomUdpBase::init(0); // Perform UDP socket initialization.

    // Connect socket to my dataReceived function.
    QUdpSocket::connect(udp, &QUdpSocket::readyRead, this, &icomUdpHandler::dataReceived);

    /*
        Connect various timers
    */
    tokenTimer = new QTimer(this);
    areYouThereTimer = new QTimer(this);
    pingTimer = new QTimer(this);
    idleTimer = new QTimer(this);

    connect(tokenTimer, &QTimer::timeout, this, std::bind(&icomUdpHandler::sendToken, this, 0x05));
    connect(areYouThereTimer, &QTimer::timeout, this, std::bind(&icomUdpBase::sendControl, this, false, 0x03, 0));
    connect(pingTimer, &QTimer::timeout, this, &icomUdpBase::sendPing);
    connect(idleTimer, &QTimer::timeout, this, std::bind(&icomUdpBase::sendControl, this, true, 0, 0));

    // Start sending are you there packets - will be stopped once "I am here" received
    areYouThereTimer->start(AREYOUTHERE_PERIOD);

    // No "I am here": say so in the log (after 5 s, 15 s, 30 s, then every
    // 30 s) with what to check, rather than staying silent - a closed port
    // forward or firewall looks like nothing at all happening.
    {
        auto since = std::make_shared<QElapsedTimer>();
        since->start();
        auto next = std::make_shared<qint64>(5000);
        connect(areYouThereTimer, &QTimer::timeout, this, [this, since, next]() {
            const qint64 t = since->elapsed();
            if (t < *next) return;
            *next = *next < 15000 ? 15000 : *next + 15000 * (*next < 30000 ? 1 : 2);
            qWarning(logUdp()).noquote() << QString("LAN: no answer from %1:%2 (%3) after %4 s - check the address, "
                                                    "that UDP %2-%5 reaches the radio or RS-BA1 server (port forwarding, "
                                                    "firewall), and that its remote access is enabled")
                .arg(lanHost).arg(controlPort).arg(radioIP.toString()).arg(t / 1000)
                .arg(controlPort + 2);
        });
    }
}

icomUdpHandler::~icomUdpHandler()
{
    if (streamOpened) {
        if (audio != Q_NULLPTR) {
            delete audio;
            audio = Q_NULLPTR;
        }

        if (civ != Q_NULLPTR) {
            delete civ;
            civ = Q_NULLPTR;
        }

        qInfo(logUdp()) << "Sending token removal packet";
        sendToken(0x01);
    }
}


void icomUdpHandler::changeLatency(quint16 value)
{
    emit haveChangeLatency(value);
}

void icomUdpHandler::setVolume(quint8 value)
{
    emit haveSetVolume(value);
}

void icomUdpHandler::receiveFromCivStream(QByteArray data)
{
    // The server says which CI-V address the chosen radio has; a frame from
    // another radio on its stream means the server's entry is wired to the
    // wrong COM port (RS-BA1 Remote Utility: the IC-9700 entry carrying the
    // IC-7300, user's 1.0.125 log). Said once per session, loudly: wfweb
    // would otherwise load the other radio and look as if it chose wrong.
    if (!civCrossWarned && civId != 0) {
        for (int i = data.indexOf("\xFE\xFE"); i >= 0 && i + 3 < data.size(); i = data.indexOf("\xFE\xFE", i + 2)) {
            const quint8 from = quint8(data.at(i + 3));
            if (from == 0xFE || from == 0xE0 || from == 0xE1 || from == 0x00) continue;
            if (from != civId) {
                civCrossWarned = true;
                qWarning(logUdp()).noquote() << QString("LAN: \"%1\" (CI-V 0x%2 on the server) answers as CI-V 0x%3 - "
                                                        "the server's %1 entry is connected to another radio's COM port "
                                                        "(Remote Utility: check that radio's COM port / CI-V settings)")
                    .arg(devName).arg(civId, 2, 16, QChar('0')).arg(from, 2, 16, QChar('0'));
            }
            break;
        }
    }
    emit haveDataFromPort(data);
}

void icomUdpHandler::receiveAudioData(const audioPacket &data)
{
    emit haveAudioData(data);
}

void icomUdpHandler::receiveExternalTxAudio(const audioPacket &data)
{
    if (audio != Q_NULLPTR) {
        audio->receiveAudioData(data);
    }
}

void icomUdpHandler::receiveDataFromUserToRig(QByteArray data)
{
    if (civ != Q_NULLPTR)
    {
        civ->send(data);
    }
}

void icomUdpHandler::getRxLevels(quint16 amplitudePeak, quint16 amplitudeRMS,quint16 latency,quint16 current, bool under, bool over) {
    status.rxAudioLevel = amplitudePeak;
    status.rxLatency = latency;
    status.rxCurrentLatency =   qint32(current);
    status.rxUnderrun = under;
    status.rxOverrun = over;
    audioLevelsRxPeak[(audioLevelsRxPosition)%audioLevelBufferSize] = amplitudePeak;
    audioLevelsRxRMS[(audioLevelsRxPosition)%audioLevelBufferSize] = amplitudeRMS;

    if((audioLevelsRxPosition)%4 == 0)
    {
        // calculate mean and emit signal
        quint8 meanPeak = findMax(audioLevelsRxPeak);
        quint8 meanRMS = findMean(audioLevelsRxRMS);
        networkAudioLevels l;
        l.haveRxLevels = true;
        l.rxAudioPeak = meanPeak;
        l.rxAudioRMS = meanRMS;
        emit haveNetworkAudioLevels(l);
    }
    audioLevelsRxPosition++;
}

void icomUdpHandler::getTxLevels(quint16 amplitudePeak, quint16 amplitudeRMS ,quint16 latency, quint16 current, bool under, bool over) {
    status.txAudioLevel = amplitudePeak;
    status.txLatency = latency;
    status.txCurrentLatency = qint32(current);
    status.txUnderrun = under;
    status.txOverrun = over;
    audioLevelsTxPeak[(audioLevelsTxPosition)%audioLevelBufferSize] = amplitudePeak;
    audioLevelsTxRMS[(audioLevelsTxPosition)%audioLevelBufferSize] = amplitudeRMS;

    if((audioLevelsTxPosition)%4 == 0)
    {
        // calculate mean and emit signal
        quint8 meanPeak = findMax(audioLevelsTxPeak);
        quint8 meanRMS = findMean(audioLevelsTxRMS);
        networkAudioLevels l;
        l.haveTxLevels = true;
        l.txAudioPeak = meanPeak;
        l.txAudioRMS = meanRMS;
        emit haveNetworkAudioLevels(l);
    }
    audioLevelsTxPosition++;
}

quint8 icomUdpHandler::findMean(quint8 *data)
{
    unsigned int sum=0;
    for(int p=0; p < audioLevelBufferSize; p++)
    {
        sum += data[p];
    }
    return sum / audioLevelBufferSize;
}

quint8 icomUdpHandler::findMax(quint8 *data)
{
    unsigned int max=0;
    for(int p=0; p < audioLevelBufferSize; p++)
    {
       if(data[p] > max)
           max = data[p];
    }
    return max;
}

void icomUdpHandler::dataReceived()
{
    while (udp->hasPendingDatagrams()) {
        lastReceived = QTime::currentTime();
        QNetworkDatagram datagram = udp->receiveDatagram();
        QByteArray r = datagram.data();

        switch (r.length())
        {
            case (CONTROL_SIZE): // control packet
            {
                control_packet_t in = (control_packet_t)r.constData();
                if (in->type == 0x04) {
                    // If timer is active, stop it as they are obviously there!
                    qInfo(logUdp()) << this->metaObject()->className() << ": Received I am here from: " <<datagram.senderAddress().toString();

                    if (areYouThereTimer->isActive()) {
                        // send ping packets every second
                        areYouThereTimer->stop();
                        pingTimer->start(PING_PERIOD);
                        idleTimer->start(IDLE_PERIOD);
                    }
                }
                // This is "I am ready" in response to "Are you ready" so send login.
                else if (in->type == 0x06)
                {
                    qInfo(logUdp()) << this->metaObject()->className() << ": Received I am ready";
                    sendLogin(); // send login packet
                }
                break;
            }
            case (PING_SIZE): // ping packet
            {
                ping_packet_t in = (ping_packet_t)r.constData();
                if (in->type == 0x07 && in->reply == 0x01 && streamOpened)
                {
                    // This is a response to our ping request so measure latency
                    status.networkLatency += lastPingSentTime.msecsTo(QDateTime::currentDateTime());
                    status.networkLatency /= 2;
                    status.packetsSent = packetsSent;
                    status.packetsLost = packetsLost;
                    if (audio != Q_NULLPTR) {
                        status.packetsSent = status.packetsSent + audio->packetsSent;
                        status.packetsLost = status.packetsLost + audio->packetsLost;
                    }
                    if (civ != Q_NULLPTR) {
                        status.packetsSent = status.packetsSent + civ->packetsSent;
                        status.packetsLost = status.packetsLost + civ->packetsLost;
                    }

                    QString tempLatency;
                    if (status.rxCurrentLatency <= qint32(status.rxLatency) && !status.rxUnderrun && !status.rxOverrun)
                    {
                        tempLatency = QString("%1 ms").arg(status.rxCurrentLatency,3);
                    }
                    else if (status.rxUnderrun){
                        tempLatency = QString("<span style = \"color:red\">%1 ms</span>").arg(status.rxCurrentLatency,3);
                    }
                    else if (status.rxOverrun){
                        tempLatency = QString("<span style = \"color:orange\">%1 ms</span>").arg(status.rxCurrentLatency,3);
                    } else
                    {
                        tempLatency = QString("<span style = \"color:green\">%1 ms</span>").arg(status.rxCurrentLatency,3);
                    }
                    QString txString="";
                    if (txSetup.codec == 0) {
                        txString = "(no tx)";
                    }
                    status.message = QString("<pre>%1 rx latency: %2 / rtt: %3 ms / loss: %4/%5</pre>").arg(txString).arg(tempLatency).arg(status.networkLatency, 3).arg(status.packetsLost, 3).arg(status.packetsSent, 3);
                    // We are only really interested in audio timeDifference.
                    status.timeDifference = audio->getTimeDifference();
                    emit haveNetworkStatus(status);
                }
                break;
            }
            case (TOKEN_SIZE): // Response to Token request
            {
                token_packet_t in = (token_packet_t)r.constData();
                if (in->requesttype == 0x05 && in->requestreply == 0x02 && in->type != 0x01)
                {
                    if (in->response == 0x0000)
                    {
                        qDebug(logUdp()) << this->metaObject()->className() << ": Token renewal successful";
                        tokenTimer->start(TOKEN_RENEWAL);
                        gotAuthOK = true;
                        if (!streamOpened)
                        {
                           sendRequestStream();
                        }

                    }
                    else if (in->response == 0xffffffff)
                    {
                        qWarning() << this->metaObject()->className() << ": Radio rejected token renewal, performing login";
                        remoteId = in->sentid;
                        tokRequest = in->tokrequest;
                        token = in->token;
                        streamOpened = false;
                        sendRequestStream();
                        // Got new token response
                        //sendToken(0x02); // Update it.
                    }
                    else
                    {
                        qWarning() << this->metaObject()->className() << ": Unknown response to token renewal? " << in->response;
                    }
                }
                break;
            }   
            case (STATUS_SIZE):  // Status packet
            {
                status_packet_t in = (status_packet_t)r.constData();
                if (in->type != 0x01) {
                    if (in->error == 0xffffffff && !streamOpened)
                    {
                        emit haveNetworkError(errorType(true, radioIP.toString(), "Connection failed\ntry rebooting the radio."));
                        qInfo(logUdp()) << this->metaObject()->className() << ": Connection failed, wait a few minutes or reboot the radio.";
                    }
                    else if (in->error == 0x00000000 && in->disc == 0x01)
                    {
                        emit haveNetworkError(errorType(radioIP.toString(), "Got radio disconnected."));
                        qInfo(logUdp()) << this->metaObject()->className() << ": Got radio disconnected.";
                        if (streamOpened) {
                            // Close stream connections but keep connection open to the radio.
                            if (audio != Q_NULLPTR) {
                                delete audio;
                                audio = Q_NULLPTR;
                            }

                            if (civ != Q_NULLPTR) {
                                delete civ;
                                civ = Q_NULLPTR;
                            }

                            streamOpened = false;
                        }
                    }
                    else {
                        civPort = qFromBigEndian(in->civport);
                        audioPort = qFromBigEndian(in->audioport);
                        if (!streamOpened) {

                            civ = new icomUdpCivData(localIP, radioIP, civPort, splitWf, civLocalPort);
                            QObject::connect(civ, SIGNAL(receive(QByteArray)), this, SLOT(receiveFromCivStream(QByteArray)));

                            // TX is not supported
                            if (txSampleRates < 2) {
                                txSetup.sampleRate=0;
                                txSetup.codec = 0;
                            }
                            // Say what the server offered: a server that offers no
                            // transmit audio keys the rig (TUNE works) with no
                            // modulation, and that used to leave nothing in the log.
                            if (txSampleRates < 2)
                                qWarning(logUdp()).noquote() << QString("LAN: \"%1\" offers no transmit audio (TX sample rates 0x%2) - "
                                                                        "the rig keys but carries no voice; on an RS-BA1 server check "
                                                                        "Remote Utility's TX audio device and the user's rights")
                                    .arg(devName).arg(txSampleRates, 4, 16, QChar('0'));
                            else
                                qInfo(logUdp()).noquote() << QString("LAN: \"%1\" audio - RX %2 Hz codec %3, TX %4 Hz codec %5 "
                                                                     "(offered RX 0x%6, TX 0x%7)")
                                    .arg(devName).arg(rxSetup.sampleRate).arg(rxSetup.codec)
                                    .arg(txSetup.sampleRate).arg(txSetup.codec)
                                    .arg(rxSampleRates, 4, 16, QChar('0')).arg(txSampleRates, 4, 16, QChar('0'));
                            streamOpened = true;
                        }
                        if (audio == Q_NULLPTR) {
                            audio = new icomUdpAudio(localIP, radioIP, audioPort, audioLocalPort, rxSetup, txSetup);

                            QObject::connect(audio, SIGNAL(haveAudioData(audioPacket)), this, SLOT(receiveAudioData(audioPacket)));
                            QObject::connect(this, SIGNAL(haveChangeLatency(quint16)), audio, SLOT(changeLatency(quint16)));
                            QObject::connect(this, SIGNAL(haveSetVolume(quint8)), audio, SLOT(setVolume(quint8)));
                            QObject::connect(audio, SIGNAL(haveRxLevels(quint16, quint16, quint16, quint16, bool, bool)), this, SLOT(getRxLevels(quint16, quint16, quint16, quint16, bool, bool)));
                            QObject::connect(audio, SIGNAL(haveTxLevels(quint16, quint16, quint16, quint16, bool, bool)), this, SLOT(getTxLevels(quint16, quint16, quint16, quint16, bool, bool)));

                        }

                        qInfo(logUdp()) << this->metaObject()->className() << "Got serial and audio request success, device name: " << devName;


                    }
                }
                break;
            }
            case(LOGIN_RESPONSE_SIZE): // Response to Login packet.
            {
                login_response_packet_t in = (login_response_packet_t)r.constData();
                if (in->type != 0x01) {

                    connectionType = in->connection;
                    qInfo(logUdp()) << "Got connection type:" << connectionType;
                    if (connectionType == "FTTH")
                    {
                        highBandwidthConnection = true;
                    }

                    if (connectionType != "WFVIEW") // NOT WFVIEW
                    {
                        if (rxSetup.codec >= 0x40 || txSetup.codec >= 0x40)
                        {
                            emit haveNetworkError(errorType(QString("UDP"), QString("Opus codec not supported, forcing LPCM16")));
                            if (rxSetup.codec >= 0x40)
                                rxSetup.codec = 0x04;
                            if (txSetup.codec >= 0x40)
                                txSetup.codec = 0x04;
                        }
                    }


                    if (in->error == 0xfeffffff)
                    {
                        emit haveNetworkError(errorType(true, radioIP.toString(), "Invalid Username/Password"));
                        qInfo(logUdp()) << this->metaObject()->className() << ": Invalid Username/Password";
                    }
                    else if (!isAuthenticated)
                    {

                        if (in->tokrequest == tokRequest)
                        {
                            status.message="Radio Login OK!";
                            qInfo(logUdp()) << this->metaObject()->className() << ": Received matching token response to our request";
                            token = in->token;
                            sendToken(0x02);
                            tokenTimer->start(TOKEN_RENEWAL); // Start token request timer
                            isAuthenticated = true;
                        }
                        else
                        {
                            qInfo(logUdp()) << this->metaObject()->className() << ": Token response did not match, sent:" << tokRequest << " got " << in->tokrequest;
                        }
                    }

                    qInfo(logUdp()) << this->metaObject()->className() << ": Detected connection speed " << in->connection;
                }
                break;
            }
            case (CONNINFO_SIZE):
            {
                // Once connected, the server will send a conninfo packet for each radio that is connected

                conninfo_packet_t in = (conninfo_packet_t)r.constData();
                QHostAddress ip = QHostAddress(qToBigEndian(in->ipaddress));

                qInfo(logUdp()) << "Got Connection status for:" << in->name << "Busy:" << in->busy << "Computer" << in->computer << "IP" << ip.toString();

                // First we need to find this radio in our capabilities packet, there aren't many so just step through
                int thisRadio = -1;
                for (quint8 f = 0; f < radios.size(); f++)
                {

                    if ((radios[f].commoncap == 0x8010 &&
                        radios[f].macaddress[0] == in->macaddress[0] &&
                        radios[f].macaddress[1] == in->macaddress[1] &&
                        radios[f].macaddress[2] == in->macaddress[2] &&
                        radios[f].macaddress[3] == in->macaddress[3] &&
                        radios[f].macaddress[4] == in->macaddress[4] &&
                        radios[f].macaddress[5] == in->macaddress[5]) ||
                        !memcmp(radios[f].guid,in->guid, GUIDLEN))
                    {

                        bool admin=false;
                        if (in->busy && in->computer[0] != '\x0')
                            admin = true;

                        qDebug(logUdp()) << "Is the user an admin? " << admin;
                        emit setRadioUsage(f, admin, in->busy, QString(in->computer), ip.toString());
                        thisRadio = f;
                        qDebug(logUdp()) << "Set radio usage num:" << f << in->name << "Busy:" << in->busy << "Computer" << in->computer << "IP" << ip.toString();
                    }
                }

                // A server with several radios (RS-BA1 with an IC-7300 and an
                // IC-9700): wfview asked in a window, which this headless
                // server does not have, so it waited for ever. The radio is
                // chosen here - by [LAN] Radio / --lan-radio, else the first.
                if (!streamOpened && !radioChosen && radios.size() > 1 && thisRadio >= 0
                    && thisRadio == pickRadio()) {
                    radioChosen = true;
                    if (in->busy && in->ipaddress != 0x00 && strcmp(in->computer, compName.toLocal8Bit())) {
                        status.message = devName + " in use by: " + in->computer + " (" + ip.toString() + ")";
                        qWarning(logUdp()).noquote() << QString("LAN: \"%1\" is in use by %2 (%3)")
                            .arg(QString(radios[thisRadio].name), QString(in->computer), ip.toString());
                        sendControl(false, 0x00, in->seq);
                    } else {
                        setCurrentRadio(quint8(thisRadio));
                    }
                }
                else if (!streamOpened && radios.size()==1) {

                    qDebug(logUdp()) << "Single radio available, can I connect to it?";

                    if (in->busy)
                    {
                        if (in->ipaddress != 0x00 && strcmp(in->computer, compName.toLocal8Bit()))
                        {
                            status.message = devName + " in use by: " + in->computer + " (" + ip.toString() + ")";
                            sendControl(false, 0x00, in->seq); // Respond with an idle
                        }
                        else {
                        }
                    }
                    else if (!in->busy)
                    {
                        qDebug(logUdp()) << "Attempting to connect to radio";
                        status.message = devName + " available";
                        
                        setCurrentRadio(0);
                    }
                }
                else if (streamOpened) 
                /* If another client connects/disconnects from the server, the server will emit
                   a CONNINFO packet, send our details to confirm we still want the stream */
                {
                    //qDebug(logUdp()) << "I am already connected????";
                    // Received while stream is open.
                    //sendRequestStream();
                }
                break;
            }
    
            default:
            {
                if ((r.length() - CAPABILITIES_SIZE) % RADIO_CAP_SIZE != 0)
                {
                    // Likely a retransmit request?
                    break;
                }


                capabilities_packet_t in = (capabilities_packet_t)r.constData();
                numRadios = in->numradios;

                for (int f = CAPABILITIES_SIZE; f < r.length(); f = f + RADIO_CAP_SIZE) {
                    radio_cap_packet rad;
                    const char* tmpRad = r.constData();
                    memcpy(&rad, tmpRad+f, RADIO_CAP_SIZE);
                    radios.append(rad);
                    qInfo(logUdp()) << this->metaObject()->className() << QString("Received radio capabilities, Name: %1, Audio: %2, CIV: %3, MAC: %4:%5:%6:%7:%8:%9 CAPF: %10")
                        .arg(rad.name).arg(rad.audio).arg((quint8)rad.civ, 2, 16, QChar('0'))
                        .arg(rad.macaddress[0], 2, 16, QChar('0')).arg(rad.macaddress[1], 2, 16, QChar('0'))
                        .arg(rad.macaddress[2], 2, 16, QChar('0')).arg(rad.macaddress[3], 2, 16, QChar('0'))
                        .arg(rad.macaddress[4], 2, 16, QChar('0')).arg(rad.macaddress[5], 2, 16, QChar('0')).arg(rad.capf, 4, 16, QChar('0'));
                }

                emit requestRadioSelection(radios);
                if (radios.size() > 1) {
                    QStringList names;
                    for (int i = 0; i < radios.size(); ++i)
                        names << QString("%1 \"%2\"").arg(i + 1).arg(QString(radios[i].name));
                    const int pick = pickRadio();
                    if (pick < 0)
                        qWarning(logUdp()).noquote() << QString("LAN: the server has %1 radios (%2) and none matches \"%3\" "
                                                                "- set [LAN] Radio / --lan-radio to part of a name or its number")
                            .arg(radios.size()).arg(names.join(", "), wantRadio);
                    else
                        qInfo(logUdp()).noquote() << QString("LAN: the server has %1 radios (%2) - using %3%4")
                            .arg(radios.size()).arg(names.join(", ")).arg(pick + 1)
                            .arg(wantRadio.isEmpty() ? QString(" (the first; choose with [LAN] Radio / --lan-radio)") : QString());
                }

                break;
            }
    
        }
        icomUdpBase::dataReceived(r); // Call parent function to process the rest.
        r.clear();
        datagram.clear();
    }
    return;
}


// The radio to use on a server with several: [LAN] Radio / --lan-radio as a
// number from 1 or part of a name (case-insensitive), else the first; -1 when
// a name was given and none matches (never a radio the operator did not mean).
int icomUdpHandler::pickRadio() const
{
    if (radios.isEmpty()) return -1;
    if (wantRadio.isEmpty()) return 0;
    bool isNum = false;
    const int n = wantRadio.toInt(&isNum);
    // A position (1, 2 ...) when it is one; otherwise digits are part of a
    // name ("7300" -> "IC-7300 Man Cave").
    if (isNum && n >= 1 && n <= radios.size()) return n - 1;
    for (int i = 0; i < radios.size(); ++i)
        if (QString(radios[i].name).contains(wantRadio, Qt::CaseInsensitive)) return i;
    return -1;
}

void icomUdpHandler::setCurrentRadio(quint8 radio) {

    // If we are currently connected to a different radio, disconnect first
    if (audio != Q_NULLPTR) {
        delete audio;
        audio = Q_NULLPTR;
    }

    if (civ != Q_NULLPTR) {
        delete civ;
        civ = Q_NULLPTR;
    }

    streamOpened = false;

    qInfo(logUdp()) << "Got Radio" << radio;
    qInfo(logUdp()) << "Find available local ports";

    /* This seems to be the only way to find some available local ports.
        The problem is we need to know the local AND remote ports but send the 
        local port to the server first and it replies with the remote ports! */
    if (civLocalPort == 0 || audioLocalPort == 0) {
        QUdpSocket* tempudp = new QUdpSocket();
        tempudp->bind(); // Bind to random port.
        civLocalPort = tempudp->localPort();
        tempudp->close();
        tempudp->bind();
        audioLocalPort = tempudp->localPort();
        tempudp->close();
        delete tempudp;
    }
    int baudrate = qFromBigEndian(radios[radio].baudrate);
    emit haveBaudRate(baudrate);

    if (radios[radio].commoncap == 0x8010) {
        memcpy(&macaddress, radios[radio].macaddress, sizeof(macaddress));
        useGuid = false;
    }
    else {
        useGuid = true;
        memcpy(&guid, radios[radio].guid, GUIDLEN);
    }

    devName =radios[radio].name;
    audioType = radios[radio].audio;
    civId = radios[radio].civ;
    rxSampleRates = radios[radio].rxsample;
    txSampleRates = radios[radio].txsample;

    sendRequestStream();
}


void icomUdpHandler::sendRequestStream()
{

    QByteArray usernameEncoded;
    passcode(username, usernameEncoded);

    conninfo_packet p;
    memset(p.packet, 0x0, sizeof(p)); // We can't be sure it is initialized with 0x00!
    p.len = sizeof(p);
    p.sentid = myId;
    p.rcvdid = remoteId;
    p.payloadsize = qToBigEndian((quint32)(sizeof(p) - 0x10));
    p.requesttype = 0x03;
    p.requestreply = 0x01;

    if (!useGuid) {
        p.commoncap = 0x8010;
        memcpy(&p.macaddress, macaddress, 6);
    }
    else {
        memcpy(&p.guid, guid, GUIDLEN);
    }
    p.innerseq = qToBigEndian(authSeq++);
    p.tokrequest = tokRequest;
    p.token = token;
    memcpy(&p.name, devName.toLocal8Bit().constData(), devName.length());
    p.rxenable = 1;
    if (this->txSampleRates > 1) {
        p.txenable = 1;
        p.txcodec = txSetup.codec;
    }
    p.rxcodec = rxSetup.codec;
    memcpy(&p.username, usernameEncoded.constData(), usernameEncoded.length());
    p.rxsample = qToBigEndian((quint32)rxSetup.sampleRate);
    p.txsample = qToBigEndian((quint32)txSetup.sampleRate);
    p.civport = qToBigEndian((quint32)civLocalPort);
    p.audioport = qToBigEndian((quint32)audioLocalPort);
    p.txbuffer = qToBigEndian((quint32)txSetup.latency);
    p.convert = 1;
    sendTrackedPacket(QByteArray::fromRawData((const char*)p.packet, sizeof(p)));
    return;
}

void icomUdpHandler::sendAreYouThere()
{
    if (areYouThereCounter == 20)
    {
        qInfo(logUdp()) << this->metaObject()->className() << ": Radio not responding.";
        status.message = "Radio not responding!";
    }
    qInfo(logUdp()) << this->metaObject()->className() << ": Sending Are You There...";

    areYouThereCounter++;
    icomUdpBase::sendControl(false,0x03,0x00);
}

void icomUdpHandler::sendLogin() // Only used on control stream.
{

    qInfo(logUdp()) << this->metaObject()->className() << ": Sending login packet";

    tokRequest = static_cast<quint16>(rand() | rand() << 8); // Generate random token request.

    QByteArray usernameEncoded;
    QByteArray passwordEncoded;
    passcode(username, usernameEncoded);
    passcode(password, passwordEncoded);

    login_packet p;
    memset(p.packet, 0x0, sizeof(p)); // We can't be sure it is initialized with 0x00!
    p.len = sizeof(p);
    p.sentid = myId;
    p.rcvdid = remoteId;
    p.payloadsize = qToBigEndian((quint32)(sizeof(p) - 0x10));
    p.requesttype = 0x00;
    p.requestreply = 0x01;

    p.innerseq = qToBigEndian(authSeq++);
    p.tokrequest = tokRequest;
    memcpy(p.username, usernameEncoded.constData(), usernameEncoded.length());
    memcpy(p.password, passwordEncoded.constData(), passwordEncoded.length());
    memcpy(p.name, compName.toLocal8Bit().constData(), compName.length());

    sendTrackedPacket(QByteArray::fromRawData((const char*)p.packet, sizeof(p)));
    return;
}

void icomUdpHandler::sendToken(uint8_t magic)
{
    qDebug(logUdp()) << this->metaObject()->className() << "Sending Token request: " << magic;

    token_packet p;
    memset(p.packet, 0x0, sizeof(p)); // We can't be sure it is initialized with 0x00!
    p.len = sizeof(p);
    p.sentid = myId;
    p.rcvdid = remoteId;
    p.payloadsize = qToBigEndian((quint32)(sizeof(p) - 0x10));
    p.requesttype = magic;
    p.requestreply = 0x01;
    p.innerseq = qToBigEndian(authSeq++);
    p.tokrequest = tokRequest;
    p.resetcap = qToBigEndian((quint16)0x0798);
    p.token = token;

    sendTrackedPacket(QByteArray::fromRawData((const char *)p.packet, sizeof(p)));
    // The radio should request a repeat of the token renewal packet via retransmission!
    //tokenTimer->start(100); // Set 100ms timer for retry (this will be cancelled if a response is received)
    return;
}

