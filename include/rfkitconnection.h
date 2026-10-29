#pragma once

// Ported from VE3NSV's RF-KIT support for AetherSDR (github.com/ve3nsv/
// AetherSDR, branch rfkit-tune-pr, 934dcd6), GPLv3. "Constitution principle"
// references are AetherSDR's project rules. Changed in FlexWeb (punch list 42,
// 43; not yet in rfkit-tune-pr): one request at a time on a receive / transmit
// schedule instead of three every 250 ms, and the tune-API probe is no longer
// sent on connect - it wakes the amplifier, so it is sent only as wake().


#include <QHash>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QUrl>

#include "rfkitprotocol.h"

namespace AetherSDR {

// Peripheral transport for an RF-KIT amplifier.
//
// A standalone Ethernet device with no FlexRadio awareness at all, so this is
// a peripheral(rfkit) accessory alongside AcomConnection/SpeConnection/
// VkampConnection/Kpa1500Connection, not an IRadioBackend implementor. See
// docs/architecture/rfkit-amplifier-design.md.
//
// The device speaks a REST API over HTTP/1.1 (RfkitProtocol.h), not a
// streaming socket protocol, and it never pushes: every reading below arrives
// because this class asked for it. So the shape differs from the socket-based
// peripherals in two ways:
//
//  * "Connected" is not a TCP state. connectNetwork() issues GET /info, and
//    the link counts as up only when that returns an RF-KIT-shaped document.
//    It counts as down again after kMaxConsecutiveFailures transport failures
//    in a row, so one dropped request does not flicker the applet.
//  * Requests are strictly serialized, one in flight at a time. The device is
//    a small embedded controller, and a command that has to queue behind a
//    full poll round on a congested link is better than a burst that makes it
//    drop requests. Commands jump the queue; polls are only enqueued when the
//    queue is empty, so a slow device lowers the poll rate instead of
//    building a backlog.
//
// Like every peripheral here, control writes never latch an optimistic value:
// the device is the authority on its own state, and the applet repaints from
// the reply (constitution Principle II).
class RfkitConnection : public QObject {
    Q_OBJECT

public:
    explicit RfkitConnection(QObject* parent = nullptr);
    ~RfkitConnection() override;

    bool isConnected() const { return m_connected; }
    QString description() const;  // "192.168.1.60:8080", for status display

    void connectNetwork(const QString& host, quint16 port = Rfkit::kDefaultPort);
    void disconnect();
    void setAutoReconnect(bool on) { m_autoReconnect = on; }

    // Control (host -> amp). Each is a no-op, logged, when not connected.
    void setOperate(bool operate);
    // An antenna outside the API document's range for its type is refused,
    // logged, and never sent.
    void selectAntenna(const Rfkit::Antenna& antenna);
    // POST /error/reset. Clears the amplifier's error state; the device answers
    // 409 while the function is disabled, which is logged.
    void resetError();
    // PUT /tuner {"tuner_mode":"START"}: asks the amplifier to run its automatic
    // tune. Ignored while a previous start is still outstanding. Sent only when the amplifier's server was found to accept it
    // (status().tuneApi) and its tuner is in automatic or manual mode; refused,
    // logged, and never sent otherwise. The amplifier tunes only while a tune
    // carrier is already being transmitted. Nothing here keys the radio.
    void startTune();

    // Punch list 42: the empty PUT /tuner {} wakes a sleeping amplifier
    // (a modified restServer.py wakes it before checking the body; it answers
    // 400 and tunes nothing) and resets its sleep timer. The reply also tells
    // whether the amplifier has the tune command, so it replaces the probe
    // that used to go out on every connect. Skipped while one is outstanding.
    void wake();
    // Punch list 43: whether the radio is transmitting (its own TX state, not
    // the amp's readings - the API has no keyed flag). While it is, and for
    // kTxHoldMs after, /power is read as fast as the amplifier answers.
    void setTransmitting(bool on);
    // A key-up is on its way: poll fast for kTxHoldMs even before the radio
    // reports it (TX requested by this server).
    void kickFast();
    // Punch list 45: [Amplifier] RxPollMs (100-5000) / TxPollMs (10-1000).
    void setPollIntervals(int rxMs, int txMs);

signals:
    void connected();
    void disconnected();
    void connectionFailed(const QString& errorString);
    // Full current snapshot, emitted when a reply actually changed something.
    void statusUpdated(const AetherSDR::Rfkit::Status& status);
    // A wake() was answered (any HTTP status) or failed in transit.
    void wakeAnswered(bool ok);

private:
    struct Request {
        enum class Method { Get, Put, Post };
        Method method{Method::Get};
        QString path;
        QByteArray body;
        bool probe{false};  // the connect handshake (GET /info)
        bool command{false};  // operator-issued; kept in issue order ahead of polls
        bool tuneProbe{false};  // PUT /tuner {} : does this server have the route?
    };

