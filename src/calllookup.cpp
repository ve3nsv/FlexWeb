// Callsign lookup for access requests. See include/calllookup.h.

#include "calllookup.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>
#include <QXmlStreamReader>


callLookup::callLookup(QObject* parent) : QObject(parent), nam(new QNetworkAccessManager(this)) {}

void callLookup::configure(const QString& dataDir, const QString& hamqthUser, const QString& hamqthPassword,
                           const QString& qrzUser, const QString& qrzPassword)
{
    dataDir_ = dataDir;
    hamqthUser_ = hamqthUser;
    hamqthPassword_ = hamqthPassword;
    qrzUser_ = qrzUser;
    qrzPassword_ = qrzPassword;
    hamqthSession_.clear();
    qrzKey_.clear();
}

// ── network side ───────────────────────────────────────────────────────────

void callLookup::get(const QString& url, std::function<void(int, QByteArray, QString)> done)
{
    QNetworkRequest req{QUrl(url)};
    req.setHeader(QNetworkRequest::UserAgentHeader, "FlexWeb");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    req.setTransferTimeout(60000);
#endif
    QNetworkReply* reply = nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [reply, done]() {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        done(status, reply->readAll(), reply->error() == QNetworkReply::NoError ? QString() : reply->errorString());
    });
}

void callLookup::lookup(const QString& callIn)
{
    const QString call = callIn.trimmed().toUpper();
    if (isCanadian(call) && !dataDir_.isEmpty()) lookupIsed(call);
    else lookupOther(call);
}

QString callLookup::isedPath() const { return dataDir_ + "/amateur_delim.txt"; }

bool callLookup::isedFresh() const
{
    const QFileInfo fi(isedPath());
    return fi.exists() && fi.size() > 1000 && fi.lastModified().secsTo(QDateTime::currentDateTime()) < 7 * 86400;
}

void callLookup::lookupIsed(const QString& call)
{
    if (!isedFresh()) {
        isedWaiting_ << call;
        fetchIsed();
        return;
    }
    QFile f(isedPath());
    QJsonObject o;
    if (f.open(QIODevice::ReadOnly)) o = findIsed(f.readAll(), call);
    if (!o.value("found").toBool() && (!qrzUser_.isEmpty() || !hamqthUser_.isEmpty())) {
        lookupOther(call);   // not in ISED's list: maybe the others know it
        return;
    }
    emit looked(call, o);
}

void callLookup::fetchIsed()
{
    if (isedFetching_) return;
    isedFetching_ = true;
    qInfo() << "Lookup: fetching ISED's amateur list";
    QPointer<callLookup> self(this);
    get(QString::fromLatin1(kIsedUrl), [self](int status, QByteArray body, QString err) {
        if (!self) return;
        self->isedFetching_ = false;
        QString why = err;
        if (why.isEmpty() && status != 200) why = QString("HTTP %1").arg(status);
        if (why.isEmpty()) {
            const QByteArray txt = zipEntry(body, ".txt", &why);
            if (!txt.isEmpty()) {
                QDir().mkpath(self->dataDir_);
                QFile f(self->isedPath());
                if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    f.write(txt);
                    f.close();
                    qInfo() << "Lookup: ISED amateur list updated," << txt.count('\n') << "lines";
                } else {
                    why = "cannot write " + self->isedPath();
                }
            }
        }
        const QStringList waiting = self->isedWaiting_;
        self->isedWaiting_.clear();
        if (!why.isEmpty()) {
            qWarning().noquote() << "Lookup: ISED amateur list:" << why;
            // An older copy is better than nothing.
            if (QFileInfo::exists(self->isedPath())) {
                for (const QString& c : waiting) {
                    QFile g(self->isedPath());
                    QJsonObject o;
                    if (g.open(QIODevice::ReadOnly)) o = findIsed(g.readAll(), c);
                    emit self->looked(c, o);
                }
                return;
            }
            for (const QString& c : waiting) {
                QJsonObject o;
                o["source"] = "ISED";
                o["found"] = false;
                o["error"] = "ISED list unavailable: " + why;
                emit self->looked(c, o);
            }
            return;
        }
        for (const QString& c : waiting) self->lookupIsed(c);
    });
}

