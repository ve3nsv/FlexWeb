#ifndef LANHOST_H
#define LANHOST_H
// The LAN rig address as typed into a launcher field, a settings file or
// --lan: an address or host name only. A pasted "--lan host", a URL
// ("http://host/"), a trailing slash or spaces made it an "Invalid
// hostname" with no hint why (VE3NSV's RS-BA1 server, 2026-11-03). The
// SmartLink ("smartlink:<radio>") and Flex discovery ("auto") forms pass
// through. A ":port" on a name or IPv4 address is cut off and returned in
// *port (the ports are their own settings); IPv6 literals are kept whole.
#include <QString>
#include <QStringList>

inline QString cleanLanHost(const QString& in, int* port = nullptr, QStringList* changes = nullptr)
{
    QString h = in.trimmed();
    if (port) *port = 0;
    if (h.startsWith(QLatin1String("smartlink:"), Qt::CaseInsensitive)) return h;
    auto note = [&](const char* what) { if (changes) changes->append(QString::fromLatin1(what)); };
    if (h.startsWith(QLatin1String("--lan="))) { h = h.mid(6).trimmed(); note("--lan="); }
    else if (h.startsWith(QLatin1String("--lan ")) || h.startsWith(QLatin1String("--lan\t"))) { h = h.mid(6).trimmed(); note("--lan"); }
    const int scheme = h.indexOf(QLatin1String("://"));
    if (scheme > 0 && scheme < 10) { h = h.mid(scheme + 3); note("scheme"); }
    if (h.startsWith(QLatin1Char('['))) {                 // [v6] or [v6]:port
        const int close = h.indexOf(QLatin1Char(']'));
        if (close > 0) {
            const QString rest = h.mid(close + 1);
            if (rest.startsWith(QLatin1Char(':'))) {
                bool ok = false;
                const int p = rest.mid(1).section(QLatin1Char('/'), 0, 0).toInt(&ok);
                if (ok && port) *port = p;
                note("port");
            }
            if (!rest.isEmpty() && !rest.startsWith(QLatin1Char(':'))) note("path");
            return h.mid(1, close - 1);
        }
    }
    const int slash = h.indexOf(QLatin1Char('/'));
    if (slash >= 0) { h = h.left(slash); note("path"); }
    if (h.count(QLatin1Char(':')) == 1) {                 // host:port (not IPv6)
        bool ok = false;
        const int p = h.section(QLatin1Char(':'), 1).toInt(&ok);
        if (ok) { if (port) *port = p; h = h.section(QLatin1Char(':'), 0, 0); note("port"); }
    }
    if (h.contains(QLatin1Char(' '))) { h = h.section(QLatin1Char(' '), 0, 0); note("spaces"); }
    return h;
}

#endif
