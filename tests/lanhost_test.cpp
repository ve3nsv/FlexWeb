// cleanLanHost (include/lanhost.h): the LAN address as people type it.
#include "lanhost.h"
#include <cstdio>

static int failures = 0;
static void expect(const char* in, const char* host, int port = 0)
{
    int p = -1;
    const QString got = cleanLanHost(QString::fromUtf8(in), &p);
    if (got != QString::fromUtf8(host) || p != port) {
        std::printf("FAIL '%s' -> '%s' port %d (want '%s' port %d)\n", in, qPrintable(got), p, host, port);
        ++failures;
    }
}

int main()
{
    expect("mancave1.hopto.org", "mancave1.hopto.org");
    expect("  mancave1.hopto.org  ", "mancave1.hopto.org");
    expect("--lan mancave1.hopto.org", "mancave1.hopto.org");
    expect("--lan=mancave1.hopto.org", "mancave1.hopto.org");
    expect("http://mancave1.hopto.org/", "mancave1.hopto.org");
    expect("https://mancave1.hopto.org", "mancave1.hopto.org");
    expect("mancave1.hopto.org/", "mancave1.hopto.org");
    expect("mancave1.hopto.org:50001", "mancave1.hopto.org", 50001);
    expect("http://142.117.83.2:50001/", "142.117.83.2", 50001);
    expect("142.117.83.2", "142.117.83.2");
    expect("192.168.1.20 ", "192.168.1.20");
    expect("fe80::1", "fe80::1");
    expect("[fe80::1]:50001", "fe80::1", 50001);
    expect("auto", "auto");
    expect("smartlink:My Radio", "smartlink:My Radio");     // a radio name may have spaces
    QStringList changes;
    cleanLanHost(QStringLiteral("http://x.org/"), nullptr, &changes);
    if (changes.size() != 2) { std::printf("FAIL changes %s\n", qPrintable(changes.join(','))); ++failures; }
    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
