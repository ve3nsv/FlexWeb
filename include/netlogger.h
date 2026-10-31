#pragma once
#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QRegularExpression>
#include <QSet>
#include <memory>
#include <QTimer>
#include <QUrlQuery>
#include <QXmlStreamReader>
#include <algorithm>
#include <functional>
#include <optional>

namespace NetLogger {
inline int minimumIntervalMs(const QString &routine) {
    if (routine == "GetActiveNets" || routine == "GetPastNets") return 60000;
    if (routine == "GetCheckins") return 20000;
    if (routine == "GetPastNetCheckins") return 6000;
    return 0;
}
inline int cacheTtlMs(const QString &routine) {
    return routine == "GetPastNetCheckins" ? 3600000 : minimumIntervalMs(routine);
}
class Limiter {
public:
    bool allow(const QString &routine, qint64 nowMs) {
        const int interval=minimumIntervalMs(routine);
        if (!interval || nowMs < backoffUntil_) return false;
        if(last_.contains(routine) && nowMs-last_[routine] < interval)return false;
        last_[routine]=nowMs;return true;
    }
    void backoff(qint64 nowMs,int seconds) {backoffUntil_=(std::max)(backoffUntil_,nowMs+qint64(std::clamp(seconds,60,3600))*1000);}
private:
    QHash<QString,qint64> last_;qint64 backoffUntil_=0;
};
// Parse only the fields needed by the panel. Private addresses and ZIP codes
// are never forwarded to connected browsers. Reject DTDs and malformed XML.
inline std::optional<QJsonObject> parse(const QByteArray &body) {
    if(body.size()>512*1024)return std::nullopt;
    QXmlStreamReader xml(body);QJsonArray nets,checkins;QJsonObject item,result;
    QString server;QStringList stack;int elements=0;bool root=false;QString response;
    const QStringList netFields{"NetName","AltNetName","Frequency","Mode","Band","NetControl","Logger","Date","SubscriberCount","NetID"};
    const QStringList checkFields{"SerialNo","Callsign","FirstName","PreferredName","State","Grid","Remarks","Status"};
    while(!xml.atEnd()){
        const auto token=xml.readNext();
        if(token==QXmlStreamReader::DTD || token==QXmlStreamReader::EntityReference)return std::nullopt;
        if(token==QXmlStreamReader::StartElement){
            if(++elements>20000||stack.size()>=32)return std::nullopt;
            const QString name=xml.name().toString();stack.append(name);
            if(stack.size()==1){if(name!="NetLoggerXML")return std::nullopt;root=true;}
            if(name=="Net"||name=="Checkin")item=QJsonObject();
            const bool leaf = netFields.contains(name)||checkFields.contains(name)||name=="ServerName"||name=="Pointer"||name=="CheckinCount"||name=="ResponseCode"||name=="Error";
            if(leaf){
                QString text=xml.readElementText(QXmlStreamReader::ErrorOnUnexpectedElement).trimmed();stack.removeLast();
                if(text.size()>512||xml.hasError())return std::nullopt;
                if(name=="ServerName")server=text;
                else if(name=="ResponseCode")response=text;
                else if(name=="Error" && !text.isEmpty())result["error"]=text;
                else if(name=="Pointer"||name=="CheckinCount")result[name]=text;
                else item[name]=text;
            }
        } else if(token==QXmlStreamReader::EndElement){
            const QString name=xml.name().toString();
            if(name=="Net"){if(nets.size()>=1000)return std::nullopt;item["ServerName"]=server;nets.append(item);}
            if(name=="Checkin"){if(checkins.size()>=1000)return std::nullopt;checkins.append(item);}
            if(stack.isEmpty()||stack.takeLast()!=name)return std::nullopt;
        }
    }
    if(xml.hasError()||!root||!stack.isEmpty())return std::nullopt;
    if(!response.isEmpty()&&!response.startsWith("200"))result["error"]=response;
    result["nets"]=nets;result["checkins"]=checkins;return result;
}
class Client : public QObject {
public:
    using Callback=std::function<void(const QJsonObject &)>;
    explicit Client(QObject *parent=nullptr):QObject(parent){clock_.start();}
    void request(const QString &routine,const QJsonObject &args,const QString &callsign,Callback callback){
        const int ttl=cacheTtlMs(routine);if(!ttl){callback({{"error","Unsupported NetLogger request"}});return;}
        QUrlQuery query;QString key=routine;
        if(routine=="GetCheckins"||routine=="GetPastNetCheckins"){
            const QString server=args["server"].toString().trimmed(),net=args["net"].toString().trimmed();
            if(!QRegularExpression("^[A-Za-z0-9_-]{1,32}$").match(server).hasMatch()||net.isEmpty()||net.size()>128||net.contains(QRegularExpression("[\\x00-\\x1f]"))){callback({{"error","Invalid net or server name"}});return;}
            query.addQueryItem("ServerName",server);query.addQueryItem("NetName",net);key+="|"+server+"|"+net;
            if(routine=="GetPastNetCheckins"){
                const QString id=args["id"].toString();if(!QRegularExpression("^[0-9]{1,20}$").match(id).hasMatch()){callback({{"error","Invalid net ID"}});return;}
                query.addQueryItem("NetID",id);key+="|"+id;
            }
        } else if(routine=="GetPastNets"){
            const int days=args["days"].toInt(7);const QString filter=args["filter"].toString().trimmed();
            if(days<1||days>365||(days>7&&filter.isEmpty())||filter.size()>64){callback({{"error","Choose a valid date range and name filter"}});return;}
            query.addQueryItem("Interval",QString::number(days));if(!filter.isEmpty())query.addQueryItem("NetNameLike",filter);key+="|"+QString::number(days)+"|"+filter;
        }
        QString call=callsign.trimmed().toUpper();
        if(!QRegularExpression("^[A-Z0-9/]{1,16}$").match(call).hasMatch()){
            callback({{"error","Set your station callsign before using NetLogger"}});return;
        }
        const qint64 now=clock_.elapsed();
        if(cache_.contains(key)&&now-cache_[key].when<ttl){auto o=cache_[key].data;o["cached"]=true;o["ageSeconds"]=double(now-cache_[key].when)/1000;callback(o);return;}
        if(inFlight_.contains(key)||!limiter_.allow(routine,now)){
            if(cache_.contains(key)){auto o=cache_[key].data;o["cached"]=true;o["stale"]=true;o["ageSeconds"]=double(now-cache_[key].when)/1000;callback(o);}
            else callback({{"error","NetLogger request is rate limited; try again shortly"}});
            return;
        }
        QUrl url("https://www.netlogger.org/api/"+routine+".php");url.setQuery(query);
        QNetworkRequest req(url);
        req.setRawHeader("User-Agent",QString("FlexWeb/1.0 (%1; +https://github.com/ve3nsv/FlexWeb)").arg(call).toUtf8());
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::ManualRedirectPolicy);
        auto *reply=network_.get(req);inFlight_.insert(key);
        auto *deadline=new QTimer(reply);deadline->setSingleShot(true);connect(deadline,&QTimer::timeout,reply,&QNetworkReply::abort);deadline->start(15000);
        auto data=std::make_shared<QByteArray>();connect(reply,&QNetworkReply::readyRead,this,[reply,data]{*data+=reply->readAll();if(data->size()>512*1024)reply->abort();});
        connect(reply,&QNetworkReply::finished,this,[this,reply,data,key,callback]{
            *data+=reply->readAll();inFlight_.remove(key);
            const int http=reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if(http==429){bool ok=false;int retry=reply->rawHeader("Retry-After").toInt(&ok);limiter_.backoff(clock_.elapsed(),ok?retry:60);}
            if(reply->error()!=QNetworkReply::NoError||http!=200||data->size()>512*1024){
                auto o=cache_.contains(key)?cache_[key].data:QJsonObject{};o["error"]=http==429?"NetLogger rate limit":"NetLogger unavailable";
                if(cache_.contains(key)){o["cached"]=true;o["stale"]=true;o["ageSeconds"]=double(clock_.elapsed()-cache_[key].when)/1000;}callback(o);
            }else if(auto parsed=parse(*data)){
                if(!parsed->contains("error"))cache_[key]=Cache{*parsed,clock_.elapsed()};
                callback(*parsed);
            }else callback({{"error","Invalid NetLogger response"}});
            reply->deleteLater();
        });
    }
private:
    struct Cache {QJsonObject data;qint64 when=0;};
    QNetworkAccessManager network_;QElapsedTimer clock_;Limiter limiter_;QHash<QString,Cache> cache_;QSet<QString> inFlight_;
};
} // namespace NetLogger
