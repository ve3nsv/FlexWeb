// Ported from VE3NSV's RF-KIT support for AetherSDR (github.com/ve3nsv/
// AetherSDR, branch rfkit-tune-pr, 934dcd6), GPLv3. "Constitution principle"
// references are AetherSDR's project rules. Changed in FlexWeb (punch list 42,
// 43; not yet in rfkit-tune-pr): one request at a time on a receive / transmit
// schedule instead of three every 250 ms, and the tune-API probe is no longer
// sent on connect - it wakes the amplifier, so it is sent only as wake().

#include "rfkitconnection.h"
#include "logcategories.h"

#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QDateTime>
#include <QStringList>

#include <iterator>

namespace AetherSDR {

namespace {

QString path(const char* literal)
{
    return QString::fromLatin1(literal);
}

qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

// Receive schedule, one GET per 250 ms tick (3 s cycle): /tuner and
// /operate-mode each 1/s (a band change, OPERATE pressed on the amp's own
// screen), /power and /data every 1.5 s (in receive /power only carries
// temperature, volts and amps), the antenna list every 3 s. An empty slot
// reads /operational-interface when it is 10 s old, else sends nothing.
// About 3.7 GETs/s against 12 before (PgTG's RF-KIT plugin: ~5).
const char* const kRxSchedule[] = {
    Rfkit::kPathPower, Rfkit::kPathTuner, Rfkit::kPathOperateMode, Rfkit::kPathData,
    Rfkit::kPathTuner, Rfkit::kPathOperateMode, Rfkit::kPathPower, Rfkit::kPathTuner,
    Rfkit::kPathOperateMode, Rfkit::kPathData, Rfkit::kPathAntennas, nullptr,
};
// Transmit schedule, one GET per 50 ms tick at most: /power four times in
// five (FWD on voice peaks), /tuner once (a tune in progress). Everything else
// waits until the radio is back in receive.
const char* const kTxSchedule[] = {
    Rfkit::kPathPower, Rfkit::kPathPower, Rfkit::kPathPower, Rfkit::kPathPower, Rfkit::kPathTuner,
};

}  // namespace

// The tune start itself: PUT /tuner that is not the discovery probe.
bool RfkitConnection::isTuneStart(const Request& request)
{
    return request.method == Request::Method::Put
        && request.path == QLatin1String(Rfkit::kPathTuner) && !request.tuneProbe;
}

RfkitConnection::RfkitConnection(QObject* parent)
    : QObject(parent)
{
    // The amplifier is a LAN device addressed by IP; never route it through a
    // system proxy the operator configured for the internet.
    m_nam.setProxy(QNetworkProxy::NoProxy);

    // Retries indefinitely until the amp returns or the user disconnects —
    // same precedent as every other peripheral connection here, for a device
    // that may legitimately be power-cycling.
    m_reconnectTimer.setSingleShot(true);
    connect(&m_reconnectTimer, &QTimer::timeout, this, [this]() {
        if (m_connected || m_lastHost.isEmpty()) { return; }
        connectNetwork(m_lastHost, m_lastPort);
    });

    m_pollTimer.setInterval(m_rxPollMs);
    connect(&m_pollTimer, &QTimer::timeout, this, &RfkitConnection::poll);

    m_deadlineTimer.setSingleShot(true);
    m_deadlineTimer.setInterval(kRequestDeadlineMs);
    connect(&m_deadlineTimer, &QTimer::timeout, this, [this]() {
        if (m_reply) {
            m_reply->setProperty("rfkitDeadline", true);
            m_reply->abort();  // finished() follows and counts it as a transport failure
        }
    });
}

RfkitConnection::~RfkitConnection()
{
    // Invalidate in-flight replies so a late finished() cannot reach a
    // half-destroyed object.
    ++m_epoch;
    if (m_reply) {
        m_reply->abort();
    }
}

QString RfkitConnection::description() const
{
    if (m_lastHost.isEmpty()) {
        return {};
    }
    return QStringLiteral("%1:%2").arg(m_lastHost).arg(m_lastPort);
}

void RfkitConnection::connectNetwork(const QString& host, quint16 port)
{
    if (m_connected) {
        disconnect();
    }
    const QString trimmed = host.trimmed();
    m_lastHost = trimmed;
    m_lastPort = port;
    m_deliberateDisconnect = false;
    m_reconnectTimer.stop();
    resetSession();

    // Build the URL from parts rather than concatenating text, and refuse
    // anything QUrl does not accept as a bare host: the address field is free
    // text, and a stray '/', '@' or space must not turn into a different
    // request target.
    QUrl url;
    url.setScheme(QStringLiteral("http"));
    url.setHost(trimmed, QUrl::StrictMode);
    url.setPort(port);
    if (trimmed.isEmpty() || port == 0 || !url.isValid() || url.host().isEmpty()) {
        qCWarning(logRig) << "RfkitConnection: invalid address" << host << port;
        emit connectionFailed(tr("Invalid address"));
        return;
    }
    m_baseUrl = url;

    qCDebug(logRig) << "RfkitConnection: connecting to" << description();
    Request probe;
    probe.method = Request::Method::Get;
    probe.path = path(Rfkit::kPathInfo);
    probe.probe = true;
    enqueue(probe);
}

void RfkitConnection::disconnect()
{
    // Self-contained rather than relying on a transport signal to fire
    // disconnected() — the ACOM row learned that the hard way (see
    // AcomConnection::disconnect()): a user-initiated disconnect that never
    // emits disconnected() leaves the applet stuck showing "Connected".
    const bool wasConnected = m_connected;
    m_deliberateDisconnect = true;
    m_reconnectTimer.stop();
    m_connected = false;
    resetSession();  // bumps the epoch: a pending request must not resurrect this
    if (wasConnected) {
        qCDebug(logRig) << "RfkitConnection: disconnected";
        emit disconnected();
    }
    m_deliberateDisconnect = false;
}

void RfkitConnection::resetSession()
{
    ++m_epoch;
    m_pollTimer.stop();
    m_deadlineTimer.stop();
    m_queue.clear();
    m_inFlight = false;
    if (m_reply) {
        QNetworkReply* reply = m_reply;
        m_reply = nullptr;
        reply->abort();  // its finished() sees the old epoch and discards it
    }
    m_status = {};
    m_lastBody.clear();
    m_warnedPaths.clear();
    m_consecutiveFailures = 0;
    m_slowPollIndex = 0;
    m_pollPausedUntilMs = 0;
    m_lastAntennasMs = 0;
    m_lastInterfaceMs = 0;
    m_tuneProbePending = false;
    m_tuneStartPending = false;
}

void RfkitConnection::enqueue(const Request& request, bool front)
{
    if (front) {
        // Ahead of queued polls, behind earlier commands: two clicks must
        // reach the amplifier in the order they were made.
        Request command = request;
        command.command = true;
        qsizetype at = 0;
        while (at < m_queue.size() && m_queue.at(at).command) {
            ++at;
        }
        m_queue.insert(at, command);
    } else {
        m_queue.enqueue(request);
    }
    pump();
}

void RfkitConnection::pump()
{
    if (m_inFlight || m_queue.isEmpty()) {
        return;
    }
    const Request request = m_queue.dequeue();

    QUrl url = m_baseUrl;
    url.setPath(request.path);
    QNetworkRequest netRequest(url);
    netRequest.setTransferTimeout(kRequestTimeoutMs);
    netRequest.setRawHeader("Accept", "application/json");
    // No redirects: a device that redirects is not speaking this API.
    netRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                            QNetworkRequest::ManualRedirectPolicy);
    netRequest.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                            QNetworkRequest::AlwaysNetwork);

    QNetworkReply* reply = nullptr;
    switch (request.method) {
    case Request::Method::Get:
        reply = m_nam.get(netRequest);
        break;
    case Request::Method::Put:
        netRequest.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        reply = m_nam.put(netRequest, request.body);
        break;
    case Request::Method::Post:
        netRequest.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        reply = m_nam.post(netRequest, request.body);
        break;
    }

    m_inFlight = true;
    m_sentAtMs = nowMs();
    m_reply = reply;
    m_deadlineTimer.start();
    const int epoch = m_epoch;

    // Principle VII: bound what a LAN peer can make us buffer. The transfer
    // timeout only aborts a transfer that goes silent for its interval; this
    // bounds the size.
    connect(reply, &QNetworkReply::downloadProgress, reply, [reply](qint64 received, qint64) {
        if (received > Rfkit::kMaxBodyBytes) {
            reply->setProperty("rfkitOversize", true);
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, request, epoch]() {
        onReplyFinished(reply, request, epoch);
    });
}

