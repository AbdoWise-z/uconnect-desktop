#include "MediaPacket.hpp"

#include <QtEndian>
#include <algorithm>

namespace ucd::screen {
namespace {
template<class T> T read(const QByteArray& b, int at) { return qFromBigEndian<T>(reinterpret_cast<const uchar*>(b.constData() + at)); }
template<class T> void write(QByteArray& b, int at, T value) { qToBigEndian<T>(value, reinterpret_cast<uchar*>(b.data() + at)); }
template<class T> void append(QByteArray& b, T value) { const int at = b.size(); b.resize(at + int(sizeof(T))); write(b, at, value); }
}  // namespace

QByteArray encodeMedia(const MediaPacket& p) {
    QByteArray out(MediaHeader, '\0');
    out[0] = char(p.kind); out[1] = char(p.type);
    write<quint16>(out, 2, p.session); write<quint32>(out, 4, p.seq); write<quint32>(out, 8, p.frame);
    write<quint16>(out, 12, p.frag); write<quint16>(out, 14, p.count); write<quint32>(out, 16, p.captureMs);
    out[20] = char((p.keyframe ? 1 : 0) | (p.retransmit ? 2 : 0));
    out += p.payload;
    return out;
}

std::optional<MediaPacket> decodeMedia(const QByteArray& b) {
    if (b.size() < MediaHeader || b.size() > MaxDatagram) return std::nullopt;
    const auto type = quint8(b[1]);
    if (type != quint8(PacketType::Media) && type != quint8(PacketType::Fec)) return std::nullopt;
    MediaPacket p;
    p.kind = quint8(b[0]); p.type = PacketType(type);
    p.session = read<quint16>(b, 2); p.seq = read<quint32>(b, 4); p.frame = read<quint32>(b, 8);
    p.frag = read<quint16>(b, 12); p.count = read<quint16>(b, 14); p.captureMs = read<quint32>(b, 16);
    p.keyframe = quint8(b[20]) & 1; p.retransmit = quint8(b[20]) & 2;
    p.payload = b.mid(MediaHeader);
    if (!p.session || !p.count || p.count > MaxFragments || p.frag >= p.count) return std::nullopt;
    if (p.type == PacketType::Fec && (p.payload.size() < FecHeader || !quint8(p.payload[0]) || p.frag + quint8(p.payload[0]) > p.count)) return std::nullopt;
    return p;
}

QVector<MediaPacket> packetize(quint8 kind, const EncodedFrame& frame, quint32& nextSeq, int fecGroup) {
    QVector<MediaPacket> out;
    if (!frame.session || frame.bytes.isEmpty() || frame.bytes.size() > MaxFrameBytes) return out;
    const int count = int((frame.bytes.size() + MediaPayload - 1) / MediaPayload);
    MediaPacket base;
    base.kind = kind; base.session = frame.session; base.frame = frame.number;
    base.count = quint16(count); base.captureMs = frame.captureMs; base.keyframe = frame.keyframe;
    QByteArray repair; quint16 xorLength = 0; int inGroup = 0, groupStart = 0;
    for (int index = 0; index < count; ++index) {
        MediaPacket p = base;
        p.seq = nextSeq++; p.frag = quint16(index);
        p.payload = frame.bytes.mid(index * MediaPayload, MediaPayload);
        out.push_back(p);
        if (fecGroup > 0) {
            if (inGroup == 0) { groupStart = index; repair = QByteArray(MediaPayload, '\0'); xorLength = 0; }
            for (int i = 0; i < p.payload.size(); ++i) repair[i] = char(quint8(repair[i]) ^ quint8(p.payload[i]));
            xorLength ^= quint16(p.payload.size());
            ++inGroup;
            if (inGroup == fecGroup || index + 1 == count) {
                MediaPacket f = base;
                f.type = PacketType::Fec; f.seq = nextSeq++;
                f.frag = quint16(groupStart);
                f.payload.resize(FecHeader); f.payload[0] = char(inGroup); write<quint16>(f.payload, 1, xorLength);
                // Only as long as the longest fragment it covers.
                int longest = 0;
                for (int i = groupStart; i < groupStart + inGroup; ++i) longest = std::max(longest, int(out[i].payload.size()));
                f.payload += repair.left(longest);
                out.push_back(f);
                inGroup = 0;
            }
        }
    }
    return out;
}

QByteArray Feedback::encode(quint8 kind) const {
    QByteArray out;
    out.reserve(32 + 2 * arrivals.size() + 4 * nacks.size());
    out.append(char(kind)); out.append(char(PacketType::Feedback));
    append<quint16>(out, session); append<quint32>(out, baseSeq);
    append<quint16>(out, quint16(std::min(arrivals.size(), qsizetype(MaxArrivals))));
    append<quint32>(out, refTimeMs); append<quint16>(out, heldMs);
    append<quint32>(out, latestFrame); append<quint32>(out, latestCaptureMs);
    append<quint16>(out, playoutMs); append<quint16>(out, decodeMs10);
    out.append(char((keyframeWanted ? 1 : 0) | (haveLatest ? 2 : 0)));
    for (qsizetype i = 0; i < std::min(arrivals.size(), qsizetype(MaxArrivals)); ++i) append<quint16>(out, arrivals[i]);
    const int n = int(std::min(nacks.size(), qsizetype(MaxNacks)));
    out.append(char(n));
    for (int i = 0; i < n; ++i) append<quint32>(out, nacks[i]);
    return out;
}

std::optional<Feedback> Feedback::decode(const QByteArray& b) {
    if (b.size() < 29 || quint8(b[1]) != quint8(PacketType::Feedback)) return std::nullopt;
    Feedback f;
    f.session = read<quint16>(b, 2); f.baseSeq = read<quint32>(b, 4);
    const int count = read<quint16>(b, 8);
    f.refTimeMs = read<quint32>(b, 10); f.heldMs = read<quint16>(b, 14);
    f.latestFrame = read<quint32>(b, 16); f.latestCaptureMs = read<quint32>(b, 20);
    f.playoutMs = read<quint16>(b, 24); f.decodeMs10 = read<quint16>(b, 26);
    f.keyframeWanted = quint8(b[28]) & 1; f.haveLatest = quint8(b[28]) & 2;
    int at = 29;
    if (count > MaxArrivals || b.size() < at + 2 * count + 1) return std::nullopt;
    for (int i = 0; i < count; ++i, at += 2) f.arrivals.push_back(read<quint16>(b, at));
    const int nacks = quint8(b[at++]);
    if (nacks > MaxNacks || b.size() < at + 4 * nacks) return std::nullopt;
    for (int i = 0; i < nacks; ++i, at += 4) f.nacks.push_back(read<quint32>(b, at));
    if (!f.session) return std::nullopt;
    return f;
}

}  // namespace ucd::screen
