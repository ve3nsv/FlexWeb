#include "acomconnection.h"
#include <QCoreApplication>
#include <QTcpServer>
#include <QTimer>
#include <cassert>
int main(int argc,char **argv){
 QCoreApplication app(argc,argv);QTcpServer server;assert(server.listen(QHostAddress::LocalHost,0));
 QByteArray fromClient; bool telemetry=false, sent=false;
 QObject::connect(&server,&QTcpServer::newConnection,&app,[&]{
   auto *peer=server.nextPendingConnection();assert(peer);
   QObject::connect(peer,&QTcpSocket::readyRead,&app,[&,peer]{
     fromClient+=peer->readAll();
     if(fromClient.contains(AetherSDR::Acom::buildModeCommand(AetherSDR::Acom::ModeCommand::Operate))){sent=true;app.quit();}
   });
   QByteArray payload(68,'\0');payload[0]=char(0x50);payload[19]=char(0xc2);payload[20]=1;payload[63]=char(0xff);
   auto f=AetherSDR::Acom::buildFrame(static_cast<quint8>(AetherSDR::Acom::Address::Telemetry),payload);
   QTimer::singleShot(50,peer,[peer,f]{peer->write(f.left(6));peer->write(f.mid(6));});
 });
 AetherSDR::AcomConnection c;
 QObject::connect(&c,&AetherSDR::AcomConnection::telemetryUpdated,&app,[&](const AetherSDR::Acom::Telemetry &t){
   assert(t.forwardPowerW==450 && t.mode==AetherSDR::Acom::Mode::Standby);telemetry=true;c.setOperate(true);
 });
 c.connectNetwork("127.0.0.1",server.serverPort());QTimer::singleShot(4000,&app,&QCoreApplication::quit);app.exec();
 assert(telemetry && sent);c.disconnect();
}
