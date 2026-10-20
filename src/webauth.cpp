// Web page login. See include/webauth.h.

#include "webauth.h"

#include <QCryptographicHash>
#include <QPasswordDigestor>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSettings>

#include <memory>

namespace {

const int kSessionShortSecs = 12 * 3600;
const int kSessionLongSecs = 30 * 24 * 3600;

// A fresh QSettings each time, synced, so users added by another process
// (wfweb --add-user, the launcher) are seen by a running server.
// WFWEB_CONFIG_DIR (tests): an ini file there instead of the platform's
// settings store, which on macOS ignores $HOME.
QString storePath()
{
    const QString dir = qEnvironmentVariable("WFWEB_CONFIG_DIR");
    return dir.isEmpty() ? QString() : dir + "/webusers.ini";
}

std::unique_ptr<QSettings> openStore()
{
    const QString path = storePath();
    std::unique_ptr<QSettings> s(path.isEmpty() ? new QSettings("wfweb", "webusers")
                                                : new QSettings(path, QSettings::IniFormat));
    s->sync();
    return s;
}

QByteArray randomBytes(int n)
{
    QByteArray b(n, Qt::Uninitialized);
    QRandomGenerator* g = QRandomGenerator::system();
    for (int i = 0; i < n; ++i)
        b[i] = char(g->bounded(256));
    return b;
}

QString tokenKey(const QString& token)
{
    return "sessions/" + QString::fromLatin1(QCryptographicHash::hash(token.toUtf8(), QCryptographicHash::Sha256).toHex());
}

bool sameBytes(const QByteArray& a, const QByteArray& b)
{
    if (a.size() != b.size())
        return false;
    unsigned char diff = 0;
    for (int i = 0; i < a.size(); ++i)
        diff |= uchar(a[i]) ^ uchar(b[i]);
    return diff == 0;
}

} // namespace

// ─── users ─────────────────────────────────────────────────────────────────

bool webAuth::validUserName(const QString& name)
{
    static const QRegularExpression re("^[A-Za-z0-9._-]{1,32}$");
    return re.match(name).hasMatch();
}

QStringList webAuth::users()
{
    auto s = openStore();
    s->beginGroup("users");
    QStringList out = s->childKeys();
    out.sort(Qt::CaseInsensitive);
    return out;
}

bool webAuth::hasUsers() { return !users().isEmpty(); }

bool webAuth::userExists(const QString& name)
{
    return validUserName(name) && openStore()->contains("users/" + name);
}

bool webAuth::setPassword(const QString& name, const QString& password)
{
    if (!validUserName(name) || password.isEmpty())
        return false;
    auto s = openStore();
    s->setValue("users/" + name, hashPassword(password));
    s->sync();
    endUserSessions(name);   // a new password signs everyone out
    return s->status() == QSettings::NoError;
}

bool webAuth::removeUser(const QString& name)
{
    if (!userExists(name))
        return false;
    auto s = openStore();
    s->remove("users/" + name);
    s->sync();
    endUserSessions(name);
    return true;
}

bool webAuth::checkPassword(const QString& name, const QString& password)
{
    if (!validUserName(name))
        return false;
    const QString stored = openStore()->value("users/" + name).toString();
    if (stored.isEmpty()) {
        // Same work for an unknown name, so timing does not reveal names.
        verifyHash(password, hashPassword(QStringLiteral("x")));
        return false;
    }
    return verifyHash(password, stored);
}

QString webAuth::hashPassword(const QString& password, int iterations)
{
    const QByteArray salt = randomBytes(16);
    const QByteArray key = QPasswordDigestor::deriveKeyPbkdf2(QCryptographicHash::Sha256,
                                                              password.toUtf8(), salt, iterations, 32);
    return QString("pbkdf2-sha256$%1$%2$%3").arg(iterations)
        .arg(QString::fromLatin1(salt.toBase64()), QString::fromLatin1(key.toBase64()));
}

bool webAuth::verifyHash(const QString& password, const QString& stored)
{
    const QStringList p = stored.split('$');
    if (p.size() != 4 || p[0] != "pbkdf2-sha256")
        return false;
    bool ok = false;
    const int iterations = p[1].toInt(&ok);
    if (!ok || iterations < 1000 || iterations > 10000000)
        return false;
    const QByteArray salt = QByteArray::fromBase64(p[2].toLatin1());
    const QByteArray want = QByteArray::fromBase64(p[3].toLatin1());
    const QByteArray got = QPasswordDigestor::deriveKeyPbkdf2(QCryptographicHash::Sha256,
                                                              password.toUtf8(), salt, iterations, want.size());
    return !want.isEmpty() && sameBytes(got, want);
}

