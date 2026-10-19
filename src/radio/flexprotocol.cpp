// FlexRadio SmartSDR wire protocol helpers. See include/flexprotocol.h.
//
// Parts of this file are ported from AetherSDR
// (https://github.com/aethersdr/AetherSDR), Copyright the AetherSDR
// contributors, licensed GPLv3 like wfweb.

#include "flexprotocol.h"

#include <QtEndian>
#include <QStringList>
#include <cmath>
#include <cstring>

namespace flexProtocol {

// ─── TCP API ────────────────────────────────────────────────────────────────

QMap<QString, QString> parseKVs(const QString& body)
{
    // "freq=14.225000 mode=USB filter_lo=100 filter_hi=2800". A quoted value
    // may hold spaces (`name="CQ Contest"`): it is kept whole, quotes
    // included, up to the token that closes it (AetherSDR CommandParser).
    QMap<QString, QString> result;
    const QStringList tokens = body.split(' ', Qt::SkipEmptyParts);
    for (int i = 0; i < tokens.size(); ++i) {
        QString token = tokens[i];
        if (token.contains(QLatin1String("=\"")) && token.count('"') == 1) {
            int end = i + 1;
            while (end < tokens.size() && !tokens[end].contains('"'))
                ++end;
            if (end < tokens.size()) {
                for (int j = i + 1; j <= end; ++j)
                    token += QLatin1Char(' ') + tokens[j];
                i = end;
            }
        }
        const int eq = token.indexOf('=');
        if (eq < 0)
            result.insert(token, QString());   // bare word, e.g. "removed"
        else
            result.insert(token.left(eq), token.mid(eq + 1));
    }
    return result;
}

message parseLine(const QString& rawLine)
{
    message msg;
    msg.raw = rawLine.trimmed();
    if (msg.raw.isEmpty())
        return msg;

    const QChar tag = msg.raw.at(0);
    const QString body = msg.raw.mid(1);

    switch (tag.toLatin1()) {
    case 'V':
        msg.type = msgType::Version;
        msg.object = body;
        break;

    case 'H':
        msg.type = msgType::Handle;
        msg.handle = body.toUInt(nullptr, 16);
        break;

    case 'R': {
        // R<seq>|<code>|<body>. The code is a 32-bit hex value; fatal codes
        // have the high bit set, so it must not go through toInt().
        msg.type = msgType::Response;
        const int p1 = body.indexOf('|');
        const int p2 = p1 < 0 ? -1 : body.indexOf('|', p1 + 1);
        msg.sequence = body.left(p1 < 0 ? body.size() : p1).toUInt();
        if (p1 >= 0) {
            const QString code = body.mid(p1 + 1, p2 < 0 ? -1 : p2 - p1 - 1);
            bool ok = false;
            const quint32 c = code.toUInt(&ok, 16);
            if (ok && !code.isEmpty() && code.size() <= 8) {
                msg.resultCode = c;
                msg.resultValid = true;
            }
        }
        if (p2 >= 0) {
            msg.object = body.mid(p2 + 1);
            msg.kvs = parseKVs(msg.object);
        }
        break;
    }

    case 'S': {
        // S<handle>|<object words> key=val ... — object names have no '=',
        // so the object ends at the last space before the first '='.
        msg.type = msgType::Status;
        const int pipe = body.indexOf('|');
        if (pipe < 0)
            break;
        msg.handle = body.left(pipe).toUInt(nullptr, 16);
        const QString statusBody = body.mid(pipe + 1);
        const int firstEq = statusBody.indexOf('=');
        if (firstEq < 0) {
            msg.object = statusBody.trimmed();
        } else {
            const int kvStart = statusBody.lastIndexOf(' ', firstEq);
            if (kvStart < 0) {
                msg.kvs = parseKVs(statusBody);
            } else {
                msg.object = statusBody.left(kvStart).trimmed();
                msg.kvs = parseKVs(statusBody.mid(kvStart + 1));
            }
        }
        break;
    }

    case 'M': {
        msg.type = msgType::Message;
        const int pipe = body.indexOf('|');
        if (pipe >= 0) {
            msg.handle = body.left(pipe).toUInt(nullptr, 16);
            msg.object = body.mid(pipe + 1);
        }
        break;
    }

    default:
        break;
    }
    return msg;
}

QByteArray buildCommand(quint32 seq, const QString& command)
{
    return QString("C%1|%2\n").arg(seq).arg(command).toUtf8();
}

// ─── VITA-49 ────────────────────────────────────────────────────────────────

vitaPacket parseVita(const QByteArray& data)
{
    vitaPacket p;
    if (data.size() < 8)
        return p;
    const auto* raw = reinterpret_cast<const uchar*>(data.constData());
    const quint32 word0 = qFromBigEndian<quint32>(raw);

    p.packetType = quint8((word0 >> 28) & 0x0F);
    p.hasClassId = (word0 & 0x08000000u) != 0;
    p.hasTrailer = (word0 & 0x04000000u) != 0;
    const int tsi = (word0 >> 22) & 0x03;
    const int tsf = (word0 >> 20) & 0x03;
    p.count = (word0 >> 16) & 0x0F;

    int off = 4;
    // Packet types 1 (IF data) and 3 (extension data) carry a stream id;
    // FlexRadio only uses those two.
    if (p.packetType == 1 || p.packetType == 3) {
        p.streamId = qFromBigEndian<quint32>(raw + off);
        off += 4;
    }
    if (p.hasClassId) {
        if (data.size() < off + 8)
            return p;
        p.oui = qFromBigEndian<quint32>(raw + off) & 0x00FFFFFFu;
        const quint32 codes = qFromBigEndian<quint32>(raw + off + 4);
        p.icc = quint16(codes >> 16);
        p.pcc = quint16(codes & 0xFFFF);
        off += 8;
    }
    if (tsi != 0) off += 4;
    if (tsf != 0) off += 8;

    // Use the datagram length, not the header's word count: Flex Opus
    // packets are byte-exact and shorter than sizeWords * 4.
    const int payload = data.size() - off - (p.hasTrailer ? 4 : 0);
    if (payload < 0)
        return p;
    p.payloadOffset = off;
    p.payloadBytes = payload;
    p.valid = true;
    return p;
}

QVector<QPair<quint16, qint16>> parseMeters(const QByteArray& data, const vitaPacket& p)
{
    QVector<QPair<quint16, qint16>> out;
    if (!p.valid)
        return out;
    const auto* src = reinterpret_cast<const uchar*>(data.constData()) + p.payloadOffset;
    const int n = p.payloadBytes / 4;
    out.reserve(n);
    for (int i = 0; i < n; ++i) {
        const quint16 id = qFromBigEndian<quint16>(src + i * 4);
        const qint16 val = qFromBigEndian<qint16>(src + i * 4 + 2);
        out.append(qMakePair(id, val));
    }
    return out;
}

float meterScale(const QString& unit, qint16 raw)
{
    // FlexLib VitaMeterPacket / AetherSDR MeterModel::convertRaw.
    if (unit == QLatin1String("dBm") || unit == QLatin1String("dB")
            || unit == QLatin1String("dBFS") || unit == QLatin1String("SWR"))
        return float(raw) / 128.0f;
    if (unit == QLatin1String("Volts") || unit == QLatin1String("Amps"))
        return float(raw) / 256.0f;
    if (unit == QLatin1String("degF") || unit == QLatin1String("degC"))
        return float(raw) / 64.0f;
    return float(raw);
}

QVector<float> decodeAudio(const QByteArray& data, const vitaPacket& p)
{
    QVector<float> out;
    if (!p.valid)
        return out;
    const auto* src = reinterpret_cast<const uchar*>(data.constData()) + p.payloadOffset;

    if (p.pcc == PCC_IF_NARROW) {
        if (p.payloadBytes < 8)
            return out;
        const int frames = p.payloadBytes / 8;
        out.resize(frames);
        for (int i = 0; i < frames; ++i) {
            const quint32 l = qFromBigEndian<quint32>(src + i * 8);
            const quint32 r = qFromBigEndian<quint32>(src + i * 8 + 4);
            float fl, fr;
            std::memcpy(&fl, &l, 4);
            std::memcpy(&fr, &r, 4);
            if (!std::isfinite(fl)) fl = 0.0f;
            if (!std::isfinite(fr)) fr = 0.0f;
            out[i] = 0.5f * (fl + fr);
        }
    } else if (p.pcc == PCC_IF_NARROW_REDUCED) {
        const int n = p.payloadBytes / 2;
        out.resize(n);
        for (int i = 0; i < n; ++i)
            out[i] = float(qFromBigEndian<qint16>(src + i * 2)) / 32768.0f;
    }
    return out;
}

QByteArray buildDaxTxPacket(quint32 streamId, int packetCount, const float* stereo, int frames)
{
    // FlexLib DAXTXAudioStream: IFDataWithStream, class id present, no
    // trailer, TSI=3 (other), TSF=1 (sample count), timestamps left zero.
    const int payloadWords = frames * 2;
    const int packetWords = 7 + payloadWords;
    QByteArray pkt(packetWords * 4, '\0');
    auto* w = reinterpret_cast<quint32*>(pkt.data());

    const quint32 hdr = (0x1u << 28) | (1u << 27) | (0x3u << 22) | (0x1u << 20)
                      | (quint32(packetCount & 0x0F) << 16) | quint32(packetWords & 0xFFFF);
    w[0] = qToBigEndian(hdr);
    w[1] = qToBigEndian(streamId);
    w[2] = qToBigEndian(FLEX_OUI);
    w[3] = qToBigEndian((quint32(FLEX_ICC) << 16) | PCC_IF_NARROW);
    w[4] = 0; w[5] = 0; w[6] = 0;

    for (int i = 0; i < payloadWords; ++i) {
        quint32 u;
        std::memcpy(&u, &stereo[i], 4);
        w[7 + i] = qToBigEndian(u);
    }
    return pkt;
}

bool fftAssembler::feed(const QByteArray& data, const vitaPacket& p)
{
    // FFT sub-header (12 bytes, big-endian) after the VITA header:
    //   uint16 startBin, uint16 numBins, uint16 binSize, uint16 totalBins,
    //   uint32 frameIndex; then numBins x uint16 bins.
    if (!p.valid || p.payloadBytes < 12)
        return false;
    const auto* sub = reinterpret_cast<const uchar*>(data.constData()) + p.payloadOffset;
    const quint16 startBin = qFromBigEndian<quint16>(sub + 0);
    const quint16 numBins = qFromBigEndian<quint16>(sub + 2);
    const quint16 binSize = qFromBigEndian<quint16>(sub + 4);
    const quint16 totalBins = qFromBigEndian<quint16>(sub + 6);
    const quint32 frame = qFromBigEndian<quint32>(sub + 8);
    if (numBins == 0 || totalBins == 0 || binSize != 2)
        return false;

    const int available = (p.payloadBytes - 12) / 2;
    const int n = qMin<int>(numBins, available);
    if (n <= 0)
        return false;

    if (frame != frameIndex || totalBins != buf.size()) {
        frameIndex = frame;
        buf.fill(0, totalBins);
        covered.fill(false, totalBins);
        coveredCount = 0;
        emitted = false;
    }
    if (emitted || int(startBin) + n > buf.size())
        return false;

    const uchar* bins = sub + 12;
    for (int i = 0; i < n; ++i) {
        const int idx = startBin + i;
        buf[idx] = qFromBigEndian<quint16>(bins + i * 2);
        if (!covered[idx]) {
            covered[idx] = true;
            ++coveredCount;
        }
    }
    if (coveredCount < buf.size())
        return false;
    emitted = true;
    return true;
}

QMap<QString, QString> parseDiscovery(const QByteArray& datagram)
{
    // The payload is ASCII after a VITA header; parsing the whole datagram
    // as text (as AetherSDR does) works too, since the binary header bytes
    // never form a key=value token. Skip it when it parses cleanly.
    QByteArray text = datagram;
    const vitaPacket p = parseVita(datagram);
    if (p.valid && p.pcc == PCC_DISCOVERY)
        text = datagram.mid(p.payloadOffset, p.payloadBytes);

    QMap<QString, QString> out;
    const QString s = QString::fromLatin1(text).trimmed();
    for (const QString& token : s.split(' ', Qt::SkipEmptyParts)) {
        const int eq = token.indexOf('=');
        if (eq <= 0)
            continue;
        QString value = token.mid(eq + 1);
        value.remove(QChar('\0'));
        out.insert(token.left(eq).toLower(), value.trimmed());
    }
    return out;
}

// ─── Filters ────────────────────────────────────────────────────────────────

bool modeHasFixedFilter(const QString& mode)
{
    return mode == QLatin1String("FM") || mode == QLatin1String("NFM")
        || mode == QLatin1String("DFM");
}

filterEdges edgesForWidth(const QString& mode, int widthHz)
{
    // SmartSDR placement, from AetherSDR ModeFilterPresets::edgesForWidth
    // with its defaults (DIGU/DIGL offset 1500 Hz, RTTY shift 170 Hz).
    filterEdges e;
    if (widthHz <= 0 || modeHasFixedFilter(mode))
        return e;

    if (mode == QLatin1String("USB")) {
        e.lo = 100; e.hi = 100 + widthHz;
    } else if (mode == QLatin1String("LSB")) {
        e.hi = -100; e.lo = -100 - widthHz;
    } else if (mode == QLatin1String("DIGU")) {
        if (widthHz < 3000) {
            e.lo = 1500 - widthHz / 2;
            e.hi = 1500 + widthHz / 2;
            if (e.lo < 95) { e.hi += 95 - e.lo; e.lo = 95; }
        } else {
            e.lo = 95; e.hi = widthHz;
        }
    } else if (mode == QLatin1String("DIGL")) {
        if (widthHz < 3000) {
            e.hi = -1500 + widthHz / 2;
            e.lo = -1500 - widthHz / 2;
            if (e.hi > -95) { e.lo -= e.hi + 95; e.hi = -95; }
        } else {
            e.lo = -widthHz; e.hi = -95;
        }
    } else if (mode == QLatin1String("RTTY")) {
        const int mid = -170 / 2;
        e.lo = mid - widthHz / 2;
        e.hi = mid + widthHz / 2;
    } else {
        // CW (carrier-relative, the radio's BFO owns the pitch), AM, SAM.
        e.lo = -widthHz / 2;
        e.hi = widthHz / 2;
    }
    return e;
}

int presetWidth(const QString& mode, int filterNum)
{
    // FIL1 widest .. FIL3 narrowest, picked from SmartSDR's own preset ladder.
    const int f = qBound(1, filterNum, 3) - 1;
    static const int ssb[3]  = {2700, 2400, 1800};
    static const int cw[3]   = {1000, 500, 250};
    static const int dig[3]  = {3000, 2000, 500};
    static const int am[3]   = {10000, 8000, 6000};
    static const int rtty[3] = {1000, 500, 300};

    if (mode == QLatin1String("CW")) return cw[f];
    if (mode == QLatin1String("DIGU") || mode == QLatin1String("DIGL")) return dig[f];
    if (mode == QLatin1String("AM") || mode == QLatin1String("SAM")) return am[f];
    if (mode == QLatin1String("RTTY")) return rtty[f];
    if (modeHasFixedFilter(mode)) return 0;
    return ssb[f];
}

} // namespace flexProtocol
