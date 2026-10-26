// Exercise the real HTTP transport, including commands queued behind an
// in-flight read. A prepend/LIFO command queue switches to the wrong antenna.
#include "rfkitconnection.h"
#include "logcategories.h"
#include <QCoreApplication>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <cassert>
#include <memory>
Q_LOGGING_CATEGORY(logRig, "test.rfkit")
using namespace AetherSDR;
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QString scenario = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("queue");
    QElapsedTimer elapsed; elapsed.start();
    bool transportFailed = false;
    QTcpServer server;
    assert(server.listen(QHostAddress::LocalHost));
    RfkitConnection connection;
    QObject::connect(&connection, &RfkitConnection::connectionFailed, &app, [&](const QString &why) {
        if (scenario == "queue") return;
        assert(!why.contains("Not an RF-KIT"));
        if (scenario == "deadline") assert(elapsed.elapsed() >= 9000 && elapsed.elapsed() < 14000);
        transportFailed = true;
        app.quit();
    });
    QStringList commands;
    bool queued = false, finished = false;
    QObject::connect(&server, &QTcpServer::newConnection, &app, [&]() {
        while (server.hasPendingConnections()) {
            auto *socket = server.nextPendingConnection();
            auto bytes = std::make_shared<QByteArray>();
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket, bytes]() {
                *bytes += socket->readAll();
                const int end = bytes->indexOf("\r\n\r\n");
                if (end < 0 || socket->property("handled").toBool()) return;
                int length = 0;
                for (const auto &line : bytes->left(end).split('\n'))
                    if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toInt();
                if (bytes->size() < end + 4 + length) return;
                socket->setProperty("handled", true);
                const auto head = bytes->left(bytes->indexOf("\r\n")).split(' ');
                const QByteArray path = head[1], method = head[0];
                if (scenario != "queue") {
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 10000\r\n\r\n{");
                    if (scenario == "truncated") socket->disconnectFromHost();
                    else {
                        auto *trickle = new QTimer(socket);
                        QObject::connect(trickle, &QTimer::timeout, socket, [socket]() { socket->write(" "); });
                        trickle->start(100);
                    }
                    return;
                }
                QByteArray body = "{}";
                int delay = 0;
                if (path == "/info") body = R"({"device":"RF2K-S"})";
                if (path == "/power" && !queued) {
                    queued = true;
                    delay = 100;
                    connection.selectAntenna({Rfkit::AntennaType::Internal, 1});
                    connection.selectAntenna({Rfkit::AntennaType::Internal, 2});
                }
                if (method == "PUT" && path == "/antennas/active") {
                    commands << QString::fromUtf8(bytes->mid(end + 4));
                    if (commands.size() == 2) {
                        assert(commands[0].contains("\"number\":1"));
                        assert(commands[1].contains("\"number\":2"));
                        finished = true;
                        QTimer::singleShot(10, &app, &QCoreApplication::quit);
                    }
                }
                QTimer::singleShot(delay, socket, [socket, body]() {
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: "
                        + QByteArray::number(body.size()) + "\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
            });
        }
    });
    connection.connectNetwork("127.0.0.1", server.serverPort());
    QTimer::singleShot(scenario == "deadline" ? 15000 : 5000, &app, &QCoreApplication::quit);
    app.exec();
    assert(scenario == "queue" ? finished : transportFailed);
}
