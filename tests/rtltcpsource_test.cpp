#include "rtltcpsource.h"
#include <QCoreApplication>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <cassert>
#include <cmath>
int main(int argc, char **argv) {
    QCoreApplication app(argc,argv);
    QTcpServer server; assert(server.listen(QHostAddress::LocalHost, 0));
    QByteArray commands;
    QObject::connect(&server,&QTcpServer::newConnection,&app,[&] {
        auto *peer=server.nextPendingConnection(); assert(peer);
        QObject::connect(peer,&QTcpSocket::readyRead,&app,[&,peer] {commands+=peer->readAll();});
        peer->write(QByteArray("RTL0\0\0\0\5\0\0\0\35",12));
        QTimer::singleShot(400,peer,[peer] {
            QByteArray data; data.reserve(4096);
            for(int i=0;i<2048;i++) {
                double a=2*3.141592653589793*128*i/2048;
                data.append(char(int(127.5+60*std::cos(a))));
                data.append(char(int(127.5+60*std::sin(a))));
            }
            peer->write(data);
        });
    });
    RtlTcpSource source; RtlTcpSource::Config c; c.port=server.serverPort();
    bool got=false;
    source.setFrameCallback([&](const SdrSpectrum::Frame &frame, quint32 hz, quint32 rate) {
        assert(hz==14230000 && rate==2048000);
        assert(frame.db.size()==2048);
        assert(std::max_element(frame.db.begin(),frame.db.end())-frame.db.begin()==1024+128);
        got=true; app.quit();
    });
    assert(source.start(c));
    QTimer::singleShot(4000,&app,&QCoreApplication::quit);
    app.exec();
    assert(got && source.status().tuner==5 && source.status().gainCount==29);
    assert(commands.size() >= 30); // sample rate, gain, ppm, direct, bias, frequency
    assert(static_cast<unsigned char>(commands[0])==2);
    source.stop();
}
