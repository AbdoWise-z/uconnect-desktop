#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QVector>
#include <deque>
#include <functional>
#include <map>
#include <optional>

namespace ucd::screen {

constexpr int FragmentHeader = 16;
constexpr int FragmentPayload = 1084;
constexpr int MaxFrameBytes = 4 * 1024 * 1024;
// Highest frame rate a session may ask for or run at (high refresh displays).
constexpr int MaxFps = 165;
constexpr int MaxFragments = (MaxFrameBytes + FragmentPayload - 1) / FragmentPayload;

struct EncodedFrame {
    quint16 session = 0;
    quint32 number = 0;
    quint32 captureMs = 0;
    bool keyframe = false;
    QByteArray bytes;
};

// Network-byte-order wire encoding. No native structs are put on the wire.
QVector<QByteArray> fragment(const EncodedFrame& frame);

// Sends encoded frames as datagrams, spread out so a large frame does not
// leave in one burst. Frames wait in a short queue: a frame arriving while the
// previous one is still going out is queued, not dropped. Datagrams the path
// refuses (still opening, or the TCP fallback's queue is full) are retried on
// the next pump. Only frames that would arrive too late to be worth showing
// are dropped; the caller then forces a keyframe, since the frames after a
// dropped one cannot be decoded.
class PacedSender {
public:
    static constexpr int MaxQueuedFrames = 3;
    // False if the queue is full: the frame is dropped.
    // `spread`: the share of the frame interval its datagrams are spread over.
    // `paceBps`: if set, no frame goes out faster than that.
    bool enqueue(const EncodedFrame& frame, qint64 nowMs, int intervalMs, double spread = 0.5, int paceBps = 0);
    // False if queued frames were dropped for being too old.
    bool pump(qint64 nowMs, const std::function<bool(const QByteArray&)>& send);
    // Fragments of a frame sent in the last half second, marked as repairs.
    QVector<QByteArray> retransmit(quint32 frame, const QVector<int>& indexes, qint64 nowMs) const;
    void reset();
    bool busy() const { return !queue_.empty(); }
    int queuedFrames() const { return int(queue_.size()); }
    quint64 dropped() const { return dropped_; }
private:
    // Each frame goes out on a straight line from `start` to `due`, about
    // half a frame interval, starting when the one before it is due.
    struct Queued {
        QVector<QByteArray> packets;
        int next = 0;
        qint64 enqueued = 0, start = 0, due = 0;
    };
    std::deque<Queued> queue_;
    // Recent frames, kept a short while so lost fragments can be sent again.
    struct Sent {
        quint32 number = 0;
        QVector<QByteArray> packets;
        qint64 expires = 0;
    };
    std::deque<Sent> history_;
    int interval_ = 33;
    quint64 dropped_ = 0;
};

struct Feedback {
    quint64 received = 0, lost = 0, late = 0;
    double jitterMs = 0, owdTrendMs = 0, decodeMs = 0;
    QJsonObject json() const;
    static std::optional<Feedback> parse(const QJsonObject& message);
};

struct Recovery {
    bool keyframe = false;
    struct Nack { quint32 frame; QVector<int> indexes; };
    QVector<Nack> nacks;
};

class Reassembler {
public:
    void reset(quint16 session, int fps = 30);
    // Patient (extended displays on Wi-Fi, kept across resets): an incomplete
    // frame gets up to three repair requests 50 ms apart (in a burst of
    // loss the resent packets can be lost too) and 16 frames may be in
    // flight, before it is dropped and a keyframe asked for.
    void setPatient(bool patient) { patient_ = patient; }
    // The host sends over TCP: nothing is lost, only late, so a frame gets
    // much longer to arrive and no repair is asked for (asking for a keyframe
    // would only put more into a full pipe).
    void setReliable(bool reliable) { reliable_ = reliable; }
    static constexpr int ReliableWaitMs = 1500;
    bool reliable() const { return reliable_; }
    QVector<EncodedFrame> receive(const QByteArray& packet, qint64 nowMs);
    Recovery expire(qint64 nowMs);
    Feedback report(double decodeMs = 0);
    bool waitingForKeyframe() const { return waiting_; }
    qsizetype bufferedBytes() const { return buffered_; }
    size_t pendingFrames() const { return frames_.size(); }
private:
    struct Assembly {
        EncodedFrame frame;
        QVector<QByteArray> parts;
        int received = 0;
        qint64 deadline = 0;
        int nacks = 0;  // repair requests sent for it
        bool complete = false;
    };
    QVector<EncodedFrame> deliver();
    void erase(std::map<quint32, Assembly>::iterator it);
    quint16 session_ = 0;
    int interval_ = 33;
    bool waiting_ = true, haveLast_ = false, requestKey_ = false, patient_ = false, reliable_ = false;
    quint32 last_ = 0;
    qint64 lastKeyRequest_ = -1000;
    std::map<quint32, Assembly> frames_;
    qsizetype buffered_ = 0;
    Feedback stats_;
    std::optional<double> previousTransit_;
    double trend_ = 0;
};

// Adjusts bitrate, frame rate and width from the viewer's reports (one every
// ~300 ms). Network trouble (loss, rising delay) lowers the bitrate, at most
// once per few reports so one bad patch is not punished repeatedly. A viewer
// that cannot decode fast enough lowers the frame rate instead: a smaller
// bitrate would not help it. Clean reports raise the bitrate gradually, then
// the frame rate and width.
// Whether a UDP path loses too much to stream over, from the viewer's
// reports: some networks (a mobile uplink policing UDP) drop a large share of
// datagrams at any rate, and the stream gets through only over TCP. True once
// `needed` of the last `window` reports lost more than `threshold`.
class LossyPath {
public:
    bool report(quint64 received, quint64 lost);
    void reset() { recent_.clear(); }
    static constexpr int Window = 10, Needed = 6;
    static constexpr double Threshold = 0.15;
private:
    std::deque<bool> recent_;
};

class CongestionController {
public:
    // `startBitrate` replaces the default start on a direct path (extended
    // displays start high: they only run between nearby devices).
    // Starts at 8 Mbit/s on a direct path, 3 relayed, 2 while still opening,
    // within `maxBitrate`; clean reports raise it about 8% each.
    void reset(const QString& path, int maxFps = 60, int maxWidth = 1920, int maxBitrate = 16000000, int startBitrate = 0);
    void update(const Feedback& report);
    // Ride out short bursts of loss and delay (kept across resets).
    void setTolerant(bool tolerant) { tolerant_ = tolerant; }
    int bitrate() const { return bitrate_; }
    int fps() const { return fps_; }
    int width() const { return width_; }
    static constexpr int MinBitrate = 300000;
    static constexpr int MaxBitrate = 100000000;
private:
    int bitrate_ = 4000000, cap_ = 8000000, fps_ = 30, maxFps_ = 30;
    int width_ = 1920, maxWidth_ = 1920, bad_ = 0, good_ = 0, slowDecode_ = 0, cooldown_ = 0;
    bool tolerant_ = false;
    int congestedStreak_ = 0;
};

}  // namespace ucd::screen
