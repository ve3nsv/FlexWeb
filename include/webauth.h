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
#include <QJsonArray>
#include <QJsonObject>
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

    // ── roles (punch list 41) ──
    // admin (the station owner; users from before roles have no entry and
    // are admins), operator (full access), listener (receive only, tunes
    // while no operator is on), suspended (cannot sign in).
    static QString role(const QString& name);
    static bool setRole(const QString& name, const QString& role);   // ends a suspended user's sessions
    static bool validRole(const QString& role);
    static int adminCount();

    // ── access requests (punch list 41) ──
    // Kept under requests/<CALL> as JSON: name, email, note, time, address,
    // lookup, and the PBKDF2 hash of the password chosen (never the password).
    static bool validCallsign(const QString& call);
    // Slow (PBKDF2). False with *error set: bad callsign, taken, already
    // requested, too many pending, empty password.
    static bool addRequest(const QString& call, const QJsonObject& info, const QString& password, QString* error);
    static QJsonArray requests();                        // oldest first, without the hash
    static QJsonObject request(const QString& call);     // without the hash; empty if none
    static int requestCount();
    static void setRequestLookup(const QString& call, const QJsonObject& lookup);
    // Approve: the request becomes user <CALL> with its password and the role.
    static bool approveRequest(const QString& call, const QString& role);
    static bool rejectRequest(const QString& call);
    // Slow: the password matches a pending request (sign-in says "waiting").
    static bool checkRequestPassword(const QString& call, const QString& password);
    static constexpr int kMaxPendingRequests = 50;

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
