#ifndef FREEDVREPORTER_H
#define FREEDVREPORTER_H

#include "spotreporter.h"
#include <QWebSocket>
#include <QTimer>
#include <QJsonObject>
#include <QHash>

class FreeDVReporter : public SpotReporter
{
    Q_OBJECT
public:
    explicit FreeDVReporter(QObject *parent = nullptr);
    ~FreeDVReporter() override;

    void setStation(const QString &callsign, const QString &grid) override;
    void connectToService() override;
    void disconnectFromService() override;
    void updateFrequency(quint64 hz) override;
    void updateTx(const QString &mode, bool transmitting) override;
    void updateRxSpot(const QString &callsign, const QString &mode, int snr) override;
    void setMessage(const QString &message);

    State state() const override { return state_; }

    // Punch list 39: the station that FreeDV Reporter says is transmitting
    // within tolHz of our own reported frequency (not us), the most recent
    // to key up if several; empty when none. From the events the server
    // pushes to every client (bulk_update, new_connection, freq_change,
    // tx_report, remove_connection).
    QString transmittingNear(int tolHz = 1000) const;

private slots:
    void onWsConnected();
    void onWsDisconnected();
    void onWsTextMessage(const QString &message);
    void onWsError(QAbstractSocket::SocketError error);
    void onPingTimeout();
    void onReconnectTimer();
    void onFreqDebounce();

private:
    void setState(State s);
    void sioEmit(const QString &event, const QJsonObject &data);
    void sioEmitNoPayload(const QString &event);
    void resendState();
    void scheduleReconnect();
    static QString reporterModeName(const QString &mode);
    void onReporterEvent(const QString &event, const QJsonObject &data);

    struct Station { QString callsign; quint64 freq = 0; bool tx = false; qint64 lastTxMs = 0; };
    QHash<QString, Station> stations_;   // by the server's sid

    static constexpr int PROTOCOL_VERSION = 2;
    static constexpr const char *DEFAULT_HOST = "qso.freedv.org";
    static constexpr int DEFAULT_PORT = 80;

    QWebSocket *socket_ = nullptr;
    QTimer *pingTimer_ = nullptr;
    QTimer *reconnectTimer_ = nullptr;
    QTimer *freqDebounceTimer_ = nullptr;

    State state_ = Disconnected;
    bool intentionalDisconnect_ = false;
    int reconnectDelay_ = 2000;
    int pingTimeout_ = 20000;

    // Station config
    QString callsign_;
    QString grid_;

    // Cached state for resend after reconnect
    quint64 lastFreq_ = 0;
    quint64 pendingFreq_ = 0;
    QString lastMode_;
    bool lastTx_ = false;
    QString lastMessage_;
    bool fullyConnected_ = false;
};

#endif // FREEDVREPORTER_H