void RfkitConnection::onReplyFinished(QNetworkReply* reply, const Request& request, int epoch)
{
    reply->deleteLater();
    if (epoch != m_epoch) {
        return;  // a reply from a session that has since ended
    }
    m_inFlight = false;
    m_reply = nullptr;
    if (isTuneStart(request)) {
        m_tuneStartPending = false;
    }
    m_deadlineTimer.stop();

    const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool oversize = reply->property("rfkitOversize").toBool();

    // No HTTP status at all means the exchange never completed: refused,
    // timed out, unresolvable, reset, or aborted for size or time. A transfer
    // that broke after the headers (reset or timeout mid-body) has a status of
    // 200 but a network-level error, so that counts too. HTTP error statuses
    // set a content/protocol error (201 and up) and are the device answering,
    // not link loss.
    const bool deadline = reply->property("rfkitDeadline").toBool();
    const QNetworkReply::NetworkError netError = reply->error();
    const bool brokeInTransit = netError != QNetworkReply::NoError
        && netError < QNetworkReply::ContentAccessDenied;
    if (oversize || deadline || http == 0 || brokeInTransit) {
        onTransportFailure(oversize ? tr("Response too large")
                           : deadline ? tr("Response took too long")
                                      : reply->errorString(), request);
        return;
    }

    m_consecutiveFailures = 0;
    m_reconnectStep = 0;
    // The amp answered, but slowly: give it as long again before the next
    // poll (punch list 46). A wake is slow by nature (~5 s) - not counted.
    const qint64 took = nowMs() - m_sentAtMs;
    if (!request.tuneProbe && took > 1000) {
        m_pollPausedUntilMs = nowMs() + qMin<qint64>(took, kRequestTimeoutMs);
        qCInfo(logRig) << "RfkitConnection: slow answer on" << request.path << took << "ms - polling paused as long";
    }
    // Read only now: a reply aborted for time or size has no open device.
    const QByteArray body = oversize ? QByteArray() : reply->read(Rfkit::kMaxBodyBytes + 1);

    if (request.tuneProbe) {
        // Not an error path: a stock server's 405 is the expected answer.
        m_tuneProbePending = false;
        emit wakeAnswered(true);
        const Rfkit::TuneApi api = Rfkit::classifyTuneProbe(http);
        if (api != m_status.tuneApi) {
            m_status.tuneApi = api;
            qCInfo(logRig) << "RfkitConnection: tune via REST"
                            << (api == Rfkit::TuneApi::Supported ? "supported"
                                : api == Rfkit::TuneApi::Unsupported ? "not supported"
                                : "unknown") << "(HTTP" << http << ")";
            emit statusUpdated(m_status);
        }
        pump();
        return;
    }

    if (http < 200 || http >= 300) {
        const auto apiError = Rfkit::parseApiError(body);
        const QString reason = apiError && !apiError->reason.isEmpty()
            ? apiError->reason : QStringLiteral("HTTP %1").arg(http);
        const char* method = "GET";
        switch (request.method) {
        case Request::Method::Get:  method = "GET"; break;
        case Request::Method::Put:  method = "PUT"; break;
        case Request::Method::Post: method = "POST"; break;
        }
        qCWarning(logRig).noquote() << "RfkitConnection:" << method
                                     << request.path << "refused:" << http << reason;
        if (request.probe) {
            // Answering the handshake with an error status means this is not
            // an RF-KIT (or not one reachable on this port).
            resetSession();
            emit connectionFailed(tr("Not an RF-KIT amplifier (HTTP %1)").arg(http));
            if (!m_deliberateDisconnect && m_autoReconnect) { armReconnect(); }
            return;
        }
        pump();
        return;
    }

    if (request.probe) {
        const auto info = Rfkit::parseInfo(body);
        if (!info) {
            resetSession();
            emit connectionFailed(tr("Not an RF-KIT amplifier (unexpected /info reply)"));
            if (!m_deliberateDisconnect && m_autoReconnect) { armReconnect(); }
            return;
        }
        m_status.info = *info;
        onProbeSucceeded();
        return;
    }

    if (applyReply(request, body)) {
        emit statusUpdated(m_status);
    }
    pump();
}

