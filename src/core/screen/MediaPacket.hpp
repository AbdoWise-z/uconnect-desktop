#pragma once

#include <QByteArray>
#include <QVector>
#include <cstdint>
#include <optional>

// The media wire format (docs/protocol.md 7.1, screen/camera 2.0): every
// datagram of a session is numbered in one sequence, media and repair alike,
// so the receiver can tell the sender exactly which arrived and when. Nothing
// here depends on the network or the codec.
namespace ucd::screen {

constexpr int MaxDatagram = 1100;  // uconnect::Topic::max_datagram()
constexpr int MediaHeader = 21;
constexpr int FecHeader = 3;  // group size, XOR of lengths
// A repair packet carries a fragment-sized XOR after its own header, so
// fragments leave room for it.
constexpr int MediaPayload = MaxDatagram - MediaHeader - FecHeader;
constexpr int MaxFrameBytes = 4 * 1024 * 1024;
constexpr int MaxFragments = (MaxFrameBytes + MediaPayload - 1) / MediaPayload;
// Highest frame rate a session may ask for or run at (high refresh displays).
constexpr int MaxFps = 165;

struct EncodedFrame {
    quint16 session = 0;
    quint32 number = 0;
    quint32 captureMs = 0;
    bool keyframe = false;
    QByteArray bytes;
};

// True if sequence `a` comes after `b` (32-bit wrap).
inline bool seqNewer(quint32 a, quint32 b) { return qint32(a - b) > 0; }

enum class PacketType : quint8 { Media = 0, Fec = 1, Feedback = 2 };

// A media packet carries one fragment of a frame (`frag` of `count`). A
// repair (FEC) packet carries the same frame fields, `frag` being the first
// of a group of consecutive fragments, and as payload the group's size (one
// byte), the XOR of the fragments' lengths and the XOR of their payloads
// (zero-padded): any one of them that is lost can be rebuilt from the others.
struct MediaPacket {
    quint8 kind = 0;
    PacketType type = PacketType::Media;
    quint16 session = 0;
    quint32 seq = 0;
    quint32 frame = 0;
    quint16 frag = 0, count = 0;
    quint32 captureMs = 0;
    bool keyframe = false, retransmit = false;
    QByteArray payload;
};

QByteArray encodeMedia(const MediaPacket& packet);
std::optional<MediaPacket> decodeMedia(const QByteArray& packet);

// Splits a frame into media packets, each `seq` numbered in turn, with a
// repair packet after every `fecGroup` fragments (0: none). Empty if the
// frame is too large.
QVector<MediaPacket> packetize(quint8 kind, const EncodedFrame& frame, quint32& nextSeq, int fecGroup);

// The receiver's word back to the sender, every few tens of milliseconds:
// for each sequence from `baseSeq`, when it arrived (quarter milliseconds
// after `refTimeMs`, on the receiver's clock) or that it did not; which
// older ones it still wants again; the newest complete frame; and whether it
// needs a keyframe. Clocks are never compared across devices: only the
// differences between arrivals matter.
struct Feedback {
    static constexpr int MaxArrivals = 200, MaxNacks = 32;
    static constexpr quint16 Missing = 0xFFFF;
    quint16 session = 0;
    quint32 baseSeq = 0;
    quint32 refTimeMs = 0;
    quint16 heldMs = 0;  // from the newest arrival to this feedback being sent
    quint32 latestFrame = 0, latestCaptureMs = 0;
    quint16 playoutMs = 0;   // the receiver's playout delay
    quint16 decodeMs10 = 0;  // decode time, tenths of a millisecond
    bool keyframeWanted = false, haveLatest = false;
    QVector<quint16> arrivals;  // quarter ms after refTimeMs, or Missing
    QVector<quint32> nacks;
    QByteArray encode(quint8 kind) const;
    static std::optional<Feedback> decode(const QByteArray& packet);
};

// Which packet type a datagram of a media kind carries, without decoding it.
inline std::optional<PacketType> packetType(const QByteArray& packet) {
    if (packet.size() < 2 || quint8(packet[1]) > 2) return std::nullopt;
    return PacketType(quint8(packet[1]));
}

}  // namespace ucd::screen
