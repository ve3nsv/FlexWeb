#ifndef FLEXCOMMANDER_H
#define FLEXCOMMANDER_H

// rigCommander for FlexRadio Systems SDRs (FLEX-6000 / FLEX-8000 / Aurora).
//
// Unlike the CI-V / CAT rigs, a Flex is a network server with no front-panel
// VFO for wfweb to poll: the client owns a *slice* (receiver) on a
// *panadapter*, subscribes to status, and gets everything pushed to it.
//
//   TCP 4992  SmartSDR API: commands out, responses + status in
//   UDP       VITA-49 from the radio to our port: panadapter FFT, meters,
//             RX audio (remote_audio_rx, float32 24 kHz)
//   UDP 4991  VITA-49 to the radio: DAX TX audio (float32 24 kHz)
//
// flexCommander maps wfweb's funcs onto that model: VFO A = our slice, the
// scope = our panadapter's FFT, browser audio <-> remote_audio_rx / dax_tx.
// GET requests from the queue are answered from the pushed state.
//
// Protocol details come from AetherSDR (https://github.com/aethersdr/AetherSDR,
// GPLv3) via flexprotocol.h.

#include "rigcommander.h"
#include "flexprotocol.h"

#include <QTcpSocket>
#include <QSslSocket>
#include "smartlinkclient.h"
#include <QUdpSocket>
#include <QHostAddress>
#include <QTimer>
#include <QSet>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QJsonArray>
#include <functional>

struct SpeexResamplerState_;

class flexCommander : public rigCommander
{
    Q_OBJECT

public:
    explicit flexCommander(rigCommander* parent = nullptr);
    explicit flexCommander(quint8 guid[GUIDLEN], rigCommander* parent = nullptr);
    ~flexCommander();

public slots:
    void process() override;
    void commSetup(rigTypedef rigList, quint16 rigCivAddr, QString rigSerialPort, quint32 rigBaudRate, QString vsp, quint16 tcp, quint8 wf) override;
    void commSetup(rigTypedef rigList, quint16 rigCivAddr, udpPreferences prefs, audioSetup rxSetup, audioSetup txSetup, QString vsp, quint16 tcp) override;
    void closeComm() override;
    void setPTTType(pttType_t ptt) override;

    void powerOn() override;
    void powerOff() override;

    void setRigID(quint16 rigID) override;
    void setCIVAddr(quint16 civAddr) override;

    void handleNewData(const QByteArray& data) override;
    void receiveBaudRate(quint32 baudrate) override;

    void receiveCommand(funcs func, QVariant value, uchar receiver) override;

    // FUNC > EQ and FUNC > PROC on the page (webServer::flexDspSet). Keys:
    // rxEqOn / txEqOn (value 0/1), rxEqBand / txEqBand (index 0-7, value
    // -10..10 dB), rxEqFlat / txEqFlat, proc (0/1), procLevel (0 NOR, 1 DX,
    // 2 DX+), dexp (0/1), dexpLevel (0-100), micLevel (0-100), txLo / txHi
    // (TX filter edges, Hz).
    void setFlexDsp(QString key, int index, int value);
    // The radio's memories (MEMORIES applet): action "apply" / "create" /
    // "set" / "remove"; fields name, group for create and set.
    void flexMemoryCommand(QString action, int id, QJsonObject fields);

signals:
    // The radio's EQ and TX audio processing, as the page shows it.
    void flexDspState(QJsonObject state);
    // The radio's memory list, every time it changes (coalesced).
    void flexMemories(QJsonArray list);
    // Another client (SmartSDR, Maestro...) disconnected us, or the radio
    // dropped us for a duplicate client id. servermain closes the LAN session
    // so wfweb stays off the radio until a browser asks to connect again.
    void evicted(QString reason);
    // The SmartSDR session is up (true) or gone (false): the terminal status
    // page shows the radio as connected only while it is up.
    void linkState(bool up);

private slots:
    void tcpConnected();
    void tcpDisconnected();
    void tcpError(QAbstractSocket::SocketError err);
    void tcpReadyRead();
    void udpReadyRead();
    void discoveryReadyRead();
    void pingTick();
    void reconnectTick();
    void adoptOrCreateSlice();
    void webTxAudio(const audioPacket& data);

private:
    using responseFn = std::function<void(quint32 code, const QString& body)>;

    // Connection
    void startConnect();
    void resetSession();
    quint32 sendCmd(const QString& cmd, responseFn cb = nullptr);
    void processLine(const QString& line);
    void handleStatus(const flexProtocol::message& msg);
    void handleSliceStatus(int id, const QMap<QString, QString>& kvs);
    void handlePanStatus(const QString& panId, const QMap<QString, QString>& kvs);
    void handleTransmitStatus(const QMap<QString, QString>& kvs);
    void handleMeterStatus(const QString& body);
    void adoptPan(const QString& id);
    void configurePan();
    void recenterPan();
    void createAudioStreams();
    void setStatus(const QString& message, bool connected);