bool RfkitConnection::applyReply(const Request& request, const QByteArray& body)
{
    const QString& p = request.path;
    const auto malformed = [this, &p]() {
        if (!m_warnedPaths.contains(p)) {
            m_warnedPaths.insert(p);
            qCWarning(logRig) << "RfkitConnection: unparseable reply from" << p;
        }
        return false;
    };

    // Writes: the reply confirms the new state, and anything it does not
    // carry is refreshed.
    if (request.method == Request::Method::Post) {
        // POST /error/reset. Nothing in the reply is needed; re-read what an
        // error can have changed.
        enqueue(Request{Request::Method::Get, path(Rfkit::kPathData), {}, false, false, false});
        enqueue(Request{Request::Method::Get, path(Rfkit::kPathInterface), {}, false, false, false});
        return false;
    }
    if (request.method == Request::Method::Put && p == QLatin1String(Rfkit::kPathTuner)) {
        // PUT /tuner {"tuner_mode":"START"}. The reply is not trusted as the
        // new state (Principle II): read the tuner and the status text back.
        enqueue(Request{Request::Method::Get, path(Rfkit::kPathTuner), {}, false, false, false});
        enqueue(Request{Request::Method::Get, path(Rfkit::kPathData), {}, false, false, false});
        return false;
    }
    if (request.method == Request::Method::Put && p == QLatin1String(Rfkit::kPathAntennaActive)) {
        // The reply names only the new active antenna; the per-antenna states
        // come from the list. Re-read it whatever the reply looks like: a 2xx
        // means the switch was accepted, and the highlight must follow it.
        enqueue(Request{Request::Method::Get, path(Rfkit::kPathAntennas), {}, false, false, false});
        if (!Rfkit::parseAntenna(body)) {
            return malformed();
        }
        return false;
    }

    if (p == QLatin1String(Rfkit::kPathAntennas)) m_lastAntennasMs = nowMs();
    if (p == QLatin1String(Rfkit::kPathInterface)) m_lastInterfaceMs = nowMs();
    if (p == QLatin1String(Rfkit::kPathPower)) {
        const auto v = Rfkit::parsePower(body);
        if (!v) { return malformed(); }
        if (m_transmitting) ++m_overPowerReads;
        m_status.power = *v;
    } else if (p == QLatin1String(Rfkit::kPathData)) {
        const auto v = Rfkit::parseData(body);
        if (!v) { return malformed(); }
        m_status.data = *v;
    } else if (p == QLatin1String(Rfkit::kPathTuner)) {
        const auto v = Rfkit::parseTuner(body);
        if (!v) { return malformed(); }
        m_status.tuner = *v;
    } else if (p == QLatin1String(Rfkit::kPathOperateMode)) {
        const auto v = Rfkit::parseOperateMode(body);
        if (!v) { return malformed(); }
        m_status.operate = *v;
    } else if (p == QLatin1String(Rfkit::kPathInterface)) {
        const auto v = Rfkit::parseInterface(body);
        if (!v) { return malformed(); }
        m_status.interfaceState = *v;
    } else if (p == QLatin1String(Rfkit::kPathAntennas)) {
        const auto v = Rfkit::parseAntennas(body);
        if (!v) { return malformed(); }
        m_status.antennas = *v;
    } else {
        return false;
    }

    const bool same = m_lastBody.value(p) == body;
    m_lastBody.insert(p, body);
    return !same;
}

