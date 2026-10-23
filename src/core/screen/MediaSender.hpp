#pragma once

#include <QByteArray>
#include <QVector>
#include <deque>
#include <functional>
#include <map>
#include "Bandwidth.hpp"
#include "MediaPacket.hpp"

// The sending side of a media session, the way real-time services send:
// frames are cut into numbered packets, with repair packets when the path
// loses some, and paced out at a steady rate a little above the bitrate the
// estimator allows rather than a burst per frame. The queue is short: a
// frame that would wait longer than a fraction of a second is dropped before
// it goes out (and the caller is told to skip capturing while that lasts),
// so what reaches the viewer is recent. The receiver's feedback drives the
// bandwidth estimate, repeats of lost packets, keyframes, and the lag.
namespace ucd::screen {

class MediaSender {
public:
    struct Options {
        quint8 kind = 1;
        quint16 session = 0;
        // Starts modestly and climbs fast (doubling about every second) until
        // the path shows where it gives way: a start above that costs
        // seconds of a full queue.
        int startBps = 1500000, minBps = 150000, maxBps = 100000000;
        double pacingGain = 1.5;       // packets leave this much faster than the target rate
        int queueMs = 150;             // more than this much queued: frames are dropped, capture skipped
        int inflightMs = 300;          // over TCP: unacknowledged bytes bounded to this much of the target
        bool reliable = false;         // the path is TCP: nothing is lost, it queues
        int fps = 30;
    };
    void reset(const Options& options);
    // False if the frame is dropped: the sender waits for a keyframe after
    // dropping its queue (the next frame encoded must be one: see
    // takeKeyframeWanted), or the frame cannot be packetized. A deep queue
    // never drops an encoded frame (the frames after it would be useless):
    // it tells the capture to skip instead (shouldSkipCapture).
    bool enqueue(const EncodedFrame& frame, qint64 nowMs);
    // Sends what the pacing budget allows; call every few milliseconds.
    void pump(qint64 nowMs, const std::function<bool(const QByteArray&)>& send);
    // The receiver's feedback: arrivals (the bandwidth estimate), repeats,
    // lag, keyframe requests.
    void onFeedback(const QByteArray& packet, qint64 nowMs);
    // Capturing should pause: the queue, or what is still unacknowledged on
    // a reliable path, is deep enough that a new frame would only add lag.
    bool shouldSkipCapture(qint64 nowMs) const;
    // The next frame encoded should be a keyframe (the receiver asked, or
    // the queue was dropped); true at most once per few hundred ms.
    bool takeKeyframeWanted(qint64 nowMs);
    // The queue is dropped (a monitor change, the viewer far behind): the next
    // frame must be a keyframe.
    void dropQueued();
    void setReliable(bool reliable) { options_.reliable = reliable; }
    void setFps(int fps) { options_.fps = std::max(1, fps); }
    // What the path takes, and what the encoder should produce: the path's
    // rate less the repair packets' share.
    int targetBps() const { return bwe_.targetBps(); }
    int encoderBps() const {
        double share = fecGroup_ > 0 ? double(fecGroup_) / (fecGroup_ + 1) : 1.;
        share *= 1 - std::min(bwe_.lossRate(), 0.5);  // repeats take the rest
        return std::max(options_.minBps / 2, int(bwe_.targetBps() * share));
    }
    int receiveBps() const { return bwe_.receiveBps(); }
    double lossRate() const { return bwe_.lossRate(); }
    double rttMs() const { return rttMs_; }
    double lagMs() const { return lagMs_; }
    int fecGroup() const { return fecGroup_; }
    int queuedBytes() const { return queuedBytes_; }
    qint64 inflightBytes() const { return inflightBytes_; }
    quint64 dropped() const { return dropped_; }
    quint64 retransmitted() const { return retransmitted_; }
    bool awaitingKeyframe() const { return awaitKey_; }  // the next frame must be a keyframe
    QString estimatorState() const { return bwe_.state(); }
    int sendBps() const { return bwe_.sendBps(); }
    bool pathLimited() const { return bwe_.limited(); }
    bool applicationLimited() const { return bwe_.applicationLimited(); }
    quint32 lastSentCaptureMs() const { return lastSentCaptureMs_; }

private:
    struct Sent {
        QByteArray bytes;
        qint64 sentMs = 0, lastRepeatMs = 0;
        int size = 0;
        bool acknowledged = false, lost = false;
    };
    Options options_;
    BandwidthEstimator bwe_;
    quint32 nextSeq_ = 0;
    std::deque<MediaPacket> queue_;
    int queuedBytes_ = 0;
    double budgetBytes_ = 0;
    qint64 lastPumpMs_ = 0;
    std::map<quint32, Sent> sent_;  // recent packets, for repeats and feedback
    qint64 inflightBytes_ = 0;
    quint32 feedbackBase_ = 0;  // sequences before this were reported on
    bool haveFeedback_ = false;
    double rttMs_ = 0, lagMs_ = 0;
    int fecGroup_ = 0;
    quint32 lastSentCaptureMs_ = 0;
    bool awaitKey_ = false;
    qint64 lastKeyMs_ = -100000, behindAt_ = -100000;
    quint64 dropped_ = 0, retransmitted_ = 0;
    void trimSent(qint64 nowMs);
    qint64 inflightLimit() const;
};

}  // namespace ucd::screen
