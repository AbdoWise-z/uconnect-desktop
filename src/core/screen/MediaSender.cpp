#include "MediaSender.hpp"

#include <algorithm>
#include <cmath>

namespace ucd::screen {
namespace {
constexpr qint64 SentHistoryMs = 2000;   // packets kept for repeats
constexpr qint64 RepeatSpacingMs = 40;   // one repeat of a packet per this long
constexpr int BehindMs = 600;            // the viewer this far behind: start over from a keyframe
constexpr int MaxBurstBytes = 3 * MaxDatagram;
}  // namespace

void MediaSender::reset(const Options& options) {
    options_ = options; options_.fps = std::max(1, options.fps);
    bwe_.reset({options.startBps, options.minBps, options.maxBps});
    nextSeq_ = 0; queue_.clear(); queuedBytes_ = 0; budgetBytes_ = MaxBurstBytes; lastPumpMs_ = 0;
    sent_.clear(); inflightBytes_ = 0; feedbackBase_ = 0; haveFeedback_ = false;
    rttMs_ = 0; minRttMs_ = 0; lagMs_ = 0; smoothEncoderBps_ = 0; fecGroup_ = 0; lastSentCaptureMs_ = 0;
    awaitKey_ = false; lastKeyMs_ = -100000; behindAt_ = -100000;
    dropped_ = retransmitted_ = 0;
}

bool MediaSender::enqueue(const EncodedFrame& frame, qint64) {
    // Until the keyframe the receiver needs, other frames are useless to it.
    if (awaitKey_ && !frame.keyframe) { ++dropped_; return false; }
    // Repair packets in proportion to what the path loses, sized for its
    // spells of loss rather than the mean: none when it is clean, one per
    // two fragments when it loses a lot, one per fragment (each sent twice,
    // in effect) when it loses most of a spell.
    const double loss = std::max(bwe_.lossRate(), bwe_.burstLossRate());
    fecGroup_ = options_.reliable || loss < 0.02 ? 0 : loss < 0.08 ? 8 : loss < 0.20 ? 4 : loss < 0.45 ? 2 : 1;
    // A keyframe is big and everything after it depends on it: protected
    // more, so it needs fewer repeats.
    const int group = frame.keyframe && fecGroup_ > 2 ? 2 : fecGroup_;
    // Sequence numbers are given as packets leave (pump): a queue dropped
    // before it was sent must not read as loss at the receiver.
    quint32 scratch = 0;
    auto packets = packetize(options_.kind, frame, scratch, group);
    if (packets.isEmpty()) { ++dropped_; return false; }
    int bytes = 0; for (const auto& p : packets) bytes += MediaHeader + int(p.payload.size());
    awaitKey_ = false;
    for (auto& p : packets) { queue_.push_back(std::move(p)); }
    queuedBytes_ += bytes;
    lastSentCaptureMs_ = frame.captureMs;
    return true;
}

bool MediaSender::shouldSkipCapture(qint64 nowMs) const {
    const int target = bwe_.targetBps();
    if (queuedBytes_ > qint64(target) * options_.queueMs / 8000) return true;
    if (options_.reliable && inflightBytes_ > inflightLimit()) return true;
    return false;
}

// Over a reliable path what is unacknowledged is what waits in the socket
// and the network: bounded, or the lag grows unseen. At least a round trip
// and a half of it, though (the quickest seen: queueing lengthens the
// others, and a bound that grew with it would bound nothing), or a long
// path could never carry the rate.
qint64 MediaSender::inflightLimit() const {
    return qint64(bwe_.targetBps()) * std::max<qint64>(options_.inflightMs, qint64(1.5 * minRttMs_)) / 8000;
}

void MediaSender::pump(qint64 nowMs, const std::function<bool(const QByteArray&)>& send) {
    // Pacing: a budget of bytes grows at the pacing rate and is spent on
    // packets in order; a frame's packets leave a few at a time, not all
    // at once.
    const double rate = bwe_.targetBps() * options_.pacingGain / 8000.;  // bytes per ms
    if (lastPumpMs_) budgetBytes_ = std::min(budgetBytes_ + rate * double(nowMs - lastPumpMs_), double(MaxBurstBytes) + rate * 5);
    const double weight = lastPumpMs_ && smoothEncoderBps_ > 0 ? std::min(1., double(nowMs - lastPumpMs_) / 1000.) : 1.;
    smoothEncoderBps_ += (encoderBps() - smoothEncoderBps_) * weight;
    lastPumpMs_ = nowMs;
    trimSent(nowMs);
    const qint64 inflightLimit = this->inflightLimit();
    while (!queue_.empty()) {
        auto& p = queue_.front();
        const int size = MediaHeader + int(p.payload.size());
        if (budgetBytes_ < size) break;
        if (options_.reliable && inflightBytes_ + size > inflightLimit) break;
        if (!p.retransmit) p.seq = nextSeq_++;  // a repeat keeps the number the receiver asked for
        const auto bytes = encodeMedia(p);
        if (!send(bytes)) break;  // refused for now (the path is still opening, or TCP pushes back)
        budgetBytes_ -= size; queuedBytes_ -= size; inflightBytes_ += size;
        auto& s = sent_[p.seq];
        const auto lastRepeat = s.lastRepeatMs;  // a repeat keeps its spacing
        s = Sent{bytes, nowMs, lastRepeat, size, false, false};
        queue_.pop_front();
    }
}

void MediaSender::trimSent(qint64 nowMs) {
    for (auto it = sent_.begin(); it != sent_.end() && nowMs - it->second.sentMs > SentHistoryMs;) {
        const auto& s = it->second;
        // On a reliable path an unacknowledged packet is still queued
        // somewhere on the way, however old: it stays counted (within
        // reason), or the bound would let the queue grow unseen.
        if (options_.reliable && !s.acknowledged && !s.lost && nowMs - s.sentMs < 10 * SentHistoryMs) { ++it; continue; }
        if (!s.acknowledged && !s.lost) inflightBytes_ -= s.size;
        it = sent_.erase(it);
    }
    inflightBytes_ = std::max<qint64>(0, inflightBytes_);
}

void MediaSender::onFeedback(const QByteArray& packet, qint64 nowMs) {
    const auto feedback = Feedback::decode(packet);
    if (!feedback || feedback->session != options_.session) return;
    const auto& f = *feedback;
    QVector<BandwidthEstimator::PacketResult> results;
    qint64 newestSentMs = -1;
    for (int i = 0; i < f.arrivals.size(); ++i) {
        const quint32 seq = f.baseSeq + quint32(i);
        auto it = sent_.find(seq);
        if (it == sent_.end()) continue;
        auto& s = it->second;
        if (s.acknowledged) continue;
        if (f.arrivals[i] == Feedback::Missing) {
            if (!s.lost) { s.lost = true; inflightBytes_ -= s.size; }
            results.push_back({s.sentMs, 0, s.size, false});
        } else {
            if (!s.lost) inflightBytes_ -= s.size;
            s.acknowledged = true; s.lost = false;
            const double arrival = double(f.refTimeMs) + f.arrivals[i] / 4.;
            results.push_back({s.sentMs, arrival, s.size, true});
            newestSentMs = std::max(newestSentMs, s.sentMs);
        }
    }
    inflightBytes_ = std::max<qint64>(0, inflightBytes_);
    // Round trip: from the newest reported packet leaving to its feedback
    // arriving, less the time the receiver held the feedback.
    if (newestSentMs >= 0) {
        const double rtt = std::max(1., double(nowMs - newestSentMs) - double(f.heldMs));
        rttMs_ = rttMs_ > 0 ? 0.8 * rttMs_ + 0.2 * rtt : rtt;
        minRttMs_ = minRttMs_ > 0 ? std::min(minRttMs_, rtt) : rtt;
    }
    std::sort(results.begin(), results.end(), [](const auto& a, const auto& b) { return a.sendMs < b.sendMs; });
    if (!results.isEmpty()) bwe_.onFeedback(results, nowMs, rttMs_);
    // On a reliable path the bound on what is in flight is what limits
    // sending when the path cannot drain it: the rate follows what arrives.
    if (options_.reliable && !results.isEmpty() && inflightBytes_ >= 0.9 * double(inflightLimit()) && bwe_.receiveBps() > 0) {
        bwe_.limitTo(int(0.85 * bwe_.receiveBps()));
    }
    // Repeats: what it still wants, once per 40 ms each, while it is recent.
    for (const quint32 seq : f.nacks) {
        auto it = sent_.find(seq);
        if (it == sent_.end() || nowMs - it->second.lastRepeatMs < RepeatSpacingMs) continue;
        it->second.lastRepeatMs = nowMs; it->second.lost = true;
        // Repeats go out ahead of new packets, paced with them.
        if (auto again = decodeMedia(it->second.bytes)) {
            again->retransmit = true; queue_.push_front(*again); queuedBytes_ += it->second.size; ++retransmitted_;
        }
    }
    // Lag: the newest frame it has against the newest queued, on this clock.
    if (f.haveLatest) lagMs_ = std::max(0, int(qint32(lastSentCaptureMs_ - f.latestCaptureMs)));
    if (lagMs_ > BehindMs && nowMs - behindAt_ > 1500) { behindAt_ = nowMs; dropQueued(); }
    if (f.keyframeWanted) awaitKey_ = true;
}

// The next frame encoded should be a keyframe: the receiver asked for one
// (it lost a reference), or the queue was dropped. Not more often than a
// couple of round trips, so one request answered on its way is not answered
// twice; each costs several frames' worth of bytes.
bool MediaSender::takeKeyframeWanted(qint64 nowMs) {
    if (!awaitKey_) return false;
    if (nowMs - lastKeyMs_ < std::max(150., rttMs_)) return false;
    lastKeyMs_ = nowMs;
    return true;
}

void MediaSender::dropQueued() {
    for (const auto& p : queue_) if (!p.retransmit) ++dropped_;
    queue_.clear(); queuedBytes_ = 0;
    awaitKey_ = true;
}

}  // namespace ucd::screen
