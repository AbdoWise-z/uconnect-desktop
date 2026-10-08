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

bool PacedSender::enqueue(const EncodedFrame& frame, qint64 nowMs, int intervalMs) {
    if (busy()) { ++dropped_; return false; }
    pending_ = fragment(frame);
    if (pending_.isEmpty()) return false;
    next_ = 0; start_ = nowMs; interval_ = std::clamp(intervalMs, 8, 1000);
    deadline_ = nowMs + interval_ * 2;
    if (frame.keyframe) { keyframe_ = pending_; keyNumber_ = frame.number; keyExpires_ = nowMs + 500; }
    return true;
}

bool PacedSender::pump(qint64 nowMs, const std::function<bool(const QByteArray&)>& send) {
    if (!busy()) return true;
    if (nowMs > deadline_) { pending_.clear(); ++dropped_; return false; }
    for (int sent = 0; sent < 64 && next_ < pending_.size(); ++sent) {
        if (nowMs < start_ + qint64(next_) * interval_ / pending_.size()) break;
        if (!send(pending_[next_])) { pending_.clear(); ++dropped_; return false; }
        ++next_;
    }
    if (next_ == pending_.size()) pending_.clear();
    return true;
}

QVector<QByteArray> PacedSender::retransmit(quint32 frame, const QVector<int>& indexes, qint64 nowMs) const {
    QVector<QByteArray> result;
    if (frame != keyNumber_ || nowMs > keyExpires_ || indexes.size() > MaxFragments) return result;
    std::set<int> seen;
    for (int index : indexes) {
        if (index < 0 || index >= keyframe_.size() || !seen.insert(index).second) continue;
        auto packet = keyframe_[index]; packet[1] = char(quint8(packet[1]) | 2);
        result.push_back(std::move(packet));
    }
    return result;
}

void PacedSender::reset() { pending_.clear(); keyframe_.clear(); next_ = 0; dropped_ = 0; }

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
    session_ = session; interval_ = 1000 / std::clamp(fps, 1, 120);
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
        if (frames_.size() >= 8 || buffered_ + packet.size() > MaxFrameBytes * 2) {
            requestKey_ = true; return {};
        }
        Assembly assembly;
        assembly.frame = {session_, number, read<quint32>(packet, 12), bool(packet[1] & 1), {}};
        assembly.parts.resize(count);
        assembly.deadline = nowMs + 2 * interval_ + qint64(std::clamp(stats_.jitterMs, 0., 60.));
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
        if (!assembly.complete && assembly.frame.keyframe && !assembly.nacked) {
            Recovery::Nack nack{it->first, {}};
            for (int i = 0; i < assembly.parts.size(); ++i) if (assembly.parts[i].isEmpty()) nack.indexes.push_back(i);
            recovery.nacks.push_back(std::move(nack));
            assembly.nacked = true; assembly.deadline = nowMs + std::max(30, interval_);
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

void CongestionController::reset(const QString& path, int maxFps, int maxWidth, int maxBitrate) {
    cap_ = std::clamp(maxBitrate, 150000, 20000000);
    maxFps_ = fps_ = std::clamp(maxFps, 1, 60);
    maxWidth_ = width_ = std::clamp(maxWidth, 320, 3840);
    bitrate_ = std::min(cap_, path == "direct" ? 4000000 : path == "relayed" ? 1500000 : 1000000);
    bad_ = good_ = 0;
}

void CongestionController::update(const Feedback& report) {
    const double loss = double(report.lost) / std::max(1., double(report.received + report.lost));
    if (loss > .02 || report.owdTrendMs > 8 || report.decodeMs > 1000. / fps_ * 1.5) {
        bitrate_ = std::max(150000, int(bitrate_ * .7)); ++bad_; good_ = 0;
        if (bad_ >= 3 && bitrate_ <= 300000) {
            if (fps_ > 10) fps_ = std::max(10, fps_ - 5);
            else width_ = std::max(320, width_ * 3 / 4);
            bad_ = 0;
        }
    } else {
        bad_ = 0; ++good_;
        if (report.received > 0) bitrate_ = std::min(cap_, bitrate_ + std::max(25000, bitrate_ / 30));
        if (good_ >= 10 && bitrate_ > 1000000) {
            width_ = std::min(maxWidth_, width_ + 160);
            fps_ = std::min(maxFps_, fps_ + 5); good_ = 0;
        }
    }
}

}  // namespace ucd::screen
