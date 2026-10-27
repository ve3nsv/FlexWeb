#pragma once

// Web page login: named users with PBKDF2 password hashes, sessions behind
// a cookie, and a per-address lockout after repeated wrong passwords.
//
// Users live in QSettings("wfweb", "webusers") under users/<name> as
// "pbkdf2-sha256$<iterations>$<salt b64>$<hash b64>"; the password itself is
// never stored. Sessions are stored by the SHA-256 of their token (so the
// settings file cannot be replayed as a cookie) and survive a restart.
// Login is on as soon as one user exists.
//
// Pure Qt Core + Network (QPasswordDigestor), no QObject: the web server
// calls it from its thread, the CLI from main(), tests/webauth_test.cpp links
// it alone.

#include <QDateTime>
#include <QHash>
#include <QString>
#include <QStringList>

class webAuth
{
public:
    // ── users ──
    static bool hasUsers();
    static QStringList users();
    static bool userExists(const QString& name);
    static bool validUserName(const QString& name);
    // Adds or replaces. False for an invalid name or an empty password.
    static bool setPassword(const QString& name, const QString& password);
    static bool removeUser(const QString& name);       // also ends its sessions
    // Slow on purpose (PBKDF2): call it off the web thread.
    static bool checkPassword(const QString& name, const QString& password);

    static QString hashPassword(const QString& password, int iterations = kIterations);
    static bool verifyHash(const QString& password, const QString& stored);

    // ── sessions ──
    // remember: 30 days, else 12 hours. Returns the cookie token.
    static QString createSession(const QString& user, bool remember);
    // The user the token belongs to, or "" (unknown, expired, user removed).
    static QString sessionUser(const QString& token);
    static void endSession(const QString& token);
    static void endUserSessions(const QString& user);
    static QString cookieToken(const QByteArray& headers);   // wfweb_session=... from a Cookie header
    static QByteArray setCookieHeader(const QString& token, bool remember);
    static QByteArray clearCookieHeader();

    // ── lockout (per remote address, in memory) ──
    // 5 wrong passwords within 15 minutes lock the address for 15 minutes.
    bool lockedOut(const QString& address, int* secondsLeft = nullptr);
    void recordFailure(const QString& address);
    void recordSuccess(const QString& address);

    static constexpr int kIterations = 60000;
    static constexpr int kMaxFailures = 5;
    static constexpr int kWindowSecs = 15 * 60;
    static constexpr const char* kCookie = "wfweb_session";
    // The guest's session user (the login page's "View only" button). Not a
    // valid user name, so it can never be a real account; the web server
    // accepts it only while guest view is on.
    static constexpr const char* kGuest = "*guest";

private:
    struct failures { int count = 0; QDateTime first; QDateTime lockedUntil; };
    QHash<QString, failures> failed;
};
