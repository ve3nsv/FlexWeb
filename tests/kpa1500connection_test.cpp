#include "Kpa1500Connection.h"
#include <QCoreApplication>
#include <QTcpServer>
#include <QTimer>
#include <cassert>
int main(int argc,char **argv){
 QCoreApplication app(argc,argv);QTcpServer server;assert(server.listen(QHostAddress::LocalHost,0));
 QByteArray commands;bool got=false,sent=false;
 QObject::connect(&server,&QTcpServer::newConnection,&app,[&]{
  auto *peer=server.nextPendingConnection();assert(peer);
  QObject::connect(peer,&QTcpSocket::readyRead,&app,[&,peer]{commands+=peer->readAll();if(commands.contains("^OS1;")){sent=true;app.quit();}});
  QTimer::singleShot(50,peer,[peer]{peer->write("^OS0;^PWF0450;^SW015;");});
 });
 AetherSDR::Kpa1500Connection c;
 QObject::connect(&c,&AetherSDR::Kpa1500Connection::statusUpdated,&app,[&](const AetherSDR::Kpa1500::Status &s){
  if(s.operate && !*s.operate && !got){got=true;c.setOperate(true);}
 });
 c.connectNetwork("127.0.0.1",server.serverPort());QTimer::singleShot(4000,&app,&QCoreApplication::quit);app.exec();
 assert(got && sent);c.disconnect();
}
