#include "kiwisource.h"
#include <QCoreApplication>
#include <QTcpServer>
#include <QtWebSockets/QWebSocketServer>
#include <QTimer>
#include <cassert>
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    QWebSocketServer server(QStringLiteral("test"),QWebSocketServer::NonSecureMode);
    assert(server.listen(QHostAddress::LocalHost,0));
    QStringList commands;
    QObject::connect(&server,&QWebSocketServer::newConnection,&app,[&] {
        auto *peer=server.nextPendingConnection();assert(peer);
        QObject::connect(peer,&QWebSocket::textMessageReceived,&app,[&](const QString &v){commands.append(v);});
        QTimer::singleShot(100,peer,[peer] {
            QByteArray msg("MSG badp=0 zoom_max=14 zoom_cap=14");peer->sendBinaryMessage(msg);
            QByteArray row(1028,char(155));row[0]='W';row[1]='/';row[2]='F';row[3]=1;
            peer->sendBinaryMessage(row);
        });
    });
    bool got=false;
    KiwiSource source(&app,[&](const SdrSpectrum::Frame &f,quint32 center,quint32 rate){
        assert(f.db.size()==1024 && f.db[0]==-100);
        assert(center>6000000 && center<8000000 && rate==1875000);got=true;app.quit();
    },[](const QString &){});
    KiwiSource::Config config;config.host="127.0.0.1";config.port=server.serverPort();
    assert(source.start(config));QTimer::singleShot(4000,&app,&QCoreApplication::quit);app.exec();
    assert(got);
    assert(commands.contains("SET auth t=kiwi p=#"));
    assert(commands.contains("SET wf_comp=0"));
    source.stop();
}
