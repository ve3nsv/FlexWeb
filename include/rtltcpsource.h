#pragma once
#include "rtltcpprotocol.h"
#include "sdrspectrum.h"
#include <QAbstractSocket>
#include <QElapsedTimer>
#include <QObject>
#include <QTcpSocket>
#include <QTimer>
#include <complex>
#include <functional>
#include <vector>

// Remote rtl_tcp IQ source. An instance lives in the event loop of its owner;
// callbacks are executed there. It is receive-only and never drives a radio.
class RtlTcpSource : public QObject {
public:
    struct Config {
        QString host = QStringLiteral("127.0.0.1");
        quint16 port = 1234;
        quint32 sampleRate = 2048000;
        quint32 centerHz = 14230000;
        int gainTenthsDb = 0;
        int ppm = 0;
        bool autoGain = true;
        bool biasT = false;
        bool directQ = false;
    };
    struct Status {
        QString state = QStringLiteral("disconnected");
        quint32 tuner = 0, gainCount = 0;
        quint64 receivedBytes = 0;
        float clipped = 0;
    };
    using FrameCallback = std::function<void(const SdrSpectrum::Frame &, quint32, quint32)>;
    explicit RtlTcpSource(QObject *parent = nullptr) : QObject(parent) {
        socket_.setReadBufferSize(262144);
        connect(&socket_, &QTcpSocket::connected, this, [this] { status_.state = QStringLiteral("waiting for rtl_tcp"); });
        connect(&socket_, &QTcpSocket::readyRead, this, [this] { receive(); });
        connect(&socket_, &QTcpSocket::disconnected, this, [this] { status_.state = QStringLiteral("disconnected"); });
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
        connect(&socket_, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) { status_.state = socket_.errorString(); });
#else
        connect(&socket_, QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::error), this, [this](QAbstractSocket::SocketError) { status_.state = socket_.errorString(); });
#endif
    }
    void setFrameCallback(FrameCallback callback) { callback_ = std::move(callback); }
    bool start(const Config &c) {
        if (c.host.trimmed().isEmpty() || c.port == 0 || c.sampleRate < 225001 || c.sampleRate > 3200000 || c.centerHz == 0 || c.ppm < -1000 || c.ppm > 1000) return false;
        stop(); config_ = c; parser_.reset(); configured_ = false; status_ = Status{};
        status_.state = QStringLiteral("connecting"); socket_.connectToHost(c.host, c.port); return true;
    }
    void stop() { socket_.abort(); parser_.reset(); configured_ = false; iq_.clear(); status_.state = QStringLiteral("disconnected"); }
    const Status &status() const { return status_; }
    // Changing frequency clears partial IQ. The next frame is held until a
    // settling interval has passed so it is not labelled as the new centre.
    void setCenter(quint32 hz) {
        if (!hz || hz == config_.centerHz) return;
        config_.centerHz = hz; iq_.clear(); settle_.restart();
        if (configured_) writeCommand(1, hz);
    }
private:
    void writeCommand(quint8 type, quint32 value) {
        const auto bytes = RtlTcp::command(type, value);
        socket_.write(reinterpret_cast<const char *>(bytes.data()), static_cast<qint64>(bytes.size()));
    }
    void configure() {
        const auto greeting = parser_.greeting(); if (!greeting) return;
        status_.tuner = greeting->tuner; status_.gainCount = greeting->gainCount;
        writeCommand(2, config_.sampleRate);
        writeCommand(3, config_.autoGain ? 0 : 1);
        if (!config_.autoGain) writeCommand(4, static_cast<quint32>(config_.gainTenthsDb));
        writeCommand(5, static_cast<quint32>(config_.ppm));
        writeCommand(9, config_.directQ ? 2 : 0);
        writeCommand(14, config_.biasT ? 1 : 0);
        writeCommand(1, config_.centerHz);
        configured_ = true; status_.state = QStringLiteral("receiving"); settle_.restart();
    }
    void receive() {
        const QByteArray data = socket_.readAll();
        status_.receivedBytes += static_cast<quint64>(data.size());
        parser_.feed(reinterpret_cast<const std::uint8_t *>(data.constData()), static_cast<std::size_t>(data.size()),
            [this](std::uint8_t i, std::uint8_t q) {
                if (!configured_ || (settle_.isValid() && settle_.elapsed() < 250)
                    || (frameClock_.isValid() && frameClock_.elapsed() < 50)) return;
                if (iq_.size() < 2048) iq_.emplace_back((float(i)-127.5f)/127.5f, (float(q)-127.5f)/127.5f);
            });
        if (parser_.failed()) { status_.state = QStringLiteral("Invalid rtl_tcp greeting"); socket_.abort(); return; }
        if (!configured_ && parser_.greeting()) configure();
        if (iq_.size() == 2048 && (!frameClock_.isValid() || frameClock_.elapsed() >= 50)) {
            const auto frame = SdrSpectrum::transform(iq_); iq_.clear(); frameClock_.restart();
            status_.clipped = frame.overloadFraction;
            if (callback_) callback_(frame, config_.centerHz, config_.sampleRate);
        }
    }
    Config config_;
    Status status_;
    QTcpSocket socket_;
    RtlTcp::Parser parser_;
    std::vector<std::complex<float>> iq_;
    QElapsedTimer settle_, frameClock_;
    FrameCallback callback_;
    bool configured_ = false;
};
