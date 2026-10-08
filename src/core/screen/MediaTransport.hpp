#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QVector>
#include <functional>
#include <map>
#include <optional>

namespace ucd::screen {

constexpr int FragmentHeader = 16;
constexpr int FragmentPayload = 1084;
constexpr int MaxFrameBytes = 4 * 1024 * 1024;
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

class PacedSender {
public:
    bool enqueue(const EncodedFrame& frame, qint64 nowMs, int intervalMs);
    // At most 64 fragments per tick. A failed send drops the remainder rather
    // than accumulating old video behind a blocked TCP fallback.
    bool pump(qint64 nowMs, const std::function<bool(const QByteArray&)>& send);
    QVector<QByteArray> retransmit(quint32 frame, const QVector<int>& indexes, qint64 nowMs) const;
    void reset();
    bool busy() const { return !pending_.isEmpty(); }
    quint64 dropped() const { return dropped_; }
private:
    QVector<QByteArray> pending_, keyframe_;
    int next_ = 0;
    qint64 start_ = 0, deadline_ = 0, keyExpires_ = 0;
    int interval_ = 33;
    quint32 keyNumber_ = 0;
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
        bool nacked = false, complete = false;
    };
    QVector<EncodedFrame> deliver();
    void erase(std::map<quint32, Assembly>::iterator it);
    quint16 session_ = 0;
    int interval_ = 33;
    bool waiting_ = true, haveLast_ = false, requestKey_ = false;
    quint32 last_ = 0;
    qint64 lastKeyRequest_ = -1000;
    std::map<quint32, Assembly> frames_;
    qsizetype buffered_ = 0;
    Feedback stats_;
    std::optional<double> previousTransit_;
    double trend_ = 0;
};

class CongestionController {
public:
    void reset(const QString& path, int maxFps = 30, int maxWidth = 1920, int maxBitrate = 8000000);
    void update(const Feedback& report);
    int bitrate() const { return bitrate_; }
    int fps() const { return fps_; }
    int width() const { return width_; }
private:
    int bitrate_ = 4000000, cap_ = 8000000, fps_ = 30, maxFps_ = 30;
    int width_ = 1920, maxWidth_ = 1920, bad_ = 0, good_ = 0;
};

}  // namespace ucd::screen
