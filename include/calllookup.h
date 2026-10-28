#pragma once

// Callsign lookup for access requests (punch list 41c). It only informs the
// station's admin about who asked; it never approves anyone.
//
// Canadian calls: ISED's public amateur list (amateur_delim.zip, refreshed
// weekly into the data directory): name, city, province, qualifications.
// Other calls (or a Canadian call ISED does not list): QRZ XML when the admin
// has a subscription, else HamQTH (free account), when configured.
//
// The result is a JSON object: source ("ISED", "QRZ", "HamQTH" or ""),
// found, name, city, region, country, qual (text), hf (ISED: the
// qualifications allow HF), class (QRZ licence class), error.

#include "calllookupparse.h"

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QStringList>

#include <functional>

class QNetworkAccessManager;

class callLookup : public QObject, public callLookupParse
{
    Q_OBJECT
public:
    explicit callLookup(QObject* parent = nullptr);

    void configure(const QString& dataDir, const QString& hamqthUser, const QString& hamqthPassword,
                   const QString& qrzUser, const QString& qrzPassword);
    // Asynchronous: looked() follows, also on failure.
    void lookup(const QString& call);

    static constexpr const char* kIsedUrl = "https://apc-cap.ic.gc.ca/datafiles/amateur_delim.zip";

signals:
    void looked(QString call, QJsonObject result);

private:
    QString isedPath() const;
    bool isedFresh() const;
    void fetchIsed();
    void lookupIsed(const QString& call);
    void lookupOther(const QString& call);
    void qrz(const QString& call, bool retried);
    void hamqth(const QString& call, bool retried);
    void get(const QString& url, std::function<void(int status, QByteArray body, QString err)> done);

    QNetworkAccessManager* nam = nullptr;
    QString dataDir_, hamqthUser_, hamqthPassword_, qrzUser_, qrzPassword_;
    QString hamqthSession_, qrzKey_;
    QStringList isedWaiting_;
    bool isedFetching_ = false;
};
