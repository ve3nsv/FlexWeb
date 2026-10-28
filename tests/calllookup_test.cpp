// Unit test for src/calllookup.cpp's parsers (tests/test_cpp_units.py):
// the zip reader on a saved sample of ISED's list format (fictitious
// entries), the ISED line parser, and HamQTH / QRZ XML answers.

#include "calllookupparse.h"

#include <QCoreApplication>
#include <QFile>
#include <cstdio>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    CHECK(callLookupParse::isCanadian("VE3NSV"));
    CHECK(callLookupParse::isCanadian("va3zzz"));
    CHECK(callLookupParse::isCanadian("VY1AA"));
    CHECK(callLookupParse::isCanadian("CY0X"));
    CHECK(!callLookupParse::isCanadian("K8CHY"));
    CHECK(!callLookupParse::isCanadian("VK5KVA"));
    CHECK(!callLookupParse::isCanadian("VEX1"));

    QFile f("tests/data/ised_sample.zip");
    CHECK(f.open(QIODevice::ReadOnly));
    const QByteArray zip = f.readAll();
    QString err;
    const QByteArray txt = callLookupParse::zipEntry(zip, ".txt", &err);
    CHECK(err.isEmpty());
    CHECK(txt.startsWith("call_sign;first_name;surname"));
    CHECK(txt.count('\n') == 601);
    CHECK(callLookupParse::zipEntry(zip, ".csv", &err).isEmpty() && !err.isEmpty());
    CHECK(callLookupParse::zipEntry("not a zip at all, not a zip at all", ".txt", &err).isEmpty());
    QByteArray bad = zip;
    bad[60] = char(bad[60] ^ 0x55);   // inside the deflated data (after the 47-byte local header)
    CHECK(callLookupParse::zipEntry(bad, ".txt").isEmpty());

    QJsonObject a = callLookupParse::findIsed(txt, "va3zzz");
    CHECK(a["found"].toBool());
    CHECK(a["name"].toString() == "Jane Example");
    CHECK(a["city"].toString() == "Ottawa");
    CHECK(a["region"].toString() == "ON");
    CHECK(a["qual"].toString() == "Basic");
    CHECK(!a["hf"].toBool());
    QJsonObject b = callLookupParse::findIsed(txt, "VE7QQQ");
    CHECK(b["qual"].toString() == "Basic with Honours, Advanced, 5 wpm");
    CHECK(b["hf"].toBool());
    QJsonObject c = callLookupParse::findIsed(txt, "VE1XXX");
    CHECK(c["found"].toBool() && c["name"].toString() == "TEST CLUB");
    CHECK(!callLookupParse::findIsed(txt, "VE9ABC")["found"].toBool());
    CHECK(!callLookupParse::findIsed(txt, "VA3ZZ")["found"].toBool());   // no prefix match
    // No header line: the default column order.
    QJsonObject d = callLookupParse::parseIsedLine(QByteArray(), "VE3AAA;ANN;O'NEIL;X;SAULT STE. MARIE;ON;P6A0A1;A;;;;E;");
    CHECK(d["name"].toString() == "Ann O'neil" || d["name"].toString() == "Ann O'Neil");
    CHECK(d["qual"].toString() == "Basic with Honours" && d["hf"].toBool());

    const QByteArray hq = R"(<?xml version="1.0"?>
<HamQTH version="2.8" xmlns="https://www.hamqth.com"><search><callsign>k0abc</callsign><nick>Bob</nick>
<adr_name>Bob Example</adr_name><adr_city>Denver</adr_city><us_state>CO</us_state><country>United States</country></search></HamQTH>)";
    QJsonObject h = callLookupParse::parseHamQth(hq);
    CHECK(h["found"].toBool() && h["call"].toString() == "K0ABC");
    CHECK(h["name"].toString() == "Bob Example" && h["city"].toString() == "Denver" && h["region"].toString() == "CO");
    CHECK(!callLookupParse::parseHamQth("<HamQTH><session><error>Callsign not found</error></session></HamQTH>")["found"].toBool());
    CHECK(callLookupParse::xmlValue("<HamQTH><session><session_id>abc123</session_id></session></HamQTH>", "session_id") == "abc123");

    const QByteArray qz = R"(<?xml version="1.0" ?>
<QRZDatabase version="1.34" xmlns="http://xmldata.qrz.com"><Callsign><call>W1XYZ</call><fname>Pat</fname>
<name>Sample</name><addr2>Boston</addr2><state>MA</state><country>United States</country><class>E</class></Callsign>
<Session><Key>k</Key></Session></QRZDatabase>)";
    QJsonObject q = callLookupParse::parseQrz(qz);
    CHECK(q["found"].toBool() && q["name"].toString() == "Pat Sample" && q["class"].toString() == "E");
    QJsonObject qn = callLookupParse::parseQrz("<QRZDatabase><Session><Error>Not found: W1NONE</Error><Key>k</Key></Session></QRZDatabase>");
    CHECK(!qn["found"].toBool() && qn["error"].toString().startsWith("Not found"));

    if (failures) return 1;
    std::printf("calllookup: all tests passed\n");
    return 0;
}