    static bool isTuneStart(const Request& request);
    void enqueue(const Request& request, bool front = false);
    void pump();
    void onReplyFinished(QNetworkReply* reply, const Request& request, int epoch);
    // Returns true when the reply changed the snapshot.
    bool applyReply(const Request& request, const QByteArray& body);
    void onProbeSucceeded();
    void onTransportFailure(const QString& errorString, const Request& request);
    void onTransportDown(const QString& errorString);
    void poll();
    bool fastMode() const;
    void updatePollRate();
    void enqueueFullRefresh();
    void armReconnect();
    void resetSession();

    QNetworkAccessManager m_nam;
    QUrl m_baseUrl;
    QPointer<QNetworkReply> m_reply;
    QQueue<Request> m_queue;
    bool m_inFlight{false};

    Rfkit::Status m_status;
    // The last body seen per path, so an unchanged slow reply does not
    // re-emit statusUpdated().
    QHash<QString, QByteArray> m_lastBody;
    // Paths already warned about for an unparseable 2xx body, so a firmware
    // that answers with an unexpected shape logs once per connection rather
    // than four times a second.
    QSet<QString> m_warnedPaths;

    QString m_lastHost;
    quint16 m_lastPort{Rfkit::kDefaultPort};

    bool m_connected{false};
    bool m_autoReconnect{false};
    bool m_deliberateDisconnect{false};

    // Bumped on every connect/disconnect. A reply carries the epoch it was
    // issued under, and one from a previous session is discarded unread: a
    // late answer from attempt N must never touch attempt N+1 (#2380).
    int m_epoch{0};
    int m_consecutiveFailures{0};
    static constexpr int kMaxConsecutiveFailures = 3;

    // Per-request transfer timeout. Also bounds the connect handshake, so an
    // unreachable address reports failure here rather than after the OS TCP
    // connect timeout (~21s on Windows).
    // 15 s (punch list 46, was 4 s; PgTG's plugin uses 15 s): a request is
    // never abandoned while the amp is merely slow - a sleeping RF2K-S takes
    // ~5 s to answer the wake, and its one-at-a-time server (Python wsgiref)
    // trips on every connection closed before its answer (BrokenPipeError)
    // and, flooded with them, stopped answering for minutes.
    static constexpr int kRequestTimeoutMs = 15000;
    // The transfer timeout above only fires when no bytes move; this bounds the
    // whole request, so a peer that trickles bytes cannot hold the one serialized
    // slot (and every command queued behind it) indefinitely. Principle VII.
    static constexpr int kRequestDeadlineMs = 20000;

    // Reconnect backoff (punch list 46): 5, 15, 30, then every 60 s while the
    // amp keeps failing; back to 5 s once it answers.
    QTimer m_reconnectTimer;
    int m_reconnectStep{0};
    // A slow or failed reply pauses polling (never a backlog of requests the
    // amp would answer into closed connections).
    qint64 m_pollPausedUntilMs{0};
    qint64 m_sentAtMs{0};

    // One GET per tick, only when nothing is queued or in flight, from a
    // receive or a transmit schedule (see poll()). A slow amplifier lowers the
    // rate instead of building a backlog.
    QTimer m_pollTimer;
    QTimer m_deadlineTimer;
    int m_rxPollMs{250};  // ~3.7 GETs/s (was 12)
    int m_txPollMs{25};   // up to ~32 GETs/s, /power 4 of 5 (punch list 45; was 50)
    static constexpr int kTxHoldMs = 3000;  // fast after unkey: feeds the 3 s peak hold
    static constexpr qint64 kSlowPathMs = 10000;  // /antennas, /operational-interface
    int m_slowPollIndex{0};
    bool m_transmitting{false};
    // Per over: /power readings while keyed, logged at unkey with the rate
    // the amplifier actually sustained (punch list 45).
    qint64 m_overStartMs{0};
    int m_overPowerReads{0};
    qint64 m_fastUntilMs{0};
    qint64 m_lastAntennasMs{0};
    qint64 m_lastInterfaceMs{0};

    bool m_tuneProbePending{false};  // a wake() is outstanding
    // A START is outstanding (queued or in flight). The tuner readback that
    // would disable the button can be a second old, so this keeps a double
    // click from sending START twice. It is a send guard, not displayed state.
    bool m_tuneStartPending{false};
};

}  // namespace AetherSDR
