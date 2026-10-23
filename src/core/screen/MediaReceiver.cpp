#include "MediaReceiver.hpp"

#include <QtEndian>
#include <algorithm>

namespace ucd::screen {
namespace {
constexpr int MaxPendingFrames = 32;
constexpr qint64 NackAfterMs = 10;       // a gap this old is asked for
constexpr qint64 MissingForgetMs = 2000;
}  // namespace

void MediaReceiver::reset(quint16 session, quint8 kind, int fps) {
    session_ = session; kind_ = kind; interval_ = 1000 / std::clamp(fps, 1, MaxFps);
    waiting_ = true; haveLast_ = false; keyWanted_ = false; last_ = 0;
    frames_.clear(); arrivals_.clear(); haveSeq_ = false; highestSeq_ = feedbackBase_ = 0; missing_.clear(); haveFloor_ = false; nackFloor_ = 0;
    lastFeedbackMs_ = newestArrivalMs_ = 0; stats_ = {}; latestCaptureMs_.reset(); latestFrame_ = 0;
    jitterMs_ = trendMs_ = 0; previousTransit_.reset();
}

// A repair packet rebuilds one missing fragment of its group once every
// other fragment of the group is in.
bool MediaReceiver::Assembly::tryRepair() {
    bool any = false;
    for (auto it = repairs.begin(); it != repairs.end();) {
        const int start = it->first, size = it->second.size;
        int missing = -1, missingCount = 0;
        for (int i = start; i < std::min(start + size, count); ++i) if (!have[i]) { missing = i; ++missingCount; }
        if (missingCount == 0) { it = repairs.erase(it); continue; }
        if (missingCount > 1) { ++it; continue; }
        const auto& payload = it->second.payload;
        quint16 length = qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(payload.constData() + 1));
        QByteArray data = payload.mid(FecHeader);
        for (int i = start; i < std::min(start + size, count); ++i) {
            if (i == missing) continue;
            length ^= quint16(parts[i].size());
            for (int k = 0; k < parts[i].size() && k < data.size(); ++k) data[k] = char(quint8(data[k]) ^ quint8(parts[i][k]));
        }
        if (length > data.size()) { it = repairs.erase(it); continue; }
        parts[missing] = data.left(length); have[missing] = true; ++got; any = true;
        it = repairs.erase(it);
    }
    return any;
}

void MediaReceiver::noteArrival(quint32 seq, qint64 nowMs) {
    arrivals_[seq] = nowMs; newestArrivalMs_ = nowMs;
    if (!haveSeq_) { haveSeq_ = true; highestSeq_ = seq; feedbackBase_ = seq; return; }
    if (seqNewer(seq, highestSeq_)) {
        // Everything between is missing until it shows up.
        for (quint32 s = highestSeq_ + 1; seqNewer(seq, s) && missing_.size() < 1000; ++s) missing_[s] = nowMs;
        highestSeq_ = seq;
    } else {
        missing_.erase(seq);
    }
}

