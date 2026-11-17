// Unit test for the FlexRadio wire helpers (include/flexprotocol.h): the
// SmartSDR TCP line parser, VITA-49 decoders, DAX TX packet builder and
// filter placement. Built and run by tests/test_cpp_units.py.
#include "flexprotocol.h"

#include <QDebug>
#include <QtEndian>
#include <cmath>
#include <cstdlib>
#include <cstring>

using namespace flexProtocol;

namespace {
void require(bool condition, const char *message)
{
    if (!condition) { qCritical() << "FAIL:" << message; ::exit(1); }
}

void put32(QByteArray &b, quint32 v) { const quint32 be = qToBigEndian(v); b.append(reinterpret_cast<const char *>(&be), 4); }
void put16(QByteArray &b, quint16 v) { const quint16 be = qToBigEndian(v); b.append(reinterpret_cast<const char *>(&be), 2); }

// A radio-to-client VITA header as the radio sends it: extension data with
// stream id, class id, TSI=3, TSF=1 (28 bytes).
QByteArray vitaHeader(quint8 type, quint32 streamId, quint16 pcc, int count = 0)
{
    QByteArray b;
    put32(b, (quint32(type) << 28) | (1u << 27) | (3u << 22) | (1u << 20) | (quint32(count & 0xF) << 16));
    put32(b, streamId);
    put32(b, FLEX_OUI);
    put32(b, (quint32(FLEX_ICC) << 16) | pcc);
    put32(b, 0); put32(b, 0); put32(b, 0);
    return b;
}

void testLines()
{
    message m = parseLine("V1.4.0.0\n");
    require(m.type == msgType::Version && m.object == "1.4.0.0", "version line");

    m = parseLine("H5A3B1C2D");
    require(m.type == msgType::Handle && m.handle == 0x5A3B1C2D, "handle line");

    m = parseLine("R12|0|0x40000000");
    require(m.type == msgType::Response && m.sequence == 12 && m.resultValid && m.resultCode == 0, "OK response");
    require(m.object == "0x40000000", "response body");

    // Fatal codes have the high bit set: must not read as 0 (success).
    m = parseLine("R7|F3000001|Unable to create slice");
    require(m.resultValid && m.resultCode == 0xF3000001u, "fatal response code kept");

    m = parseLine("R8|50000016|");
    require(m.resultCode == 0x50000016u && m.object.isEmpty(), "error response with empty body");

    m = parseLine("S5A3B1C2D|slice 0 RF_frequency=14.074000 mode=DIGU filter_lo=100 filter_hi=3100 client_handle=0x5A3B1C2D");
    require(m.type == msgType::Status && m.handle == 0x5A3B1C2D, "status handle");
    require(m.object == "slice 0", "slice object");
    require(m.kvs.value("RF_frequency") == "14.074000" && m.kvs.value("mode") == "DIGU", "slice kvs");
    require(m.kvs.value("filter_hi").toInt() == 3100, "filter_hi");

    m = parseLine("S0|display pan 0x40000000 center=14.100000 bandwidth=0.200000 x_pixels=1024");
    require(m.object == "display pan 0x40000000", "multi-word object");
    require(m.kvs.value("bandwidth") == "0.200000", "pan bandwidth");

    m = parseLine("S0|interlock state=TRANSMITTING source=SW");
    require(m.object == "interlock" && m.kvs.value("state") == "TRANSMITTING", "interlock");

    m = parseLine("S0|slice 1 removed");
    require(m.object == "slice 1 removed" && m.kvs.isEmpty(), "object with no kvs");

    QMap<QString, QString> kv = parseKVs("name=\"CQ Contest\" index=2");
    require(kv.value("name") == "\"CQ Contest\"" && kv.value("index") == "2", "quoted value with space");

    m = parseLine("M10000001|Client connected from IP 192.168.1.20");
    require(m.type == msgType::Message && m.object.startsWith("Client connected"), "message line");

    require(buildCommand(3, "slice tune 0 14.074000") == "C3|slice tune 0 14.074000\n", "command framing");
}

void testFft()
{
    // A 10-bin frame split over two datagrams, second one first.
    fftAssembler fft;
    auto part = [](quint16 start, QVector<quint16> bins, quint32 frame) {
        QByteArray d = vitaHeader(3, 0x40000000, PCC_FFT);
        put16(d, start); put16(d, quint16(bins.size())); put16(d, 2); put16(d, 10); put32(d, frame);
        for (quint16 b : bins) put16(d, b);
        return d;
    };
    QByteArray b = part(6, {60, 70, 80, 90}, 5);
    vitaPacket p = parseVita(b);
    require(p.valid && p.pcc == PCC_FFT && p.streamId == 0x40000000 && p.oui == FLEX_OUI, "FFT header");
    require(p.payloadOffset == 28, "28-byte VITA header");
    require(!fft.feed(b, p), "incomplete frame not emitted");

    b = part(0, {0, 10, 20, 30, 40, 50}, 5);
    require(fft.feed(b, parseVita(b)), "frame completes");
    require(fft.bins().size() == 10 && fft.bins()[0] == 0 && fft.bins()[9] == 90, "bins in place");
    require(!fft.feed(b, parseVita(b)), "complete frame emitted once");

    // A new frame index starts over.
    b = part(0, {1, 1, 1, 1, 1}, 6);
    require(!fft.feed(b, parseVita(b)), "half of next frame");
    b = part(5, {2, 2, 2, 2, 2}, 6);
    require(fft.feed(b, parseVita(b)) && fft.bins()[9] == 2, "next frame");

    // A fragment past the end of the frame is rejected, not written.
    b = part(8, {1, 1, 1, 1}, 7);
    require(!fft.feed(b, parseVita(b)), "overrun fragment ignored");
}

void testMeters()
{
    QByteArray d = vitaHeader(3, 0x700, PCC_METER);
    put16(d, 7); put16(d, quint16(qint16(-73 * 128)));     // SLC LEVEL, dBm
    put16(d, 12); put16(d, quint16(qint16(1.5 * 128)));    // SWR
    put16(d, 20); put16(d, quint16(qint16(13.8 * 256)));   // Volts
    const vitaPacket p = parseVita(d);
    const auto m = parseMeters(d, p);
    require(m.size() == 3 && m[0].first == 7 && m[2].first == 20, "meter ids");
    require(std::fabs(meterScale("dBm", m[0].second) + 73.0f) < 0.01f, "dBm scale");
    require(std::fabs(meterScale("SWR", m[1].second) - 1.5f) < 0.01f, "SWR scale");
    require(std::fabs(meterScale("Volts", m[2].second) - 13.8f) < 0.01f, "volts scale");
    require(std::fabs(meterScale("degC", 64 * 40) - 40.0f) < 0.01f, "temperature scale");
}

void testAudio()
{
    // Float stereo (remote_audio_rx uncompressed): downmixed to mono.
    QByteArray d = vitaHeader(1, 0x4000008, PCC_IF_NARROW);
    const float frames[4][2] = {{0.5f, 0.5f}, {-0.25f, -0.25f}, {1.0f, 0.0f}, {0.0f, 0.0f}};
    for (const auto &f : frames)
        for (float s : f) { quint32 u; std::memcpy(&u, &s, 4); put32(d, u); }
    vitaPacket p = parseVita(d);
    QVector<float> pcm = decodeAudio(d, p);
    require(pcm.size() == 4 && pcm[0] == 0.5f && pcm[1] == -0.25f && pcm[2] == 0.5f, "float stereo decode");

    // Reduced-bandwidth int16 mono.
    d = vitaHeader(1, 0x4000009, PCC_IF_NARROW_REDUCED);
    put16(d, quint16(qint16(16384))); put16(d, quint16(qint16(-32768)));
    pcm = decodeAudio(d, parseVita(d));
    require(pcm.size() == 2 && pcm[0] == 0.5f && pcm[1] == -1.0f, "int16 mono decode");

    // DAX TX packet: FlexLib layout, and it decodes back to what went in.
    float stereo[DAX_TX_FRAMES * 2];
    for (int i = 0; i < DAX_TX_FRAMES; ++i) {
        stereo[i * 2] = stereo[i * 2 + 1] = float(std::sin(i * 0.1));
    }
    const QByteArray tx = buildDaxTxPacket(0x84000000, 17, stereo, DAX_TX_FRAMES);
    require(tx.size() == 28 + DAX_TX_FRAMES * 8, "DAX TX size");
    const quint32 w0 = qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(tx.constData()));
    require((w0 >> 28) == 1, "IF data with stream id");
    require(((w0 >> 16) & 0xF) == 1, "4-bit packet counter wraps");
    require((w0 & 0xFFFF) == quint32(7 + DAX_TX_FRAMES * 2), "size in words");
    p = parseVita(tx);
    require(p.valid && p.streamId == 0x84000000 && p.pcc == PCC_IF_NARROW && !p.hasTrailer, "DAX TX header");
    pcm = decodeAudio(tx, p);
    require(pcm.size() == DAX_TX_FRAMES && std::fabs(pcm[5] - float(std::sin(0.5))) < 1e-6f, "DAX TX round trip");
}