void RfkitConnection::onProbeSucceeded()
{
    m_connected = true;
    m_consecutiveFailures = 0;
    m_slowPollIndex = 0;
    qCInfo(logRig) << "RfkitConnection: connected to" << description()
                    << (m_status.info ? m_status.info->device : QString());
    updatePollRate();
    m_pollTimer.start();
    emit connected();
    // Read everything once so the applet fills in immediately instead of
    // waiting a full slow-poll rotation for the configuration readbacks.
    enqueueFullRefresh();
    emit statusUpdated(m_status);
}

void RfkitConnection::enqueueFullRefresh()
{
    static const char* const kPaths[] = {
        Rfkit::kPathPower,    Rfkit::kPathData,  Rfkit::kPathOperateMode,
        Rfkit::kPathAntennas, Rfkit::kPathTuner, Rfkit::kPathInterface,
    };
    for (const char* literal : kPaths) {
        enqueue(Request{Request::Method::Get, path(literal), {}, false, false, false});
    }
    // No tune-API probe here any more: it wakes the amplifier (punch list 42).
    // The host calls wake() when someone who may transmit is on.
}

void RfkitConnection::wake()
{
    if (!m_connected || m_tuneProbePending) {
        return;
    }
    m_tuneProbePending = true;
    Request request;
    request.method = Request::Method::Put;
    request.path = path(Rfkit::kPathTuner);
    request.body = Rfkit::buildTunerProbeBody();
    request.tuneProbe = true;
    enqueue(request, true);
}

