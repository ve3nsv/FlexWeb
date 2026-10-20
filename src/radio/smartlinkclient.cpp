// SmartLink (FlexRadio WAN) client. See include/smartlinkclient.h.
//
// Ported from AetherSDR (https://github.com/aethersdr/AetherSDR), Copyright
// the AetherSDR contributors, licensed GPLv3 like wfweb.

#include "smartlinkclient.h"
#include "logcategories.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QSslConfiguration>
#include <QStringList>
#include <QSysInfo>
#include <QUrl>

namespace {
const char* const kAuthUrl    = "https://frtest.auth0.com/oauth/token";
const char* const kClientId   = "4Y9fEIIsVYyQo5u6jr7yBWc4lV5ugC2m";
const char* const kServerHost = "smartlink.flexradio.com";
const quint16 kServerPort     = 443;
const int kMaxLine            = 1 << 20;

// Test hook (tests/test_flex.py): WFWEB_SMARTLINK_AUTH (token URL) and
// WFWEB_SMARTLINK_SERVER (host:port) point at a local mock. Its certificate
// is self-signed, so certificate checks are off in that mode only.
bool testMode()
{
    return qEnvironmentVariableIsSet("WFWEB_SMARTLINK_AUTH") || qEnvironmentVariableIsSet("WFWEB_SMARTLINK_SERVER");
}

QString authUrl()
{
    const QString env = qEnvironmentVariable("WFWEB_SMARTLINK_AUTH");
    return env.isEmpty() ? QString(kAuthUrl) : env;
}

QString serverHost()
{
    const QString env = qEnvironmentVariable("WFWEB_SMARTLINK_SERVER");
    return env.isEmpty() ? QString(kServerHost) : env.section(':', 0, 0);
}

quint16 serverPort()
{
    const QString env = qEnvironmentVariable("WFWEB_SMARTLINK_SERVER");
    return env.isEmpty() ? kServerPort : quint16(env.section(':', 1, 1).toUInt());
}

QSettings& store()
{
    // WFWEB_CONFIG_DIR (tests): an ini file there, as webAuth does.
    static QSettings s(qEnvironmentVariableIsEmpty("WFWEB_CONFIG_DIR")
                           ? QString() : qEnvironmentVariable("WFWEB_CONFIG_DIR") + "/smartlink.ini",
                       QSettings::IniFormat);
    static QSettings native("wfweb", "smartlink");
    return qEnvironmentVariableIsEmpty("WFWEB_CONFIG_DIR") ? native : s;
}

QMap<QString, QString> words(const QString& text)
{
    QMap<QString, QString> kv;
    for (const QString& w : text.split(' ', Qt::SkipEmptyParts)) {
        const int eq = w.indexOf('=');
        if (eq > 0)
            kv.insert(w.left(eq), w.mid(eq + 1));
    }
    return kv;
}
} // namespace