QVector<EncodedFrame> MediaReceiver::receive(const QByteArray& packet, qint64 nowMs) {
    const auto p = decodeMedia(packet);
    if (!p || p->session != session_ || p->kind != kind_) return {};
    noteArrival(p->seq, nowMs);
    ++stats_.received;
    if (p->retransmit) ++stats_.repaired;
    if (haveLast_ && !seqNewer(p->frame, last_)) { ++stats_.late; return {}; }
    auto it = frames_.find(p->frame);
    if (it == frames_.end()) {
        if (waiting_ && !p->keyframe) { ++stats_.discarded; return {}; }  // useless until a keyframe
        if (frames_.size() >= MaxPendingFrames) { give(frames_.begin()); waiting_ = true; keyWanted_ = true; }
        Assembly a;
        a.frame = {session_, p->frame, p->captureMs, p->keyframe, {}};
        a.count = p->count; a.parts.resize(p->count); a.have.resize(p->count); a.firstMs = a.lastMs = nowMs;
        a.lowSeq = a.highSeq = p->seq;
        it = frames_.emplace(p->frame, std::move(a)).first;
    }
    auto& a = it->second;
    if (a.count != p->count) return {};  // not the same frame
    a.lastMs = nowMs;
    if (seqNewer(a.lowSeq, p->seq)) a.lowSeq = p->seq;
    if (seqNewer(p->seq, a.highSeq)) a.highSeq = p->seq;
    if (p->type == PacketType::Fec) {
        a.repairs[p->frag] = {quint8(p->payload[0]), p->payload};
    } else if (!a.have[p->frag]) {
        a.parts[p->frag] = p->payload; a.have[p->frag] = true; ++a.got;
    }
    if (!a.complete() && !a.repairs.empty() && a.tryRepair()) ++stats_.recovered;
    if (a.complete() && a.frame.bytes.isEmpty()) {
        for (const auto& part : a.parts) a.frame.bytes += part;
        // Whole: whatever of it is still missing (rebuilt from repair
        // packets) need not be sent again.
        for (auto m = missing_.lower_bound(a.lowSeq); m != missing_.end() && !seqNewer(m->first, a.highSeq);) m = missing_.erase(m);
        // Transit (arrival minus capture, clocks mixed): its variation is the
        // jitter, its drift a queue building on the path.
        const double transit = double(qint32(quint32(nowMs) - a.frame.captureMs));
        if (previousTransit_) {
            const double delta = transit - *previousTransit_;
            jitterMs_ += (std::abs(delta) - jitterMs_) / 16; trendMs_ += (delta - trendMs_) / 8;
        }
        previousTransit_ = transit;
    }
    return deliver();
}

QVector<EncodedFrame> MediaReceiver::deliver() {
    QVector<EncodedFrame> out;
    // A complete keyframe may skip whatever is incomplete before it.
    for (auto it = frames_.begin(); it != frames_.end(); ++it) {
        if (it->second.complete() && it->second.frame.keyframe && (!haveLast_ || seqNewer(it->first, last_))) {
            last_ = it->first - 1; haveLast_ = true; waiting_ = false; keyWanted_ = false;
            break;
        }
    }
    if (waiting_ || !haveLast_) return out;
    while (true) {
        auto it = frames_.find(last_ + 1);
        if (it == frames_.end() || !it->second.complete()) break;
        last_ = it->first;
        latestCaptureMs_ = it->second.frame.captureMs; latestFrame_ = it->first;
        if (!haveFloor_ || seqNewer(it->second.highSeq, nackFloor_)) { nackFloor_ = it->second.highSeq; haveFloor_ = true; }
        out.push_back(std::move(it->second.frame));
        frames_.erase(it);
    }
    for (auto it = frames_.begin(); it != frames_.end();) {
        if (!seqNewer(it->first, last_)) { auto old = it++; frames_.erase(old); } else ++it;
    }
    return out;
}

void MediaReceiver::give(std::map<quint32, Assembly>::iterator it) {
    ++stats_.givenUp; stats_.lost += std::max(0, it->second.count - it->second.got);
    const auto& a = it->second;
    if (giveLog.size() < 12) {
        int nacked = 0; for (const auto& [seq, since] : missing_) if (!seqNewer(a.lowSeq, seq) && !seqNewer(seq, a.highSeq)) ++nacked;
        giveLog << QStringLiteral("frame %1 %2/%3%4 age %5 idle %6 missing %7 pending %8").arg(it->first).arg(a.got).arg(a.count).arg(a.frame.keyframe ? "k" : "")
                       .arg(a.lastMs - a.firstMs).arg(newestArrivalMs_ - a.lastMs).arg(nacked).arg(frames_.size());
    }
    for (auto m = missing_.lower_bound(a.lowSeq); m != missing_.end() && !seqNewer(m->first, a.highSeq);) m = missing_.erase(m);
    frames_.erase(it);
}

void MediaReceiver::expire(qint64 nowMs) {
    // A frame that has made no progress for a while (repeats had their
    // chance: several round trips) is given up with those behind it that
    // depend on it; the stream restarts at the next keyframe. A big frame
    // still arriving is not stuck, however long it takes.
    const qint64 giveUpMs = std::max<qint64>(400, 8 * interval_);
    for (auto it = frames_.begin(); it != frames_.end();) {
        if (it->second.complete() || nowMs - it->second.lastMs < giveUpMs) { ++it; continue; }
        give(it++);
        waiting_ = true; keyWanted_ = true;
    }
    for (auto it = missing_.begin(); it != missing_.end();) {
        if (nowMs - it->second > MissingForgetMs) it = missing_.erase(it); else ++it;
    }
}