void RfkitConnection::setTransmitting(bool on)
{
    if (on == m_transmitting) {
        return;
    }
    m_transmitting = on;
    if (on) {
        m_overStartMs = nowMs();
        m_overPowerReads = 0;
    } else {
        m_fastUntilMs = nowMs() + kTxHoldMs;  // keep the peak hold fed after unkey
        const double secs = (nowMs() - m_overStartMs) / 1000.0;
        if (m_connected && m_overStartMs > 0 && secs >= 0.5) {
            qCInfo(logRig).noquote() << QString("RF-KIT: over %1 s, %2 /power reads = %3/s (asked every %4 ms)")
                .arg(secs, 0, 'f', 1).arg(m_overPowerReads).arg(m_overPowerReads / secs, 0, 'f', 1).arg(m_txPollMs);
        }
    }
    updatePollRate();
}

void RfkitConnection::setPollIntervals(int rxMs, int txMs)
{
    m_rxPollMs = qBound(100, rxMs, 5000);
    m_txPollMs = qBound(10, txMs, 1000);
    updatePollRate();
}

void RfkitConnection::kickFast()
{
    m_fastUntilMs = qMax(m_fastUntilMs, nowMs() + kTxHoldMs);
    updatePollRate();
}

bool RfkitConnection::fastMode() const
{
    return m_transmitting || nowMs() < m_fastUntilMs;
}

void RfkitConnection::updatePollRate()
{
    const int want = fastMode() ? m_txPollMs : m_rxPollMs;
    if (m_pollTimer.interval() != want) {
        m_pollTimer.setInterval(want);
        m_slowPollIndex = 0;
    }
}

void RfkitConnection::onTransportFailure(const QString& errorString, const Request& request)
{
    qCWarning(logRig) << "RfkitConnection: transport error on" << request.path << errorString;
    if (request.tuneProbe) {
        m_tuneProbePending = false;
        emit wakeAnswered(false);
        if (m_connected) {
            // A wake that got no answer does not count toward dropping the
            // link (punch list 46); the polls decide that.
            m_pollPausedUntilMs = nowMs() + 5000;
            pump();
            return;
        }
    }
    if (isTuneStart(request)) {
        m_tuneStartPending = false;
    }
    if (request.probe || !m_connected) {
        resetSession();
        emit connectionFailed(errorString);
        if (!m_deliberateDisconnect && m_autoReconnect) { armReconnect(); }
        return;
    }
    if (++m_consecutiveFailures >= kMaxConsecutiveFailures) {
        onTransportDown(errorString);
        return;
    }
    m_pollPausedUntilMs = nowMs() + 5000;  // give a struggling amp room
    pump();  // drop the failed request and carry on with the rest
}

void RfkitConnection::onTransportDown(const QString& errorString)
{
    const bool wasConnected = m_connected;
    m_connected = false;
    resetSession();
    if (wasConnected) {
        qCDebug(logRig) << "RfkitConnection: disconnected";
        emit disconnected();
    }
    emit connectionFailed(errorString);
    if (!m_deliberateDisconnect && m_autoReconnect) {
        armReconnect();
    }
}

