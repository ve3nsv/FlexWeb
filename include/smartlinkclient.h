#pragma once

// SmartLink: FlexRadio's internet relay. Signs in to the user's FlexRadio
// account (Auth0), keeps the TLS session to smartlink.flexradio.com that
// lists the account's radios and asks a radio to accept a remote client.
//
// Ported from AetherSDR's SmartLinkClient / WanConnection
// (https://github.com/aethersdr/AetherSDR), Copyright the AetherSDR
// contributors, GPLv3 like wfweb. Differences: no keychain — the refresh
// token (never the password) is kept in QSettings("wfweb", "smartlink"),
// and the radio's certificate pin (trust on first use) lives there too.

#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSslSocket>
#include <QString>
#include <QTimer>

struct smartLinkRadio {
    QString serial;
    QString nickname;
    QString callsign;
    QString model;
    QString status;          // "Available", "In_Use", ...
    QString publicIp;
    int publicTlsPort = -1;  // forwarded port, else UPnP port, else -1
    int publicUdpPort = -1;
    bool upnp = false;
    bool needsHolePunch = false;
    QString guiClients;      // programs already on the radio, comma-separated
};

class smartLinkClient : public QObject
{
    Q_OBJECT
public:
    explicit smartLinkClient(QObject* parent = nullptr);
    ~smartLinkClient() override;

    // Sign in with the account's e-mail and password. The password is sent
    // to Auth0 and dropped; only the refresh token it returns is stored.
    void login(const QString& email, const QString& password);
    // Sign in again with the stored refresh token.
    bool loginWithStoredToken();
    void requestConnect(const QString& serial, quint16 holePunchPort);
    void close();

    static bool haveStoredToken();
    static QString storedEmail();
    static void forgetCredentials();

    // Radio certificate pins, trust on first use. Returns "" when none.
    static QString certPin(const QString& host);
    static void setCertPin(const QString& host, const QString& sha256Hex);
    static void forgetCertPins();

    // Pick a radio by serial or nickname (case-insensitive). An empty or
    // "auto" name picks the only radio on the account.
    static int findRadio(const QList<smartLinkRadio>& radios, const QString& name);
    static QList<smartLinkRadio> parseRadioList(const QString& line);

signals:
    void authenticated();
    void authFailed(const QString& error);
    void serverConnected();
    void serverDisconnected();
    void radioList(const QList<smartLinkRadio>& radios);
    void connectReady(const QString& wanHandle, const QString& serial);

private slots:
    void onEncrypted();
    void onDisconnected();
    void onReadyRead();

private:
    void tokenRequest(const QByteArray& body);
    void connectToServer();
    void handleLine(const QString& line);

    QNetworkAccessManager nam;
    QTimer pingTimer;
    QSslSocket socket;
    QByteArray readBuffer;
    QString idToken;
    QString refreshToken;
    bool serverUp = false;
    bool requestInFlight = false;
};
