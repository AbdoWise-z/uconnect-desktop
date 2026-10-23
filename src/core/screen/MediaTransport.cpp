#include "MediaTransport.hpp"

#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <set>

namespace ucd::screen {
namespace {
template<class T> T read(const QByteArray& b, int at) {
    return qFromBigEndian<T>(reinterpret_cast<const uchar*>(b.constData() + at));
}
template<class T> void write(QByteArray& b, int at, T value) {
    qToBigEndian<T>(value, reinterpret_cast<uchar*>(b.data() + at));
}
bool newer(quint32 a, quint32 b) { return qint32(a - b) > 0; }
}

QVector<QByteArray> fragment(const EncodedFrame& frame) {
    QVector<QByteArray> result;
    if (!frame.session || frame.bytes.isEmpty() || frame.bytes.size() > MaxFrameBytes) return result;
    const int count = (frame.bytes.size() + FragmentPayload - 1) / FragmentPayload;
    result.reserve(count);
    for (int index = 0; index < count; ++index) {
        QByteArray packet(FragmentHeader, '\0');
        packet[0] = 1; packet[1] = frame.keyframe ? 1 : 0;
        write(packet, 2, frame.session); write(packet, 4, frame.number);
        write(packet, 8, quint16(index)); write(packet, 10, quint16(count));
        write(packet, 12, frame.captureMs);
        packet += frame.bytes.mid(index * FragmentPayload, FragmentPayload);
        result.push_back(std::move(packet));
    }
    return result;
}

bool PacedSender::enqueue(const EncodedFrame& frame, qint64 nowMs, int intervalMs, double spread, int paceBps) {
    if (int(queue_.size()) >= MaxQueuedFrames) { ++dropped_; return false; }
    Queued queued{fragment(frame), 0, nowMs, nowMs, nowMs};
    if (queued.packets.isEmpty()) return false;
    interval_ = std::clamp(intervalMs, 1000 / MaxFps, 1000);
    queued.start = queue_.empty() ? nowMs : std::max(nowMs, queue_.back().due);
    int duration = std::max(spread < 0.5 ? 2 : 4, int(interval_ * std::clamp(spread, 0.1, 1.0)));
    // A big frame (a keyframe) no faster than paceBps: sent in a few
    // milliseconds it is a burst Wi-Fi drops, which asks for another keyframe.
    // At most 60 ms, so the frames behind it are not held up past their use.
    if (paceBps > 0) duration = std::max(duration, int(std::min<qint64>(60, qint64(frame.bytes.size()) * 8000 / paceBps)));
    queued.due   = queued.start + duration;
    while (!history_.empty() && (history_.front().expires < nowMs || history_.size() >= 16)) history_.pop_front();
    history_.push_back({frame.number, queued.packets, nowMs + 500});
    queue_.push_back(std::move(queued));
    return true;
}

bool PacedSender::pump(qint64 nowMs, const std::function<bool(const QByteArray&)>& send) {
    if (queue_.empty()) return true;
    // Too late to be worth showing: drop it and everything queued behind it
    // (which depends on it). The caller asks the encoder for a keyframe.
    const qint64 budget = std::max<qint64>(150, qint64(interval_) * 4);
    if (nowMs - queue_.front().enqueued > budget) {
        dropped_ += queue_.size(); queue_.clear();
        return false;
    }
    while (!queue_.empty()) {
        auto& front = queue_.front();
        if (nowMs < front.start) break;
        // How many of its datagrams should be out by now; the first few go at once.
        const qint64 size = front.packets.size();
        const qint64 span = std::max<qint64>(1, front.due - front.start);
        const qint64 target = std::min(size, std::max(std::min<qint64>(4, size),
                                                      (size * (nowMs - front.start) + span - 1) / span));
        while (front.next < target) {
            if (!send(front.packets[front.next])) return true;  // refused for now: retry next pump
            ++front.next;
        }
        if (front.next < size) break;
        queue_.pop_front();
    }
    return true;
}

QVector<QByteArray> PacedSender::retransmit(quint32 frame, const QVector<int>& indexes, qint64 nowMs) const {
    QVector<QByteArray> result;
    if (indexes.size() > MaxFragments) return result;
    const auto sent = std::find_if(history_.begin(), history_.end(), [&](const Sent& s) { return s.number == frame; });
    if (sent == history_.end() || nowMs > sent->expires) return result;
    std::set<int> seen;
    for (int index : indexes) {
        if (index < 0 || index >= sent->packets.size() || !seen.insert(index).second) continue;
        auto packet = sent->packets[index]; packet[1] = char(quint8(packet[1]) | 2);
        result.push_back(std::move(packet));
    }
    return result;
}

void PacedSender::reset() { queue_.clear(); history_.clear(); dropped_ = 0; }

QJsonObject Feedback::json() const {
    return {{"received", double(received)}, {"lost", double(lost)}, {"late", double(late)},
        {"jitterMs", jitterMs}, {"owdTrendMs", owdTrendMs}, {"decodeMs", decodeMs}};
}

std::optional<Feedback> Feedback::parse(const QJsonObject& message) {
    for (const char* key : {"received", "lost", "late", "jitterMs", "owdTrendMs", "decodeMs"}) {
        const auto value = message.value(QLatin1String(key));
        if (!value.isDouble() || !std::isfinite(value.toDouble()) || std::abs(value.toDouble()) > 10000000)
            return std::nullopt;
        if (QLatin1String(key) != QLatin1String("owdTrendMs") && value.toDouble() < 0) return std::nullopt;
    }
    return Feedback{quint64(message.value("received").toDouble()), quint64(message.value("lost").toDouble()),
        quint64(message.value("late").toDouble()), message.value("jitterMs").toDouble(),
        message.value("owdTrendMs").toDouble(), message.value("decodeMs").toDouble()};
}

void Reassembler::reset(quint16 session, int fps) {
    session_ = session; interval_ = 1000 / std::clamp(fps, 1, MaxFps);
    frames_.clear(); buffered_ = 0; waiting_ = true; haveLast_ = false; requestKey_ = false;
    stats_ = {}; previousTransit_.reset(); trend_ = 0; lastKeyRequest_ = -1000;
}

void Reassembler::erase(std::map<quint32, Assembly>::iterator it) {
    for (const auto& part : it->second.parts) buffered_ -= part.size();
    frames_.erase(it);
}

QVector<EncodedFrame> Reassembler::receive(const QByteArray& packet, qint64 nowMs) {
    if (!session_ || packet.size() <= FragmentHeader || packet.size() > 1100 || packet[0] != 1 ||
        (quint8(packet[1]) & ~3) || read<quint16>(packet, 2) != session_) return {};
    const auto number = read<quint32>(packet, 4);
    const int index = read<quint16>(packet, 8), count = read<quint16>(packet, 10);
    if (!count || count > MaxFragments || index >= count ||
        (index + 1 < count && packet.size() != 1100)) return {};
    if (haveLast_ && !newer(number, last_)) { ++stats_.late; return {}; }
    if (!frames_.contains(number)) {
        if (frames_.size() >= (reliable_ ? 64u : patient_ ? 16u : 8u) || buffered_ + packet.size() > MaxFrameBytes * 2) {
            requestKey_ = true; return {};
        }
        Assembly assembly;
        assembly.frame = {session_, number, read<quint32>(packet, 12), bool(packet[1] & 1), {}};
        assembly.parts.resize(count);
        assembly.deadline = nowMs + (reliable_ ? ReliableWaitMs : 2 * interval_ + qint64(std::clamp(stats_.jitterMs, 0., 60.)));
        frames_.emplace(number, std::move(assembly));
    }
    auto it = frames_.find(number);
    auto& assembly = it->second;
    if (assembly.parts.size() != count || assembly.frame.keyframe != bool(packet[1] & 1) ||
        assembly.frame.captureMs != read<quint32>(packet, 12)) {
        erase(it); waiting_ = true; requestKey_ = true; return {};
    }
    if (!assembly.parts[index].isEmpty()) return {};  // duplicate is not loss/receive feedback
    if (buffered_ + packet.size() - FragmentHeader > MaxFrameBytes * 2) {
        erase(it); waiting_ = true; requestKey_ = true; return {};
    }
    assembly.parts[index] = packet.mid(FragmentHeader);
    buffered_ += assembly.parts[index].size(); ++assembly.received; ++stats_.received;
    // Still arriving is not late: a sender may spread a big frame (a phone's
    // keyframe, paced for its uplink) over several frame intervals.
    assembly.deadline = std::max(assembly.deadline, nowMs + (reliable_ ? ReliableWaitMs : 2 * interval_ + qint64(std::clamp(stats_.jitterMs, 0., 60.))));
    if (assembly.received == count) {
        assembly.complete = true;
        // Clock offset cancels in differences. Timestamp wrap is handled in
        // unsigned 32-bit arithmetic; this is a trend, not absolute latency.
        const double transit = qint32(quint32(nowMs) - assembly.frame.captureMs);
        if (previousTransit_) {
            const double delta = transit - *previousTransit_;
            stats_.jitterMs += (std::abs(delta) - stats_.jitterMs) / 16;
            trend_ += (delta - trend_) / 8;
        }
        previousTransit_ = transit;
    }
    return deliver();
}

QVector<EncodedFrame> Reassembler::deliver() {
    QVector<EncodedFrame> result;
    // A complete keyframe can safely skip earlier incomplete references.
    for (auto it = frames_.begin(); it != frames_.end(); ++it) {
        if (it->second.complete && it->second.frame.keyframe && (!haveLast_ || newer(it->first, last_))) {
            last_ = it->first - 1; haveLast_ = true; waiting_ = false; requestKey_ = false;
            break;
        }
    }
    if (waiting_ || !haveLast_) return result;
    while (true) {
        auto it = frames_.find(last_ + 1);
        if (it == frames_.end() || !it->second.complete) break;
        auto frame = it->second.frame;
        for (const auto& part : it->second.parts) frame.bytes += part;
        if (frame.bytes.size() > MaxFrameBytes) { waiting_ = true; requestKey_ = true; erase(it); break; }
        last_ = frame.number;
        result.push_back(std::move(frame)); erase(it);
    }
    for (auto it = frames_.begin(); it != frames_.end();) {
        if (!newer(it->first, last_)) { auto old = it++; erase(old); } else ++it;
    }
    return result;
}

Recovery Reassembler::expire(qint64 nowMs) {
    Recovery recovery;
    for (auto it = frames_.begin(); it != frames_.end();) {
        auto& assembly = it->second;
        if (nowMs < assembly.deadline) { ++it; continue; }
        // Ask once for what is missing, unless it could not be decoded anyway
        // (a delta while waiting for a keyframe).
        if (!reliable_ && !assembly.complete && assembly.nacks < (patient_ ? 3 : 1) && (assembly.frame.keyframe || !waiting_)) {
            Recovery::Nack nack{it->first, {}};
            for (int i = 0; i < assembly.parts.size(); ++i) if (assembly.parts[i].isEmpty()) nack.indexes.push_back(i);
            recovery.nacks.push_back(std::move(nack));
            ++assembly.nacks; assembly.deadline = nowMs + std::max(patient_ ? 50 : 30, interval_);
            ++it; continue;
        }
        stats_.lost += std::max(1, int(assembly.parts.size()) - assembly.received);
        ++stats_.late; waiting_ = true; requestKey_ = true;
        auto old = it++; erase(old);
    }
    if (requestKey_ && nowMs - lastKeyRequest_ >= 200) {
        recovery.keyframe = true; lastKeyRequest_ = nowMs;
    }
    return recovery;
}

Feedback Reassembler::report(double decodeMs) {
    Feedback result = stats_;
    result.owdTrendMs = trend_; result.decodeMs = std::clamp(decodeMs, 0., 10000.);
    stats_.received = stats_.lost = stats_.late = 0;
    return result;
}

bool LossyPath::report(quint64 received, quint64 lost) {
    if (received + lost == 0) return false;
    recent_.push_back(double(lost) / double(received + lost) > Threshold);
    while (int(recent_.size()) > Window) recent_.pop_front();
    return int(std::count(recent_.begin(), recent_.end(), true)) >= Needed;
}

void CongestionController::reset(const QString& path, int maxFps, int maxWidth, int maxBitrate, int startBitrate) {
    cap_ = std::clamp(maxBitrate, MinBitrate, MaxBitrate);
    maxFps_ = fps_ = std::clamp(maxFps, 1, MaxFps);
    maxWidth_ = width_ = std::clamp(maxWidth, 320, 3840);
    const int start = startBitrate > 0 && path == "direct" ? startBitrate
        : path == "direct" ? 8000000 : path == "relayed" ? 3000000 : 2000000;
    bitrate_ = std::min(cap_, start);
    bad_ = good_ = slowDecode_ = cooldown_ = congestedStreak_ = 0;
}

void CongestionController::update(const Feedback& report) {
    if (cooldown_ > 0) --cooldown_;
    // Decoding slower than frames arrive: fewer frames, same quality.
    if (report.decodeMs > 1000. / fps_ * 1.2) {
        // A burst of frames decoded together can look slow for a report or two.
        if (++slowDecode_ >= (tolerant_ ? 6 : 3) && fps_ > 5) { fps_ = std::max(5, fps_ * 3 / 4); slowDecode_ = 0; }
    } else {
        slowDecode_ = 0;
    }
    const double loss = double(report.lost) / std::max(1., double(report.received + report.lost));
    // Tolerant (extended displays, nearby on Wi-Fi): short bursts of loss and
    // delay are the radio, not a full link, and every cut costs a hardware
    // encoder a keyframe; only sustained trouble lowers the bitrate.
    const bool congested = tolerant_ ? loss > .10 || report.owdTrendMs > 30 : loss > .05 || report.owdTrendMs > 15;
    if (congested) {
        good_ = 0;
        if (tolerant_ && ++congestedStreak_ < 2) return;
        congestedStreak_ = 0;
        if (cooldown_ > 0) return;  // already eased off for this patch
        bitrate_ = std::max(MinBitrate, int(bitrate_ * .8)); cooldown_ = 3;
        if (++bad_ >= 3 && bitrate_ <= MinBitrate * 2) {
            if (fps_ > 15) fps_ = std::max(15, fps_ - 5);
            else width_ = std::max(320, width_ * 3 / 4);
            bad_ = 0;
        }
        return;
    }
    bad_ = 0; ++good_; congestedStreak_ = 0;
    if (report.received > 0) bitrate_ = std::min(cap_, bitrate_ + std::max(100000, bitrate_ / 12));
    if (good_ >= 10) {
        width_ = std::min(maxWidth_, width_ + 160);
        if (slowDecode_ == 0) fps_ = std::min(maxFps_, fps_ + 5);
        good_ = 0;
    }
}

}  // namespace ucd::screen