    // VITA-49
    void handleFft(const QByteArray& d, const flexProtocol::vitaPacket& p);
    void handleMeters(const QByteArray& d, const flexProtocol::vitaPacket& p);
    void handleRxAudio(const QByteArray& d, const flexProtocol::vitaPacket& p);
    void logUdpStats();

    // wfweb glue
    void determineRigCaps();
    void identify();
    void report(funcs func, const QVariant& value);
    bool reportCurrent(funcs func);
    modeInfo modeFor(const QString& flexMode, int filter) const;
    QString flexModeForReg(quint8 reg) const;
    int filterNumForWidth(const QString& mode, int width) const;
    void applyFilterWidth(int widthHz);
    void sliceSet(const QString& kv);
    void setTransmit(bool on);

    // Transport
    // A QSslSocket: plain TCP on the LAN, TLS to the radio over SmartLink.
    QSslSocket* tcp = nullptr;
    QUdpSocket* udp = nullptr;
    QUdpSocket* discovery = nullptr;
    QTimer* pingTimer = nullptr;
    QTimer* reconnectTimer = nullptr;
    QByteArray lineBuffer;
    QHostAddress radioAddress;
    quint16 radioPort = 4992;
    quint16 radioUdpPort = 4991;
    bool useDiscovery = false;
    bool closing = false;

    // SmartLink (--smartlink <radio>, prefs.ipAddress "smartlink:<radio>"):
    // the SmartLink server brokers the session, the radio is reached on its
    // public address over TLS, and UDP is registered from our side
    // ("client udp_register") instead of "client udpport".
    bool wan = false;
    QString wanRadioName;
    smartLinkClient* smartLink = nullptr;
    smartLinkRadio wanRadio;
    QString wanHandle;
    bool wanConnectRequested = false;
    bool wanUdpConfirmed = false;
    QTimer* wanRegisterTimer = nullptr;
    QTimer* wanWatchdog = nullptr;
    void startWan();
    void wanRadioList(const QList<smartLinkRadio>& radios);
    void wanConnectReady(const QString& handle, const QString& serial);
    void wanEncrypted();
    void wanRetry(const QString& why, int ms);
    void sendUdpRegister();

    quint32 nextSeq = 1;
    QHash<quint32, responseFn> pending;
    quint32 clientHandle = 0;
    bool handshakeDone = false;     // "client gui" accepted
    bool rigIdentified = false;     // determineRigCaps() ran this session
    bool infoDone = false;          // the radio answered "info" (model name)
    bool capsLoaded = false;
    QString firmware;
    QString radioModel;
    QString radioNickname;

    // Our receiver
    int sliceId = -1;
    bool sliceCreatePending = false;
    QString panId;                  // "0x40000000"
    quint32 panStreamId = 0;
    quint32 rxStreamId = 0;
    quint32 txStreamId = 0;
    int txPacketCount = 0;

    // Slice state (as last reported by the radio)
    quint64 freqHz = 0;
    QString mode;
    int filterLo = 0;
    int filterHi = 0;
    int filterNum = 1;
    int audioLevel = 50;            // 0..100
    int agcThreshold = 65;          // 0..100, used as "RF gain"
    QString agcMode;
    bool nb = false;
    bool nr = false;
    bool anf = false;
    bool squelchOn = false;
    int squelchLevel = 20;          // 0..100
    QString rxAnt;
    QString txAnt;
    // The slice's own antenna lists (ant_list / tx_ant_list in its status):
    // everything this radio can receive or transmit on, RX_A / XVTA included.
    QStringList rxAntList, txAntList;
    // 2 m through a transverter: tuning into 144-148 MHz moves RX and TX to
    // the XVTR port, leaving the band puts back the HF antennas it had.
    bool on2m = false;
    QString hfRxAnt, hfTxAnt;
    // Radio memories by index, as the radio reports them ("sub memories all").
    QMap<int, QMap<QString, QString>> memories;
    bool memPublishPending = false;
    void handleMemoryStatus(const QString& object, const QMap<QString, QString>& kvs);
    void publishMemories();
    // After leaving 2 m: the antennas wfweb put back, and until when it
    // defends them against the radio's own per-band antenna recall, which
    // can land after our command.
    QString hfRestoreRx, hfRestoreTx;
    qint64 hfRestoreUntilMs = 0;
    static bool isTwoMetres(quint64 hz) { return hz >= 144000000ULL && hz <= 148000000ULL; }
    void rememberHfAntennas();
    void applyBandAntennas();
    void setSliceAntennas(const QString& rx, const QString& tx);
    bool ritOn = false;
    int ritFreq = 0;
    QStringList antList;