void RfkitConnection::armReconnect()
{
    if (m_reconnectTimer.isActive()) {
        return;
    }
    static const int kSteps[] = {5000, 15000, 30000, 60000};
    const int ms = kSteps[qMin(m_reconnectStep, int(std::size(kSteps)) - 1)];
    if (m_reconnectStep > 0) {
        qCInfo(logRig) << "RfkitConnection: amp still not answering, next try in" << ms / 1000 << "s";
    }
    ++m_reconnectStep;
    m_reconnectTimer.start(ms);
}

void RfkitConnection::poll()
{
    // Never stack a poll round behind unfinished work: a slow device lowers
    // the poll rate instead of growing a backlog.
    updatePollRate();  // the hold after unkey ends here
    if (!m_connected || m_inFlight || !m_queue.isEmpty()) {
        return;
    }
    if (nowMs() < m_pollPausedUntilMs) {
        return;  // the amp was slow or failed: let it catch up
    }
    const bool fast = fastMode();
    const int n = fast ? int(std::size(kTxSchedule)) : int(std::size(kRxSchedule));
    const char* literal = (fast ? kTxSchedule : kRxSchedule)[m_slowPollIndex % n];
    m_slowPollIndex = (m_slowPollIndex + 1) % n;
    if (!literal) {
        // The spare receive slot: the configuration readbacks when they are due.
        if (nowMs() - m_lastInterfaceMs >= kSlowPathMs) {
            literal = Rfkit::kPathInterface;
        } else {
            return;
        }
    }
    if (literal == Rfkit::kPathAntennas && nowMs() - m_lastAntennasMs < kSlowPathMs / 4) {
        return;  // just read (after a switch, or at connect)
    }
    enqueue(Request{Request::Method::Get, path(literal), {}, false, false, false});
}

void RfkitConnection::setOperate(bool operate)
{
    if (!m_connected) {
        qCDebug(logRig) << "RfkitConnection: dropping operate command, not connected";
        return;
    }
    Request request;
    request.method = Request::Method::Put;
    request.path = path(Rfkit::kPathOperateMode);
    request.body = Rfkit::buildOperateModeBody(operate ? Rfkit::OperateMode::Operate
                                                       : Rfkit::OperateMode::Standby);
    enqueue(request, true);
}

void RfkitConnection::selectAntenna(const Rfkit::Antenna& antenna)
{
    if (!m_connected) {
        qCDebug(logRig) << "RfkitConnection: dropping antenna command, not connected";
        return;
    }
    const QByteArray body = Rfkit::buildAntennaBody(antenna);
    if (body.isEmpty()) {
        qCWarning(logRig) << "RfkitConnection: antenna" << Rfkit::antennaLabel(antenna)
                           << "out of range — ignoring";
        return;
    }
    Request request;
    request.method = Request::Method::Put;
    request.path = path(Rfkit::kPathAntennaActive);
    request.body = body;
    enqueue(request, true);
}

void RfkitConnection::resetError()
{
    if (!m_connected) {
        qCDebug(logRig) << "RfkitConnection: dropping error reset, not connected";
        return;
    }
    Request request;
    request.method = Request::Method::Post;
    request.path = path(Rfkit::kPathErrorReset);
    enqueue(request, true);
}

void RfkitConnection::startTune()
{
    if (!m_connected) {
        qCDebug(logRig) << "RfkitConnection: dropping tune command, not connected";
        return;
    }
    if (m_status.tuneApi != Rfkit::TuneApi::Supported) {
        qCWarning(logRig) << "RfkitConnection: tune refused, this amplifier's REST server "
                              "has no tune command";
        return;
    }
    if (!m_status.tuner || !Rfkit::tuneAllowed(*m_status.tuner)) {
        qCWarning(logRig) << "RfkitConnection: tune refused, the tuner is not idle in "
                              "automatic or manual mode";
        return;
    }
    if (m_tuneStartPending) {
        qCDebug(logRig) << "RfkitConnection: tune already requested, ignoring";
        return;
    }
    m_tuneStartPending = true;
    Request request;
    request.method = Request::Method::Put;
    request.path = path(Rfkit::kPathTuner);
    request.body = Rfkit::buildTunerStartBody();
    enqueue(request, true);
}

}  // namespace AetherSDR
