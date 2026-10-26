// Unit test for src/webauth.cpp (tests/test_cpp_units.py). Runs with HOME
// pointed at a temporary directory, so QSettings stays out of real settings.

#include "webauth.h"
#include "guestpolicy.h"

#include <QCoreApplication>
#include <cstdio>
#include <cstdlib>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    // Hash format and verification; PBKDF2 itself matches Python's
    // hashlib.pbkdf2_hmac('sha256', b'pw', b'saltsaltsalt', 100000).
    const QString h = webAuth::hashPassword("correct horse", 2000);
    CHECK(h.startsWith("pbkdf2-sha256$2000$"));
    CHECK(webAuth::verifyHash("correct horse", h));
    CHECK(!webAuth::verifyHash("correct hors", h));
    CHECK(!webAuth::verifyHash("correct horse", "plain"));
    CHECK(!webAuth::verifyHash("x", "pbkdf2-sha256$10$AAAA$AAAA"));   // too few iterations
    CHECK(webAuth::hashPassword("same", 2000) != webAuth::hashPassword("same", 2000)); // salted
    CHECK(webAuth::verifyHash("pw", "pbkdf2-sha256$100000$c2FsdHNhbHRzYWx0$A9eEiy16r5F4e434mIYn+TXXnDlp16BcB2jQ51oEDIs="));

    // Users.
    CHECK(!webAuth::hasUsers());
    CHECK(!webAuth::setPassword("bad name", "x"));
    CHECK(!webAuth::setPassword("tom", ""));
    CHECK(webAuth::setPassword("tom", "s3cret"));
    CHECK(webAuth::setPassword("ve3nsv", "other"));
    CHECK(webAuth::hasUsers());
    CHECK(webAuth::users() == QStringList({"tom", "ve3nsv"}));
    CHECK(webAuth::checkPassword("tom", "s3cret"));
    CHECK(!webAuth::checkPassword("tom", "other"));
    CHECK(!webAuth::checkPassword("nobody", "s3cret"));

    // Sessions.
    const QString t = webAuth::createSession("tom", true);
    CHECK(t.size() >= 40);
    CHECK(webAuth::sessionUser(t) == "tom");
    CHECK(webAuth::sessionUser(t + "x").isEmpty());
    CHECK(webAuth::sessionUser("").isEmpty());
    const QString t2 = webAuth::createSession("tom", false);
    webAuth::endSession(t2);
    CHECK(webAuth::sessionUser(t2).isEmpty());
    CHECK(webAuth::sessionUser(t) == "tom");
    // A new password ends the user's sessions; removing the user too.
    const QString t3 = webAuth::createSession("ve3nsv", false);
    CHECK(webAuth::setPassword("tom", "new"));
    CHECK(webAuth::sessionUser(t).isEmpty());
    CHECK(webAuth::sessionUser(t3) == "ve3nsv");
    CHECK(webAuth::removeUser("ve3nsv"));
    CHECK(webAuth::sessionUser(t3).isEmpty());
    CHECK(!webAuth::removeUser("ve3nsv"));

    CHECK(!webAuth::validUserName(webAuth::kGuestUser));
    const auto guest = webAuth::createGuestSession();
    CHECK(webAuth::sessionUser(guest) == webAuth::kGuestUser);
    CHECK(guestCommandAllowed("enableAudio"));
    CHECK(guestCommandAllowed("getStatus"));
    for (const auto &command : {"setPTT", "ampTune", "ampOperate", "setFrequency", "setSstvSettings", "futureUnknownCommand", "enableMic"})
        CHECK(!guestCommandAllowed(command));
    webAuth::endGuestSessions();
    CHECK(webAuth::sessionUser(guest).isEmpty());

    // Cookie header parsing.
    CHECK(webAuth::cookieToken("GET / HTTP/1.1\r\nHost: x\r\nCookie: a=1; wfweb_session=abc-_9; b=2\r\n") == "abc-_9");
    CHECK(webAuth::cookieToken("GET / HTTP/1.1\r\ncookie: wfweb_session=Z\r\n") == "Z");
    CHECK(webAuth::cookieToken("GET / HTTP/1.1\r\nCookie: xwfweb_session=Z\r\n").isEmpty());
    CHECK(webAuth::setCookieHeader("T", true).contains("Max-Age="));
    CHECK(!webAuth::setCookieHeader("T", false).contains("Max-Age="));
    CHECK(webAuth::setCookieHeader("T", false).contains("HttpOnly; Secure; SameSite=Strict"));

    // Lockout after 5 failures, per address.
    webAuth a;
    for (int i = 0; i < 4; ++i) a.recordFailure("1.2.3.4");
    CHECK(!a.lockedOut("1.2.3.4"));
    a.recordFailure("1.2.3.4");
    int left = 0;
    CHECK(a.lockedOut("1.2.3.4", &left));
    CHECK(left > 800);
    CHECK(!a.lockedOut("5.6.7.8"));
    a.recordFailure("5.6.7.8");
    a.recordSuccess("5.6.7.8");
    for (int i = 0; i < 4; ++i) a.recordFailure("5.6.7.8");
    CHECK(!a.lockedOut("5.6.7.8"));

    if (failures) return 1;
    std::printf("webauth: all tests passed\n");
    return 0;
}
