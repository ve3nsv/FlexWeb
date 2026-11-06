#ifndef RADIOPROFILE_H
#define RADIOPROFILE_H
// Punch list 73: one server, several radios, one at a time, chosen on the
// page. Each radio is a --radio profile the launcher composes:
//
//   --radio "name=Flex 6400;type=flex;host=auto;amp=10.0.0.25"
//   --radio "name=IC-7300;type=icomlan;host=100.74.78.87;user=me;pass=...;radio=7300;buffer=300"
//
// type: flex (FlexRadio on the LAN; host = IP or "auto"), smartlink (host =
// the radio's name or serial, or "auto"), icomlan (an Icom LAN radio or an
// RS-BA1 / wfview server; radio = which one on a server with several).
// A ';' or '\' inside a value is written "\;" / "\\". Unknown keys are
// ignored, so an older server takes a newer launcher's profiles.
//
// Qt5Core only, header-only: tests/radioprofile_test.cpp.
#include <QString>
#include <QStringList>

struct radioProfile {
    QString name;
    QString type;     // flex | smartlink | icomlan
    QString host;
    QString user;
    QString pass;
    QString radio;    // icomlan: --lan-radio
    QString amp;      // RF-KIT host[:port]; empty = no amplifier with this radio
    int bufferMs = 0; // icomlan: --lan-buffer (0 = the server's default)
    int controlPort = 0;  // --lan-control (0 = the type's default: Icom 50001, Flex 4992)
    int audioPort = 0;    // --lan-audio (0 = default: Icom 50003, Flex 4991)

    bool isFlex() const { return type == QLatin1String("flex") || type == QLatin1String("smartlink"); }
    bool valid() const
    {
        return !name.isEmpty()
            && (type == QLatin1String("flex") || type == QLatin1String("smartlink") || type == QLatin1String("icomlan"))
            && (type != QLatin1String("icomlan") || !host.isEmpty());
    }

    // "k=v;k=v" -> profile. *error says why one is refused.
    static radioProfile parse(const QString &spec, QString *error = nullptr)
    {
        radioProfile p;
        QStringList fields;
        QString cur;
        for (int i = 0; i < spec.size(); ++i) {
            const QChar c = spec.at(i);
            if (c == QLatin1Char('\\') && i + 1 < spec.size()) { cur += spec.at(++i); continue; }
            if (c == QLatin1Char(';')) { fields << cur; cur.clear(); continue; }
            cur += c;
        }
        fields << cur;
        for (const QString &f : fields) {
            const int eq = f.indexOf(QLatin1Char('='));
            if (eq <= 0) continue;
            const QString k = f.left(eq).trimmed().toLower();
            const QString v = f.mid(eq + 1);
            if (k == QLatin1String("name")) p.name = v.trimmed();
            else if (k == QLatin1String("type")) p.type = v.trimmed().toLower();
            else if (k == QLatin1String("host")) p.host = v.trimmed();
            else if (k == QLatin1String("user")) p.user = v;
            else if (k == QLatin1String("pass")) p.pass = v;
            else if (k == QLatin1String("radio")) p.radio = v.trimmed();
            else if (k == QLatin1String("amp")) p.amp = v.trimmed();
            else if (k == QLatin1String("buffer")) p.bufferMs = v.trimmed().toInt();
            else if (k == QLatin1String("port")) p.controlPort = v.trimmed().toInt();
            else if (k == QLatin1String("audioport")) p.audioPort = v.trimmed().toInt();
        }
        if (p.isFlex() && p.host.isEmpty()) p.host = QStringLiteral("auto");
        if (error) {
            if (p.name.isEmpty()) *error = QStringLiteral("no name");
            else if (!p.valid() && p.type != QLatin1String("icomlan")) *error = QStringLiteral("type must be flex, smartlink or icomlan");
            else if (!p.valid()) *error = QStringLiteral("an icomlan radio needs a host");
            else error->clear();
        }
        return p;
    }

    // The inverse, for the launcher (values escaped).
    QString toSpec() const
    {
        auto esc = [](QString v) { return v.replace(QLatin1String("\\"), QLatin1String("\\\\")).replace(QLatin1String(";"), QLatin1String("\\;")); };
        QStringList out;
        out << QStringLiteral("name=") + esc(name) << QStringLiteral("type=") + esc(type);
        if (!host.isEmpty()) out << QStringLiteral("host=") + esc(host);
        if (!user.isEmpty()) out << QStringLiteral("user=") + esc(user);
        if (!pass.isEmpty()) out << QStringLiteral("pass=") + esc(pass);
        if (!radio.isEmpty()) out << QStringLiteral("radio=") + esc(radio);
        if (!amp.isEmpty()) out << QStringLiteral("amp=") + esc(amp);
        if (bufferMs > 0) out << QStringLiteral("buffer=") + QString::number(bufferMs);
        if (controlPort > 0) out << QStringLiteral("port=") + QString::number(controlPort);
        if (audioPort > 0) out << QStringLiteral("audioport=") + QString::number(audioPort);
        return out.join(QLatin1Char(';'));
    }
};

#endif
