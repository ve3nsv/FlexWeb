#pragma once

// Callsign lookup parsers (punch list 41c), apart from the network side so
// tests/calllookup_test.cpp can link them without moc. See calllookup.h.

#include <QByteArray>
#include <QJsonObject>
#include <QString>

struct callLookupParse
{
    static bool isCanadian(const QString& call);
    // One file out of a .zip (stored or deflated). Empty with *error set on failure.
    static QByteArray zipEntry(const QByteArray& zip, const QString& nameSuffix, QString* error = nullptr);
    // A line of amateur_delim.txt ("call_sign;first_name;..." header or none).
    static QJsonObject parseIsedLine(const QByteArray& header, const QByteArray& line);
    // Scans the whole file text for the callsign.
    static QJsonObject findIsed(const QByteArray& file, const QString& call);
    static QJsonObject parseHamQth(const QByteArray& xml);
    static QJsonObject parseQrz(const QByteArray& xml);
    static QString xmlValue(const QByteArray& xml, const QString& element);
};