bool MediaReceiver::feedbackDue(qint64 nowMs) const {
    if (!haveSeq_) return false;
    // Sooner when a keyframe is wanted, but not every packet: the sender
    // reads its send rate from each feedback's window.
    return nowMs - lastFeedbackMs_ >= FeedbackIntervalMs || qint32(highestSeq_ - feedbackBase_) >= Feedback::MaxArrivals - 20 ||
           (keyWanted_ && nowMs - lastFeedbackMs_ >= FeedbackIntervalMs / 2);
}

QByteArray MediaReceiver::takeFeedback(qint64 nowMs, int playoutMs, double decodeMs) {
    if (!haveSeq_) return {};
    Feedback f;
    f.session = session_; f.baseSeq = feedbackBase_;
    // Arrival times are quarter milliseconds after a reference 16 s ago,
    // so anything from the last 16 s fits in 16 bits.
    const qint64 ref = nowMs - 16000;
    f.refTimeMs = quint32(ref);
    f.heldMs = quint16(std::clamp<qint64>(nowMs - newestArrivalMs_, 0, 65535));
    f.latestFrame = latestFrame_; f.latestCaptureMs = latestCaptureMs_.value_or(0); f.haveLatest = latestCaptureMs_.has_value();
    f.playoutMs = quint16(std::clamp(playoutMs, 0, 65535)); f.decodeMs10 = quint16(std::clamp(int(decodeMs * 10), 0, 65535));
    // A keyframe is wanted unless one is already arriving (a packet of it in
    // the last 100 ms): asking again would only bring a second one.
    bool keyArriving = false;
    for (const auto& [number, a] : frames_) if (a.frame.keyframe && !a.complete() && nowMs - a.lastMs < 100) keyArriving = true;
    f.keyframeWanted = keyWanted_ && !keyArriving;
    // Every sequence from the base to the highest seen: when it arrived, or
    // that it has not (the sender counts those lost, and sends again the ones
    // asked for below).
    const int span = std::min(int(qint32(highestSeq_ - feedbackBase_)) + 1, Feedback::MaxArrivals);
    for (int i = 0; i < span; ++i) {
        const quint32 seq = feedbackBase_ + quint32(i);
        const auto at = arrivals_.find(seq);
        f.arrivals.push_back(at == arrivals_.end() ? Feedback::Missing : quint16(std::clamp<qint64>((at->second - ref) * 4, 0, 65534)));
    }
    feedbackBase_ += quint32(span);
    for (auto it = arrivals_.begin(); it != arrivals_.end();) {
        if (!seqNewer(it->first, feedbackBase_ - 1)) it = arrivals_.erase(it); else ++it;
    }
    // Still missing, old enough to be a loss rather than reordering, and
    // after the last frame delivered (packets of delivered frames are not
    // wanted, and would crowd out the ones that are): asked for again.
    for (auto it = missing_.begin(); it != missing_.end();) {
        if (haveFloor_ && !seqNewer(it->first, nackFloor_)) { it = missing_.erase(it); continue; }
        if (nowMs - it->second >= NackAfterMs && f.nacks.size() < Feedback::MaxNacks) f.nacks.push_back(it->first);
        ++it;
    }
    lastFeedbackMs_ = nowMs;
    return f.encode(kind_);
}

QString MediaReceiver::pendingText() const {
    QStringList parts;
    for (const auto& [number, a] : frames_) parts << QStringLiteral("%1:%2/%3%4").arg(number).arg(a.got).arg(a.count).arg(a.frame.keyframe ? "k" : "");
    return QStringLiteral("last %1 waiting %2 [").arg(haveLast_ ? qint64(last_) : -1).arg(waiting_) + parts.join(' ') + ']';
}

MediaReceiver::Stats MediaReceiver::report() {
    const auto out = stats_; stats_ = {};
    return out;
}

}  // namespace ucd::screen