smartLinkClient::smartLinkClient(QObject* parent) : QObject(parent)
{
#if (QT_VERSION >= QT_VERSION_CHECK(5,15,0))
    nam.setTransferTimeout(15000);
#endif
    connect(&socket, &QSslSocket::encrypted, this, &smartLinkClient::onEncrypted);
    connect(&socket, &QSslSocket::disconnected, this, &smartLinkClient::onDisconnected);
    connect(&socket, &QSslSocket::readyRead, this, &smartLinkClient::onReadyRead);
#if (QT_VERSION < QT_VERSION_CHECK(5,15,0))
    connect(&socket, QOverload<QAbstractSocket::SocketError>::of(&QAbstractSocket::error), this, [this](QAbstractSocket::SocketError) {
#else
    connect(&socket, &QAbstractSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
#endif
        qWarning(logRig()) << "SmartLink: server connection error:" << socket.errorString();
        if (!serverUp)
            emit serverDisconnected();
    });
    pingTimer.setInterval(10000);
    connect(&pingTimer, &QTimer::timeout, this, [this]() {
        if (serverUp)
            socket.write("ping from client\n");
    });
}

smartLinkClient::~smartLinkClient()
{
    QObject::disconnect(&socket, nullptr, this, nullptr);
    close();
}

// ─── Account ───────────────────────────────────────────────────────────────

void smartLinkClient::login(const QString& email, const QString& password)
{
    QJsonObject body;
    body["grant_type"] = "http://auth0.com/oauth/grant-type/password-realm";
    body["realm"] = "Username-Password-Authentication";
    body["username"] = email;
    body["password"] = password;
    body["client_id"] = kClientId;
    body["scope"] = "openid email given_name family_name profile picture offline_access";
    store().setValue("email", email);
    tokenRequest(QJsonDocument(body).toJson(QJsonDocument::Compact));
}

bool smartLinkClient::loginWithStoredToken()
{
    const QString token = refreshToken.isEmpty() ? store().value("refreshToken").toString() : refreshToken;
    if (token.isEmpty())
        return false;
    QJsonObject body;
    body["grant_type"] = "refresh_token";
    body["refresh_token"] = token;
    body["client_id"] = kClientId;
    tokenRequest(QJsonDocument(body).toJson(QJsonDocument::Compact));
    return true;
}

void smartLinkClient::tokenRequest(const QByteArray& body)
{
    if (requestInFlight)
        return;
    requestInFlight = true;
    QNetworkRequest req{QUrl(authUrl())};
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QNetworkReply* reply = nam.post(req, body);
    if (testMode())
        reply->ignoreSslErrors();
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        requestInFlight = false;
        const QJsonObject resp = QJsonDocument::fromJson(reply->readAll()).object();
        if (reply->error() != QNetworkReply::NoError) {
            const QString err = resp.value("error_description").toString(reply->errorString());
            // A refused refresh token will never work again; drop it.
            if (resp.value("error").toString() == "invalid_grant")
                store().remove("refreshToken");
            emit authFailed(err);
            return;
        }
        idToken = resp.value("id_token").toString();
        const QString newRefresh = resp.value("refresh_token").toString();
        if (!newRefresh.isEmpty()) {
            refreshToken = newRefresh;
            store().setValue("refreshToken", refreshToken);
            store().sync();
        }
        if (idToken.isEmpty()) {
            emit authFailed("the sign-in answer had no id_token");
            return;
        }
        emit authenticated();
        connectToServer();
    });
}

bool smartLinkClient::haveStoredToken() { return !store().value("refreshToken").toString().isEmpty(); }
QString smartLinkClient::storedEmail() { return store().value("email").toString(); }

void smartLinkClient::forgetCredentials()
{
    store().remove("refreshToken");
    store().remove("email");
    store().sync();
}

QString smartLinkClient::certPin(const QString& host)
{
    return store().value("certPins/" + host).toString();
}

void smartLinkClient::setCertPin(const QString& host, const QString& sha256Hex)
{
    store().setValue("certPins/" + host, sha256Hex);
    store().sync();
}

void smartLinkClient::forgetCertPins()
{
    store().remove("certPins");
    store().sync();
}

// ─── SmartLink server ──────────────────────────────────────────────────────

void smartLinkClient::connectToServer()
{
    if (socket.state() != QAbstractSocket::UnconnectedState)
        socket.abort();
    readBuffer.clear();
    QSslConfiguration cfg = QSslConfiguration::defaultConfiguration();
    cfg.setPeerVerifyMode(testMode() ? QSslSocket::VerifyNone : QSslSocket::VerifyPeer);
    socket.setSslConfiguration(cfg);
    qInfo(logRig()) << "SmartLink: connecting to" << serverHost();
    socket.connectToHostEncrypted(serverHost(), serverPort());
}

void smartLinkClient::onEncrypted()
{
    serverUp = true;
    QString platform = QSysInfo::productType();
    platform.replace(' ', '_');
    socket.write(QString("application register name=wfweb platform=%1 token=%2\n").arg(platform, idToken).toUtf8());
    pingTimer.start();
    emit serverConnected();
}