    // Transmit state
    int rfPower = 100;              // 0..100
    int micLevel = 50;              // 0..100
    bool daxTx = false;
    bool monitor = false;
    int monitorGain = 50;
    bool processor = false;
    // EQ and processor state for flexDspState. The radio does not echo a
    // client's own changes here either, so every set updates these at once.
    struct eqState { bool on = false; int bands[8] = {0, 0, 0, 0, 0, 0, 0, 0}; };
    eqState eqRx, eqTx;
    int procLevel = 0;              // speech_processor_level: 0 NOR, 1 DX, 2 DX+
    bool dexpOn = false;            // compander (downward expander)
    int dexpLevel = 0;
    int txFilterLo = 100, txFilterHi = 2900;
    bool dspStateDirty = false;
    QTimer* dspPublishTimer = nullptr;
    void handleEqStatus(bool tx, const QMap<QString, QString>& kvs);
    void publishDsp();
    int cwSpeed = 20;
    int cwPitch = 600;
    bool transmitting = false;
    bool tuning = false;
    QString atuStatus;
    bool atuPresent = false;
    int modLevel = 191;             // wfweb 0..255 "USB mod level", 75% = unity gain

    // Panadapter
    double panCenterMHz = 0.0;
    double panBandwidthMHz = 0.0;
    int panXPixels = 0;
    int panYPixels = 0;
    int panRfGain = 0;
    int scopeFps = 20;
    bool panConfigured = false;
    bool recenterPending = false;
    QHash<QString, QMap<QString, QString>> panStates;   // every pan's last status, by id
    flexProtocol::fftAssembler fft;
    static constexpr int kPanYPixels = 1201;      // 0.1 dB per pixel over the range below
    static constexpr int kPanMinDbm = -150;
    static constexpr int kPanMaxDbm = -30;
    double panMinDbm = kPanMinDbm;
    double panMaxDbm = kPanMaxDbm;
    float noiseFloorDbm = -999.0f;

    // Diagnostics (logged every 10 s)
    QSet<quint64> udpSeen;
    QMap<QString, int> udpStats;
    QElapsedTimer udpStatsTimer;
    int scopeFramesSent = 0;
    int scopeFramesDropped = 0;

    // Meters
    struct meterDef { QString src; int num = -1; QString name; QString unit; };
    QHash<quint16, meterDef> meters;
    // Meter readings reach wfweb at most every 100 ms, always the latest:
    // a reading that arrives inside the window is held (meterSDirty /
    // meterTxDirty) and sent when meterFlushTimer ends it, so a packet of
    // other meters can never swallow an S-meter update.
    QTimer* meterFlushTimer = nullptr;
    bool meterSDirty = false;
    bool meterTxDirty = false;
    void flushMeters();
    double lastSMeter = -54.0;
    double lastPower = 0.0;
    double lastSwr = 1.0;
    double lastAlc = 0.0;
    // A Flex has no ALC reading for audio drive (its HWALC meter is the
    // external ALC jack, 0 dBFS with nothing plugged in), so wfweb's ALC is
    // the peak of the DAX TX audio we send, over ~100 ms: 0 up to -2 dBFS,
    // 1.0 at full scale. The FT8/JS8 drive loop then holds the DAX level
    // just under clipping, which is where the radio makes its set power.
    float txPeakWindow = 0.0f;
    QElapsedTimer txPeakTimer;
    double lastComp = 0.0;
    double lastVolts = 0.0;
    double lastAmps = 0.0;

    // Audio
    SpeexResamplerState_* rxResampler = nullptr;
    // remote_audio_rx compression: "opus" over SmartLink by default (about
    // 30x less than raw float), "none" on the LAN; WFWEB_FLEX_AUDIO /
    // --flex-audio opus|none|auto overrides.
    QString rxCompression;
    struct OpusDecoder* opusDec = nullptr;
    QVector<float> decodeOpus(const QByteArray& d, const flexProtocol::vitaPacket& p);
    SpeexResamplerState_* txResampler = nullptr;
    quint32 rxSeq = 0;
    QVector<float> txAccumulator;   // mono 24 kHz, waiting for a full packet
    // DAX TX audio between "xmit 1" and the radio actually transmitting
    // (interlock TRANSMITTING) is held, not sent: the radio drops DAX audio
    // until it is on the air, which cut the start of every FT8/JS8 frame
    // (and a voice over's first syllable). Sent as soon as it is on the air.
    bool txRequested = false;       // we sent xmit 1, no xmit 0 since
    bool onAir = false;             // interlock state TRANSMITTING
    QVector<QByteArray> txPending;
    QElapsedTimer txKeyTimer;       // xmit 1 -> on the air, for the log
    static constexpr int kMaxPendingPackets = 24000 / 128;   // 1 s of 128-frame DAX packets

    QHash<quint16, rigInfo> rigList;
};

#endif // FLEXCOMMANDER_H
