// Callsign lookup parsers. See include/calllookupparse.h.

#include "calllookupparse.h"

#include <QRegularExpression>
#include <QStringList>
#include <QXmlStreamReader>

extern "C" {
#include "thirdparty/tinf/tinf.h"
}

namespace {

quint32 le32(const QByteArray& b, int at)
{
    if (at < 0 || at + 4 > b.size()) return 0;
    const uchar* p = reinterpret_cast<const uchar*>(b.constData()) + at;
    return quint32(p[0]) | (quint32(p[1]) << 8) | (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
}

quint16 le16(const QByteArray& b, int at)
{
    if (at < 0 || at + 2 > b.size()) return 0;
    const uchar* p = reinterpret_cast<const uchar*>(b.constData()) + at;
    return quint16(p[0] | (p[1] << 8));
}

// ISED's file may be Latin-1; take UTF-8 only when it decodes cleanly.
QString text(const QByteArray& b)
{
    const QString u = QString::fromUtf8(b);
    return u.contains(QChar::ReplacementCharacter) ? QString::fromLatin1(b) : u;
}

QString titleCase(const QString& s)
{
    QString out = s.toLower();
    bool start = true;
    for (QChar& c : out) {
        if (start && c.isLetter()) c = c.toUpper();
        start = !c.isLetter() && c != QLatin1Char('\'');
    }
    return out;
}

} // namespace

bool callLookupParse::isCanadian(const QString& call)
{
    // VA-VG, VO, VX, VY, CF-CK, CY-CZ, XJ-XO, followed by a digit.
    static const QRegularExpression re("^(V[A-GOXY]|C[F-KYZ]|X[J-O])[0-9]");
    return re.match(call.toUpper()).hasMatch();
}

QByteArray callLookupParse::zipEntry(const QByteArray& zip, const QString& nameSuffix, QString* error)
{
    auto fail = [error](const QString& e) { if (error) *error = e; return QByteArray(); };
    // End of central directory: the last "PK\5\6" within the trailing 64 kB.
    int eocd = -1;
    for (int i = zip.size() - 22; i >= 0 && i >= zip.size() - 22 - 65535; --i)
        if (le32(zip, i) == 0x06054b50u) { eocd = i; break; }
    if (eocd < 0) return fail("not a zip file");
    const int entries = le16(zip, eocd + 10);
    int p = int(le32(zip, eocd + 16));
    for (int n = 0; n < entries; ++n) {
        if (le32(zip, p) != 0x02014b50u) return fail("bad zip directory");
        const int method = le16(zip, p + 10);
        const quint32 crc = le32(zip, p + 16);
        const quint32 csize = le32(zip, p + 20);
        const quint32 usize = le32(zip, p + 24);
        const int nlen = le16(zip, p + 28), xlen = le16(zip, p + 30), clen = le16(zip, p + 32);
        const quint32 local = le32(zip, p + 42);
        const QString name = QString::fromUtf8(zip.mid(p + 46, nlen));
        p += 46 + nlen + xlen + clen;
        if (!name.endsWith(nameSuffix, Qt::CaseInsensitive)) continue;
        if (le32(zip, int(local)) != 0x04034b50u) return fail("bad zip entry");
        const int data = int(local) + 30 + le16(zip, int(local) + 26) + le16(zip, int(local) + 28);
        if (data < 0 || qint64(data) + csize > zip.size()) return fail("truncated zip");
        if (usize > 512u * 1024 * 1024) return fail("zip entry too large");
        QByteArray out;
        if (method == 0) {
            out = zip.mid(data, int(csize));
        } else if (method == 8) {
            out.resize(int(usize));
            unsigned int len = usize;
            if (tinf_uncompress(out.data(), &len, zip.constData() + data, csize) != TINF_OK || len != usize)
                return fail("could not inflate " + name);
        } else {
            return fail(QString("zip method %1 not supported").arg(method));
        }
        if (tinf_crc32(out.constData(), unsigned(out.size())) != crc) return fail("zip CRC mismatch");
        return out;
    }
    return fail("no " + nameSuffix + " in the zip");
}

QJsonObject callLookupParse::parseIsedLine(const QByteArray& header, const QByteArray& line)
{
    // The column order used by ISED's delimited list; a header line, when
    // the file has one, overrides it.
    static const QStringList defaults = {"call_sign", "first_name", "surname", "address_line", "city",
                                         "prov_cd", "postal_code", "qual_a", "qual_b", "qual_c", "qual_d",
                                         "qual_e", "club_name"};
    QStringList cols = defaults;
    if (header.toLower().contains("call_sign")) {
        cols.clear();
        for (const QByteArray& h : header.trimmed().split(';')) cols << QString::fromLatin1(h.trimmed()).toLower();
    }
    const QList<QByteArray> f = line.trimmed().split(';');
    auto col = [&](const char* name) {
        const int i = cols.indexOf(QLatin1String(name));
        return i >= 0 && i < f.size() ? text(f.at(i)).trimmed() : QString();
    };
    QJsonObject o;
    o["source"] = "ISED";
    o["call"] = col("call_sign").toUpper();
    o["found"] = !col("call_sign").isEmpty();
    QString name = titleCase((col("first_name") + ' ' + col("surname")).trimmed());
    if (name.isEmpty()) name = col("club_name");
    o["name"] = name;
    o["city"] = titleCase(col("city"));
    o["region"] = col("prov_cd").toUpper();
    o["country"] = "Canada";
    const bool basic = !col("qual_a").isEmpty(), wpm5 = !col("qual_b").isEmpty(), wpm12 = !col("qual_c").isEmpty(),
               adv = !col("qual_d").isEmpty(), honours = !col("qual_e").isEmpty();
    QStringList q;
    if (basic) q << (honours ? "Basic with Honours" : "Basic");
    else if (honours) q << "Basic with Honours";
    if (adv) q << "Advanced";
    if (wpm12) q << "12 wpm";
    else if (wpm5) q << "5 wpm";
    o["qual"] = q.join(", ");
    // Below 30 MHz: Basic with Honours, Morse (5 / 12 wpm) or Advanced.
    o["hf"] = honours || wpm5 || wpm12 || adv;
    return o;
}

QJsonObject callLookupParse::findIsed(const QByteArray& file, const QString& callIn)
{
    const QByteArray call = callIn.toUpper().toLatin1();
    int nl = file.indexOf('\n');
    const QByteArray first = file.left(nl < 0 ? file.size() : nl);
    const QByteArray header = first.toLower().contains("call_sign") ? first : QByteArray();
    int pos = 0;
    while (pos < file.size()) {
        int end = file.indexOf('\n', pos);
        if (end < 0) end = file.size();
        const int semi = file.indexOf(';', pos);
        if (semi > pos && semi < end && file.mid(pos, semi - pos).trimmed().toUpper() == call)
            return parseIsedLine(header, file.mid(pos, end - pos));
        pos = end + 1;
    }
    QJsonObject o;
    o["source"] = "ISED";
    o["call"] = callIn.toUpper();
    o["found"] = false;
    return o;
}

QString callLookupParse::xmlValue(const QByteArray& xml, const QString& element)
{
    QXmlStreamReader r(xml);
    while (!r.atEnd()) {
        if (r.readNext() == QXmlStreamReader::StartElement
            && r.name().compare(element, Qt::CaseInsensitive) == 0)
            return r.readElementText(QXmlStreamReader::IncludeChildElements).trimmed();
    }
    return QString();
}

QJsonObject callLookupParse::parseHamQth(const QByteArray& xml)
{
    QJsonObject o;
    o["source"] = "HamQTH";
    const QString err = xmlValue(xml, "error");
    if (!err.isEmpty()) { o["found"] = false; o["error"] = err; return o; }
    o["call"] = xmlValue(xml, "callsign").toUpper();
    o["found"] = !o["call"].toString().isEmpty();
    QString name = xmlValue(xml, "adr_name");
    if (name.isEmpty()) name = xmlValue(xml, "nick");
    o["name"] = name;
    o["city"] = xmlValue(xml, "adr_city");
    o["region"] = xmlValue(xml, "us_state");
    o["country"] = xmlValue(xml, "country");
    return o;
}

QJsonObject callLookupParse::parseQrz(const QByteArray& xml)
{
    QJsonObject o;
    o["source"] = "QRZ";
    const QString err = xmlValue(xml, "Error");
    o["call"] = xmlValue(xml, "call").toUpper();
    o["found"] = !o["call"].toString().isEmpty();
    if (!o["found"].toBool()) { o["error"] = err.isEmpty() ? QStringLiteral("not found") : err; return o; }
    o["name"] = (xmlValue(xml, "fname") + ' ' + xmlValue(xml, "name")).trimmed();
    o["city"] = xmlValue(xml, "addr2");
    o["region"] = xmlValue(xml, "state");
    o["country"] = xmlValue(xml, "country");
    o["class"] = xmlValue(xml, "class");
    return o;
}