// ─── sessions ──────────────────────────────────────────────────────────────

QString webAuth::createSession(const QString& user, bool remember)
{
    const QString token = QString::fromLatin1(randomBytes(32).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    auto s = openStore();
    // Drop expired sessions while we are here.
    s->beginGroup("sessions");
    for (const QString& k : s->childKeys()) {
        const QStringList v = s->value(k).toString().split('|');
        if (v.size() < 2 || v[1].toLongLong() < now)
            s->remove(k);
    }
    s->endGroup();
    s->setValue(tokenKey(token), QString("%1|%2").arg(user).arg(now + (remember ? kSessionLongSecs : kSessionShortSecs)));
    s->sync();
    return token;
}

QString webAuth::sessionUser(const QString& token)
{
    if (token.isEmpty() || token.size() > 128)
        return QString();
    auto s = openStore();
    const QStringList v = s->value(tokenKey(token)).toString().split('|');
    if (v.size() < 2 || v[1].toLongLong() < QDateTime::currentSecsSinceEpoch())
        return QString();
    if (!s->contains("users/" + v[0]))
        return QString();
    return v[0];
}

void webAuth::endSession(const QString& token)
{
    if (token.isEmpty())
        return;
    auto s = openStore();
    s->remove(tokenKey(token));
    s->sync();
}

void webAuth::endUserSessions(const QString& user)
{
    auto s = openStore();
    s->beginGroup("sessions");
    for (const QString& k : s->childKeys())
        if (s->value(k).toString().section('|', 0, 0) == user)
            s->remove(k);
    s->endGroup();
    s->sync();
}

QString webAuth::cookieToken(const QByteArray& headers)
{
    // Any "Cookie:" header line, any position in it.
    for (const QByteArray& line : headers.split('\n')) {
        const QByteArray l = line.trimmed();
        if (!l.toLower().startsWith("cookie:"))
            continue;
        for (const QByteArray& part : l.mid(7).split(';')) {
            const QByteArray kv = part.trimmed();
            const int eq = kv.indexOf('=');
            if (eq > 0 && kv.left(eq) == kCookie)
                return QString::fromLatin1(kv.mid(eq + 1));
        }
    }
    return QString();
}

QByteArray webAuth::setCookieHeader(const QString& token, bool remember)
{
    QByteArray h = QByteArray("Set-Cookie: ") + kCookie + "=" + token.toLatin1()
                 + "; Path=/; HttpOnly; Secure; SameSite=Strict";
    if (remember)
        h += "; Max-Age=" + QByteArray::number(kSessionLongSecs);
    return h;
}

QByteArray webAuth::clearCookieHeader()
{
    return QByteArray("Set-Cookie: ") + kCookie + "=; Path=/; HttpOnly; Secure; SameSite=Strict; Max-Age=0";
}

// ─── lockout ───────────────────────────────────────────────────────────────

bool webAuth::lockedOut(const QString& address, int* secondsLeft)
{
    auto it = failed.find(address);
    if (it == failed.end())
        return false;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    if (it->lockedUntil.isValid() && it->lockedUntil > now) {
        if (secondsLeft)
            *secondsLeft = int(now.secsTo(it->lockedUntil));
        return true;
    }
    if (it->lockedUntil.isValid() || it->first.secsTo(now) > kWindowSecs)
        failed.erase(it);
    return false;
}

void webAuth::recordFailure(const QString& address)
{
    const QDateTime now = QDateTime::currentDateTimeUtc();
    failures& f = failed[address];
    if (!f.first.isValid() || f.first.secsTo(now) > kWindowSecs) {
        f.first = now;
        f.count = 0;
    }
    if (++f.count >= kMaxFailures)
        f.lockedUntil = now.addSecs(kWindowSecs);
    // Keep the table bounded against a flood of addresses.
    if (failed.size() > 10000)
        failed.clear();
}

void webAuth::recordSuccess(const QString& address)
{
    failed.remove(address);
}
