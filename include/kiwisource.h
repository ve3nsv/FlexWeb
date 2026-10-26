#pragma once
#include "kiwiwaterfall.h"
#include <QByteArray>
#include <QDateTime>
#include <QObject>
#include <QTimer>
#include <QUrl>
#include <QtWebSockets/QWebSocket>
#include <functional>

// Kiwi waterfall-only connection; no audio or transmit path is opened.
class KiwiSource : public QObject {
public:
    struct Config {QString host,password;quint16 port=8073;quint32 center=7171000,span=1800000;};
    using FrameCallback=std::function<void(const SdrSpectrum::Frame &,quint32,quint32)>;
    using StatusCallback=std::function<void(const QString &)>;
    KiwiSource(QObject *parent,FrameCallback frame,StatusCallback statusCallback)
        :QObject(parent),socket_(QString(),QWebSocketProtocol::VersionLatest),frame_(std::move(frame)),status_(std::move(statusCallback)){
        timer_.setInterval(5000);
        connect(&timer_,&QTimer::timeout,this,[this]{send("SET keepalive");});
        connect(&socket_,&QWebSocket::connected,this,[this]{
            const QByteArray encoded=config_.password.isEmpty()?QByteArray("#"):QUrl::toPercentEncoding(config_.password);
            send("SET auth t=kiwi p="+QString::fromLatin1(encoded));
            send("SERVER DE CLIENT FlexWeb W/F");send("SET wf_comp=0");send("SET send_dB=1");
            send("SET maxdb=-10 mindb=-110");send("SET wf_speed=3");requestView();timer_.start();status("connecting");
        });
        connect(&socket_,&QWebSocket::binaryMessageReceived,this,[this](const QByteArray &msg){receive(msg);});
        connect(&socket_,&QWebSocket::textMessageReceived,this,[this](const QString &msg){receive(msg.toUtf8());});
        connect(&socket_,&QWebSocket::disconnected,this,[this]{timer_.stop();if(!failed_)status("disconnected");});
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
        connect(&socket_,&QWebSocket::errorOccurred,this,[this](QAbstractSocket::SocketError){status(socket_.errorString());});
#else
        connect(&socket_,QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),this,[this](QAbstractSocket::SocketError){status(socket_.errorString());});
#endif
    }
    bool start(const Config &c){
        if(c.host.trimmed().isEmpty()||c.host.size()>253||!c.port||QUrl::toPercentEncoding(c.password).size()>256||!c.span)return false;
        stop();config_=c;zoomMax_=14;zoomCap_=14;valid_=false;failed_=false;
        QUrl url;url.setScheme("ws");url.setHost(c.host);url.setPort(c.port);
        const auto id=(quint64(1)<<62)+quint64(QDateTime::currentMSecsSinceEpoch());
        url.setPath(QStringLiteral("/ws/kiwi/%1/W/F").arg(id));
        socket_.open(url);status("connecting");return true;
    }
    void stop(){timer_.stop();socket_.abort();valid_=false;}
    void setCenter(quint32 hz){if(!hz || hz==config_.center)return;config_.center=hz;requestView();}
private:
    void send(const QString &s){if(socket_.state()==QAbstractSocket::ConnectedState)socket_.sendTextMessage(s);}
    void status(const QString &s){if(status_)status_(s);}
    void requestView(){
        if(config_.center>30000000){valid_=false;status("outside Kiwi HF range");return;}
        auto view=KiwiWaterfall::view(config_.center,config_.span,30000000,std::min(zoomMax_,zoomCap_));
        if(!view){valid_=false;return;}
        // Start units use advertised zoom_max even when the receiver caps the
        // available zoom. Recalculate with the correct fixed-point scale.
        const double scale=std::ldexp(1024.0,zoomMax_);
        view->start=quint32(std::round(view->lowHz/30000000.0*scale));
        view->lowHz=double(view->start)/scale*30000000.0;
        view->highHz=view->lowHz+30000000.0/std::pow(2.0,view->zoom);
        view_=*view;valid_=true;
        send(QStringLiteral("SET zoom=%1 start=%2").arg(view_.zoom).arg(view_.start));
    }
    void receive(const QByteArray &msg){
        if(msg.startsWith("MSG")){
            const auto tokens=QString::fromUtf8(msg.mid(4)).split(' ',Qt::SkipEmptyParts);
            bool changed=false;
            for(const auto &token:tokens){
                const int eq=token.indexOf('=');if(eq<0)continue;
                const auto key=token.left(eq);bool ok=false;int value=token.mid(eq+1).toInt(&ok);
                if(key=="badp" && ok && value!=0){failed_=true;status(QStringLiteral("Kiwi access refused (%1)").arg(value));socket_.close();return;}
                if(key=="zoom_max" && ok && value>=0 && value<=20){zoomMax_=value;changed=true;}
                if(key=="zoom_cap" && ok && value>=0 && value<=20){zoomCap_=value;changed=true;}
            }
            if(changed)requestView();return;
        }
        if(!valid_)return;
        auto row=KiwiWaterfall::decode(reinterpret_cast<const std::uint8_t*>(msg.constData()),std::size_t(msg.size()),zoomCap_);
        if(!row)return;
        double low=view_.lowHz,high=view_.highHz;
        if(row->extended){
            const double scale=std::ldexp(1024.0,zoomMax_);
            low=double(row->start)/scale*30000000.0;high=low+30000000.0/std::pow(2.0,row->zoom);
        }
        if(low<0||high>30000001||config_.center<low||config_.center>high)return;
        status("receiving");if(frame_)frame_(row->spectrum,quint32(std::round((low+high)/2)),quint32(std::round(high-low)));
    }
    Config config_;QWebSocket socket_;QTimer timer_;FrameCallback frame_;StatusCallback status_;
    KiwiWaterfall::View view_{};int zoomMax_=14,zoomCap_=14;bool valid_=false,failed_=false;
};
