#pragma once

#include <QByteArray>
#include <QVector>
#include <deque>
#include <map>
#include <optional>
#include "MediaPacket.hpp"

// The receiving side of a media session: packets become frames (lost
// fragments rebuilt from repair packets where possible), frames are handed
// out in order, and every few tens of milliseconds the sender hears exactly
// which packets arrived and when, which ones to send again, and whether a
// keyframe is needed. A frame that cannot be completed in time is given up
// and the stream restarts from the next keyframe.
namespace ucd::screen {

class MediaReceiver {
public:
    static constexpr int FeedbackIntervalMs = 50, KeepAliveMs = 500;
    void reset(quint16 session, quint8 kind, int fps);
    // A media or repair packet; returns the frames now complete and in order.
    QVector<EncodedFrame> receive(const QByteArray& packet, qint64 nowMs);
    // Gives up on frames too old to complete; call often (each tick).
    void expire(qint64 nowMs);
    bool feedbackDue(qint64 nowMs) const;
    // The feedback to send now (empty if nothing to say): `playoutMs` the
    // playout delay in use, `decodeMs` the decode time, for the sender's view.
    QByteArray takeFeedback(qint64 nowMs, int playoutMs, double decodeMs);
    void requestKeyframe() { keyWanted_ = true; }
    // Drops everything being assembled and waits for a keyframe (the decoder
    // lost its way, or fell far behind).
    void restartFromKeyframe() { frames_.clear(); waiting_ = true; keyWanted_ = true; }
    bool waitingForKeyframe() const { return waiting_; }
    size_t pendingFrames() const { return frames_.size(); }
    // This report period's counters (reset by report()): packets received,
    // reported missing, rebuilt from repair packets, late (of frames already
    // delivered), received again on request; frames given up on, and packets
    // discarded while waiting for a keyframe.
    struct Stats { quint64 received = 0, lost = 0, recovered = 0, late = 0, repaired = 0, givenUp = 0, discarded = 0; };
    const Stats& stats() const { return stats_; }
    Stats report();
    double jitterMs() const { return jitterMs_; }
    double trendMs() const { return trendMs_; }
    // The longest a recent gap took to fill (a repeat arriving, or the packet
    // merely late), fading over a few seconds.
    double repairDelayMs() const { return repairDelayMs_; }
    // The playout delay that would show every frame on time, from the jitter
    // and the time gaps take to fill; the caller smooths it.
    int suggestedPlayoutMs() const;
    std::optional<quint32> latestCaptureMs() const { return latestCaptureMs_; }
    // The frames still being assembled, for diagnostics: "number:got/count".
    QString pendingText() const;
    QStringList giveLog;  // diagnostics: why frames were given up (last few)
private:
    struct Assembly {
        EncodedFrame frame;
        QVector<QByteArray> parts;
        QVector<bool> have;
        int got = 0, count = 0;
        qint64 firstMs = 0, lastMs = 0;  // first and latest packet of it arriving
        quint32 lowSeq = 0, highSeq = 0;  // its packets' sequence range, to stop asking for them once it is whole
        // Repair packets by first fragment, with their group size.
        struct Repair { int size; QByteArray payload; };
        std::map<int, Repair> repairs;
        bool complete() const { return got == count; }
        bool tryRepair();
    };
    QVector<EncodedFrame> deliver();
    void give(std::map<quint32, Assembly>::iterator it);
    void noteArrival(quint32 seq, qint64 nowMs);
    quint16 session_ = 0; quint8 kind_ = 1; int interval_ = 33;
    bool waiting_ = true, haveLast_ = false, keyWanted_ = false;
    quint32 last_ = 0;
    std::map<quint32, Assembly> frames_;
    // Arrivals by sequence since the last feedback, and sequences still missing.
    std::map<quint32, qint64> arrivals_;
    bool haveSeq_ = false; quint32 highestSeq_ = 0, feedbackBase_ = 0;
    std::map<quint32, qint64> missing_;  // seq -> when first noticed
    quint32 nackFloor_ = 0; bool haveFloor_ = false;  // the last delivered frame's highest sequence
    qint64 lastFeedbackMs_ = 0, newestArrivalMs_ = 0;
    Stats stats_;
    std::optional<quint32> latestCaptureMs_;
    quint32 latestFrame_ = 0;
    double jitterMs_ = 0, trendMs_ = 0, repairDelayMs_ = 0;
    qint64 lastExpireMs_ = 0;
    std::optional<double> previousTransit_;
};

}  // namespace ucd::screen