void testDiscovery()
{
    QByteArray d = vitaHeader(3, 0x800, PCC_DISCOVERY);
    d.append("discovery_protocol_version=3.0.0.2 model=FLEX-6600 serial=1234-5678 version=3.8.19.29733 "
             "nickname=Shack callsign=VE3NSV ip=10.0.0.20 port=4992 status=Available");
    d.append('\0'); d.append('\0');
    const QMap<QString, QString> kv = parseDiscovery(d);
    require(kv.value("model") == "FLEX-6600", "discovery model");
    require(kv.value("ip") == "10.0.0.20" && kv.value("port") == "4992", "discovery address");
    require(kv.value("status") == "Available", "trailing NUL padding stripped");
}

void testFilters()
{
    filterEdges e = edgesForWidth("USB", 2700);
    require(e.lo == 100 && e.hi == 2800, "USB 2.7k");
    e = edgesForWidth("LSB", 2400);
    require(e.lo == -2500 && e.hi == -100, "LSB 2.4k");
    e = edgesForWidth("CW", 500);
    require(e.lo == -250 && e.hi == 250, "CW 500");
    e = edgesForWidth("DIGU", 3000);
    require(e.lo == 95 && e.hi == 3000, "DIGU 3k");
    e = edgesForWidth("DIGU", 500);
    require(e.lo == 1250 && e.hi == 1750, "DIGU 500 centred on 1500");
    e = edgesForWidth("DIGL", 500);
    require(e.lo == -1750 && e.hi == -1250, "DIGL 500");
    e = edgesForWidth("AM", 6000);
    require(e.lo == -3000 && e.hi == 3000, "AM 6k");
    e = edgesForWidth("FM", 6000);
    require(e.lo == 0 && e.hi == 0 && modeHasFixedFilter("NFM"), "FM fixed");

    require(presetWidth("USB", 1) == 2700 && presetWidth("USB", 3) == 1800, "SSB presets");
    require(presetWidth("CW", 2) == 500, "CW preset");
    require(presetWidth("DIGU", 1) == 3000, "DIGU preset wide enough for FT8");
    require(presetWidth("FM", 1) == 0, "FM no preset");
    require(presetWidth("USB", 9) == 1800, "filter number clamped");
}
} // namespace

int main()
{
    testLines();
    testFft();
    testMeters();
    testAudio();
    testDiscovery();
    testFilters();
    qInfo() << "flexprotocol: all tests passed";
    return 0;
}