void smartLinkClient::onDisconnected()
{
    pingTimer.stop();
    const bool was = serverUp;
    serverUp = false;
    if (was)
        emit serverDisconnected();
}

void smartLinkClient::close()
{
    pingTimer.stop();
    serverUp = false;
    if (socket.state() != QAbstractSocket::UnconnectedState) {
        socket.disconnectFromHost();
        if (socket.state() != QAbstractSocket::UnconnectedState)
            socket.abort();
    }
}

void smartLinkClient::requestConnect(const QString& serial, quint16 holePunchPort)
{
    if (!serverUp)
        return;
    socket.write(QString("application connect serial=%1 hole_punch_port=%2\n").arg(serial).arg(holePunchPort).toUtf8());
}

void smartLinkClient::onReadyRead()
{
    readBuffer.append(socket.readAll());
    int nl;
    while ((nl = readBuffer.indexOf('\n')) >= 0) {
        const QString line = QString::fromUtf8(readBuffer.left(nl)).trimmed();
        readBuffer.remove(0, nl + 1);
        if (!line.isEmpty())
            handleLine(line);
    }
    if (readBuffer.size() > kMaxLine) {
        readBuffer.clear();
        socket.abort();
    }
}

void smartLinkClient::handleLine(const QString& line)
{
    if (line.startsWith("radio list")) {
        emit radioList(parseRadioList(line));
    } else if (line.startsWith("radio connect_ready")) {
        const auto kv = words(line);
        emit connectReady(kv.value("handle"), kv.value("serial"));
    } else if (line.startsWith("application registration_invalid")) {
        close();
        store().remove("refreshToken");
        emit authFailed("SmartLink refused the sign-in; sign in again with --smartlink-login");
    } else if (!line.startsWith("ping") && !line.startsWith("application user_settings")) {
        qDebug(logRig()).noquote() << "SmartLink <" << line.left(160);
    }
}

QList<smartLinkRadio> smartLinkClient::parseRadioList(const QString& line)
{
    // "radio list name=... serial=...|name=... serial=...|"
    QList<smartLinkRadio> out;
    QString body = line;
    body.remove(0, QString("radio list").length());
    for (const QString& part : body.split('|', Qt::SkipEmptyParts)) {
        const auto kv = words(part);
        smartLinkRadio r;
        r.serial = kv.value("serial");
        if (r.serial.isEmpty())
            continue;
        r.nickname = kv.value("radio_name", kv.value("name"));
        r.callsign = kv.value("callsign");
        r.model = kv.value("model");
        r.status = kv.value("status", "Unknown");
        r.publicIp = kv.value("public_ip");
        r.upnp = kv.value("upnp_supported") == "1";
        r.guiClients = kv.value("gui_client_programs");
        const int tls = kv.value("public_tls_port", "-1").toInt();
        const int udp = kv.value("public_udp_port", "-1").toInt();
        const int upTls = kv.value("public_upnp_tls_port", "-1").toInt();
        const int upUdp = kv.value("public_upnp_udp_port", "-1").toInt();
        // Manual port forwards first, then UPnP (FlexLib WanServer order).
        if (tls > 0 && udp > 0) {
            r.publicTlsPort = tls;
            r.publicUdpPort = udp;
        } else if (r.upnp && upTls > 0 && upUdp > 0) {
            r.publicTlsPort = upTls;
            r.publicUdpPort = upUdp;
        }
        r.needsHolePunch = r.publicTlsPort <= 0;
        out.append(r);
    }
    return out;
}

int smartLinkClient::findRadio(const QList<smartLinkRadio>& radios, const QString& name)
{
    const QString n = name.trimmed();
    if (n.isEmpty() || n.compare("auto", Qt::CaseInsensitive) == 0)
        return radios.size() == 1 ? 0 : -1;
    for (int i = 0; i < radios.size(); ++i)
        if (radios[i].serial.compare(n, Qt::CaseInsensitive) == 0)
            return i;
    for (int i = 0; i < radios.size(); ++i)
        if (radios[i].nickname.compare(n, Qt::CaseInsensitive) == 0)
            return i;
    return -1;
}