void callLookup::lookupOther(const QString& call)
{
    if (!qrzUser_.isEmpty()) { qrz(call, false); return; }
    if (!hamqthUser_.isEmpty()) { hamqth(call, false); return; }
    QJsonObject o;
    o["source"] = "";
    o["found"] = false;
    o["error"] = isCanadian(call) ? "not in ISED's list" : "no lookup configured (HamQTH or QRZ in [Lookup])";
    emit looked(call, o);
}

void callLookup::hamqth(const QString& call, bool retried)
{
    QPointer<callLookup> self(this);
    auto fail = [self, call](const QString& e) {
        if (!self) return;
        QJsonObject o;
        o["source"] = "HamQTH";
        o["found"] = false;
        o["error"] = e;
        emit self->looked(call, o);
    };
    if (hamqthSession_.isEmpty()) {
        QUrlQuery q;
        q.addQueryItem("u", hamqthUser_);
        q.addQueryItem("p", hamqthPassword_);
        get("https://www.hamqth.com/xml.php?" + q.toString(QUrl::FullyEncoded),
            [self, call, retried, fail](int, QByteArray body, QString err) {
                if (!self) return;
                self->hamqthSession_ = xmlValue(body, "session_id");
                if (self->hamqthSession_.isEmpty()) { fail(err.isEmpty() ? xmlValue(body, "error") : err); return; }
                self->hamqth(call, retried);
            });
        return;
    }
    QUrlQuery q;
    q.addQueryItem("id", hamqthSession_);
    q.addQueryItem("callsign", call);
    q.addQueryItem("prg", "FlexWeb");
    get("https://www.hamqth.com/xml.php?" + q.toString(QUrl::FullyEncoded),
        [self, call, retried, fail](int, QByteArray body, QString err) {
            if (!self) return;
            if (!err.isEmpty()) { fail(err); return; }
            const QString e = xmlValue(body, "error");
            if (e.contains("session", Qt::CaseInsensitive) && !retried) {
                self->hamqthSession_.clear();
                self->hamqth(call, true);
                return;
            }
            emit self->looked(call, parseHamQth(body));
        });
}

void callLookup::qrz(const QString& call, bool retried)
{
    QPointer<callLookup> self(this);
    auto fail = [self, call](const QString& e) {
        if (!self) return;
        QJsonObject o;
        o["source"] = "QRZ";
        o["found"] = false;
        o["error"] = e;
        emit self->looked(call, o);
    };
    if (qrzKey_.isEmpty()) {
        QUrlQuery q;
        q.addQueryItem("username", qrzUser_);
        q.addQueryItem("password", qrzPassword_);
        q.addQueryItem("agent", "FlexWeb");
        get("https://xmldata.qrz.com/xml/current/?" + q.toString(QUrl::FullyEncoded),
            [self, call, retried, fail](int, QByteArray body, QString err) {
                if (!self) return;
                self->qrzKey_ = xmlValue(body, "Key");
                if (self->qrzKey_.isEmpty()) { fail(err.isEmpty() ? xmlValue(body, "Error") : err); return; }
                self->qrz(call, retried);
            });
        return;
    }
    QUrlQuery q;
    q.addQueryItem("s", qrzKey_);
    q.addQueryItem("callsign", call);
    get("https://xmldata.qrz.com/xml/current/?" + q.toString(QUrl::FullyEncoded),
        [self, call, retried, fail](int, QByteArray body, QString err) {
            if (!self) return;
            if (!err.isEmpty()) { fail(err); return; }
            const QString e = xmlValue(body, "Error");
            if (xmlValue(body, "Key").isEmpty() && !retried) {
                // The session expired: sign in again once.
                self->qrzKey_.clear();
                self->qrz(call, true);
                return;
            }
            Q_UNUSED(e)
            emit self->looked(call, parseQrz(body));
        });
}
