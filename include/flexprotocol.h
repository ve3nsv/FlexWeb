#ifndef FLEXPROTOCOL_H
#define FLEXPROTOCOL_H

// FlexRadio SmartSDR wire protocol helpers (6000 / 8000 / Aurora series).
//
// Pure Qt Core, no QObject: the TCP line parser, the VITA-49 packet decoders
// (panadapter FFT, meters, audio) and the DAX TX packet builder that
// flexCommander uses. Kept free of wfweb types so it can be unit-tested on
// its own (tests/flexprotocol_test.cpp).
//
// Ported from AetherSDR (https://github.com/aethersdr/AetherSDR, GPLv3):
//   src/core/backends/flex/CommandParser.cpp     -> parseLine / parseKVs
//   src/core/backends/flex/PanadapterStream.cpp  -> VITA-49 decode, FFT frame assembly
//   src/core/AudioEngine.cpp (buildVitaTxPacket) -> buildDaxTxPacket
//   src/core/RadioDiscovery.cpp                  -> parseDiscovery
//   src/models/MeterModel.cpp                    -> meterScale
//   src/gui/ModeFilterPresets.cpp                -> edgesForWidth

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QVector>
#include <QPair>

namespace flexProtocol {

// ---------------------------------------------------------------------------
// TCP API (port 4992). All lines are ASCII terminated by '\n':
//   V<version>                     radio firmware version, first line
//   H<hex handle>                  our client handle, second line
//   C<seq>|<command>               client -> radio
//   R<seq>|<hex code>|<body>       response to command <seq>, code 0 = OK
//   S<hex handle>|<object> k=v ... status
//   M<hex id>|<text>               message from the radio
// ---------------------------------------------------------------------------

enum class msgType { Unknown, Version, Handle, Response, Status, Message };

struct message {
    msgType type = msgType::Unknown;
    quint32 sequence = 0;      // R lines
    quint32 handle = 0;        // H / S / M lines
    quint32 resultCode = 0;    // R lines (hex on the wire)
    bool resultValid = false;  // false when the code field did not parse
    QString object;            // "slice 0", "display pan 0x40000000", R body, M text
    QString raw;               // the whole line, trimmed
    QMap<QString, QString> kvs;
};

QMap<QString, QString> parseKVs(const QString& body);
message parseLine(const QString& line);
QByteArray buildCommand(quint32 seq, const QString& command);

// ---------------------------------------------------------------------------
// VITA-49 (UDP; radio sends to our registered port, we send to radio:4991)
// ---------------------------------------------------------------------------

constexpr quint32 FLEX_OUI = 0x001C2D;
constexpr quint16 FLEX_ICC = 0x534C;

constexpr quint16 PCC_IF_NARROW         = 0x03E3; // audio, float32 stereo big-endian
constexpr quint16 PCC_IF_NARROW_REDUCED = 0x0123; // audio, int16 mono big-endian
constexpr quint16 PCC_OPUS              = 0x8005; // audio, Opus
constexpr quint16 PCC_METER             = 0x8002;
constexpr quint16 PCC_FFT               = 0x8003;
constexpr quint16 PCC_WATERFALL         = 0x8004;
constexpr quint16 PCC_DISCOVERY         = 0xFFFF;

constexpr int DAX_RATE = 24000;         // remote_audio_rx (uncompressed) and dax_tx
constexpr int DAX_TX_FRAMES = 128;      // stereo frames per DAX TX packet (as FlexLib)

struct vitaPacket {
    bool valid = false;
    quint8 packetType = 0;
    bool hasClassId = false;
    bool hasTrailer = false;
    int count = 0;              // 4-bit packet counter
    quint32 streamId = 0;
    quint32 oui = 0;
    quint16 icc = 0;
    quint16 pcc = 0;
    int payloadOffset = 0;
    int payloadBytes = 0;
};

vitaPacket parseVita(const QByteArray& datagram);

// Meter packet payload: N x (uint16 id, int16 raw value), big-endian.
QVector<QPair<quint16, qint16>> parseMeters(const QByteArray& datagram, const vitaPacket& p);

// Raw meter value -> engineering units, by the unit the radio declared.
float meterScale(const QString& unit, qint16 raw);

// Audio payload -> mono float samples in [-1, 1]. Float stereo is downmixed
// (the radio puts the same slice audio in both channels unless panned).
QVector<float> decodeAudio(const QByteArray& datagram, const vitaPacket& p);

// DAX TX packet: float32 stereo big-endian at 24 kHz, PCC 0x03E3.
QByteArray buildDaxTxPacket(quint32 streamId, int packetCount, const float* stereo, int frames);

// One panadapter FFT stream. The radio splits a frame across several
// datagrams; feed() each one and it returns true when a frame is complete.
// Bin values are vertical pixel positions (0 = top = max_dbm).
class fftAssembler {
public:
    bool feed(const QByteArray& datagram, const vitaPacket& p);
    const QVector<quint16>& bins() const { return buf; }
    void reset() { frameIndex = 0xFFFFFFFF; buf.clear(); covered.clear(); coveredCount = 0; }
private:
    quint32 frameIndex = 0xFFFFFFFF;
    QVector<quint16> buf;
    QVector<bool> covered;
    int coveredCount = 0;
    bool emitted = false;
};

// Discovery broadcast (UDP 4992): ASCII "key=value" tokens after a VITA header.
QMap<QString, QString> parseDiscovery(const QByteArray& datagram);

// ---------------------------------------------------------------------------
// Slice filter edges for a passband width, the way SmartSDR places them
// (Hz, relative to the slice frequency). FM-family modes return lo == hi
// == 0: their passband is fixed by the radio.
// ---------------------------------------------------------------------------
struct filterEdges { int lo = 0; int hi = 0; };
filterEdges edgesForWidth(const QString& mode, int widthHz);

// The three wfweb filter slots (FIL1 widest .. FIL3 narrowest) per mode.
int presetWidth(const QString& mode, int filterNum);
bool modeHasFixedFilter(const QString& mode);

} // namespace flexProtocol

#endif // FLEXPROTOCOL_H
