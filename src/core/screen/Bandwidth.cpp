#include "Bandwidth.hpp"

#include <algorithm>
#include <cmath>

namespace ucd::screen {
namespace {
constexpr int TrendWindow = 20;        // samples the trend line is fitted through
constexpr double TrendGain = 4.0;      // the slope, scaled to compare with the threshold
constexpr double OveruseHoldMs = 100;  // the trend must stay above the threshold this long (a cellular
                                       // link delivers in bursts, which read as a sawtooth of delay)
constexpr double ReceiveWindowMs = 1000;
constexpr double MaxTargetOverReceived = 1.5;  // never ask for much more than the path has shown it takes
constexpr qint64 BurstMs = 5;                  // packets sent this close together are one group
}  // namespace

void BandwidthEstimator::reset(const Options& options) {
    options_ = options;
    targetBps_ = std::clamp(options.startBps, options.minBps, options.maxBps);
    receiveBps_ = 0; sendBps_ = 0; lastDecreaseBps_ = 0; loss_ = 0; burstLoss_ = 0; trend_ = 0; threshold_ = OveruseThresholdMs;
    state_ = State::Increase; lastUpdateMs_ = 0; overusingSinceMs_ = -1; lastLossCutMs_ = -1000000; previousTrend_ = 0;
    accumulatedDelayMs_ = smoothedDelayMs_ = 0; samples_.clear(); deltas_ = 0; haveSample_ = false; firstArrivalMs_ = 0;
    previousSendMs_ = 0; previousArrivalMs_ = 0; received_.clear(); lastArrivalMs_ = 0; sent_.clear(); lastSendMs_ = 0; minRttMs_ = 0;
    groupStartSendMs_ = groupSendMs_ = 0; groupArrivalMs_ = 0; haveGroup_ = false;
}

void BandwidthEstimator::limitTo(int bps) {
    const int target = std::clamp(bps, options_.minBps, options_.maxBps);
    if (target >= targetBps_) return;
    targetBps_ = target; lastDecreaseBps_ = target; state_ = State::Decrease;
}

QString BandwidthEstimator::state() const {
    return state_ == State::Hold ? QStringLiteral("hold") : state_ == State::Increase ? QStringLiteral("increase") : QStringLiteral("decrease");
}

// The trend of the accumulated delay variation against the threshold, the
// threshold itself following the trend so a path with a lot of normal
// variation does not read as overuse all the time.
BandwidthEstimator::Signal BandwidthEstimator::detect(double trend, double dtMs) {
    const double k = std::abs(trend) < threshold_ ? 0.039 : 0.0087;
    threshold_ = std::clamp(threshold_ + k * (std::abs(trend) - threshold_) * std::clamp(dtMs, 0., 100.), 6., 600.);
    return trend > threshold_ ? Signal::Overuse : trend < -threshold_ ? Signal::Underuse : Signal::Normal;
}

void BandwidthEstimator::onFeedback(const QVector<PacketResult>& results, qint64 nowMs, double rttMs, double lagMs) {
    int lost = 0, got = 0, bytes = 0;
    for (const auto& r : results) {
        sent_.emplace_back(r.sendMs, r.bytes); lastSendMs_ = std::max(lastSendMs_, r.sendMs);
        if (!r.received) { ++lost; continue; }
        ++got; bytes += r.bytes;
        // The receiver's clock jumped (its reference wrapped or restarted):
        // nothing before compares with what follows.
        if (haveSample_ && std::abs(r.arrivalMs - groupArrivalMs_) > 3600000.) {
            haveSample_ = false; haveGroup_ = false; samples_.clear(); deltas_ = 0; received_.clear(); lastArrivalMs_ = 0;
            accumulatedDelayMs_ = smoothedDelayMs_ = 0;
        }
        received_.emplace_back(r.arrivalMs, r.bytes);
        lastArrivalMs_ = std::max(lastArrivalMs_, r.arrivalMs);
        if (!haveSample_) {
            haveSample_ = true; firstArrivalMs_ = r.arrivalMs;
            groupStartSendMs_ = groupSendMs_ = r.sendMs; groupArrivalMs_ = r.arrivalMs; haveGroup_ = false;
            continue;
        }
        // Packets sent within a few milliseconds of each other are one group
        // (a burst leaves together, and a link that delivers in bursts hands
        // it over together): the delay deltas are between groups, as in GCC,
        // or every burst reads as delay building and then falling.
        if (r.sendMs - groupStartSendMs_ <= BurstMs) {
            groupSendMs_ = r.sendMs; groupArrivalMs_ = std::max(groupArrivalMs_, r.arrivalMs);
            continue;
        }
        if (haveGroup_) {
            // Arrivals spreading out more than departures: delay is building.
            const double delta = (groupArrivalMs_ - previousArrivalMs_) - double(groupSendMs_ - previousSendMs_);
            accumulatedDelayMs_ += delta;
            smoothedDelayMs_ = 0.9 * smoothedDelayMs_ + 0.1 * accumulatedDelayMs_;
            samples_.emplace_back(groupArrivalMs_ - firstArrivalMs_, smoothedDelayMs_);
            while (int(samples_.size()) > TrendWindow) samples_.pop_front();
            deltas_ = std::min(deltas_ + 1, 60);
        }
        previousSendMs_ = groupSendMs_; previousArrivalMs_ = groupArrivalMs_; haveGroup_ = true;
        groupStartSendMs_ = groupSendMs_ = r.sendMs; groupArrivalMs_ = r.arrivalMs;
    }
    if (got + lost > 0) loss_ = 0.8 * loss_ + 0.2 * double(lost) / double(got + lost);
    // The receive rate over the last second of arrivals. A link that
    // delivers in bursts (cellular) packs a window's packets into a few
    // milliseconds: the span is never read shorter than a quarter second,
    // and a window that has filled counts as the whole of it.
    while (!received_.empty() && received_.front().first < lastArrivalMs_ - ReceiveWindowMs) received_.pop_front();
    if (received_.size() >= 2) {
        qint64 total = 0; for (const auto& [at, size] : received_) total += size;
        const double covered = lastArrivalMs_ - received_.front().first;
        const double span = covered > ReceiveWindowMs - 200 ? ReceiveWindowMs : std::max(250., covered);
        receiveBps_ = int(total * 8000. / span);
    }
    // Trend line through the recent samples: its slope is the signal.
    double slope = 0;
    if (samples_.size() >= 2) {
        double sumX = 0, sumY = 0;
        for (const auto& [x, y] : samples_) { sumX += x; sumY += y; }
        const double meanX = sumX / double(samples_.size()), meanY = sumY / double(samples_.size());
        double num = 0, den = 0;
        for (const auto& [x, y] : samples_) { num += (x - meanX) * (y - meanY); den += (x - meanX) * (x - meanX); }
        if (den > 0) slope = num / den;
    }
    // Weighted by how much has been seen (up to 60 deltas): a few samples
    // say little.
    trend_ = double(deltas_) * slope * TrendGain;
    const double dt = lastUpdateMs_ ? double(nowMs - lastUpdateMs_) : 100.;
    lastUpdateMs_ = nowMs;
    // The worst recent spell, halving every half second; a handful of
    // packets is too few to call a spell.
    burstLoss_ *= std::pow(0.5, dt / 500.);
    if (got + lost >= 10) burstLoss_ = std::max(burstLoss_, double(lost) / double(got + lost));
    Signal signal = detect(trend_, dt);
    if (signal == Signal::Overuse) {
        if (overusingSinceMs_ < 0) overusingSinceMs_ = nowMs;
        if (nowMs - overusingSinceMs_ < OveruseHoldMs || trend_ < previousTrend_) signal = Signal::Normal;  // not yet: a blip
    } else {
        overusingSinceMs_ = -1;
    }
    // A queue building shows as the receiver falling behind live. Within a
    // tenth of a second of live, a rising trend is the link's own unevenness
    // (a cellular link delivers in bursts), not a queue of ours.
    if (signal == Signal::Overuse && lagMs >= 0 && lagMs < 100) signal = Signal::Normal;
    previousTrend_ = trend_;

    // Rate control: overuse cuts to a little under what arrives; otherwise
    // the rate grows, by 8% a second while far from where it was last cut,
    // by about a packet per round trip near it.
    const double seconds = std::clamp(dt, 1., 1000.) / 1000.;
    // Sending well under the target (a still screen): the path is not being
    // tested, so neither the receive rate nor the lack of trouble says
    // anything about it; the target holds.
    while (!sent_.empty() && sent_.front().first < lastSendMs_ - qint64(ReceiveWindowMs)) sent_.pop_front();
    if (sent_.size() >= 2) {
        qint64 total = 0; for (const auto& [at, size] : sent_) total += size;
        sendBps_ = int(total * 8000. / std::max<qint64>(50, lastSendMs_ - sent_.front().first));
    }
    const bool limitedByApp = sendBps_ < 0.7 * targetBps_;
    int target = targetBps_;
    if (signal == Signal::Overuse) {
        state_ = State::Decrease;
        target = int(0.85 * std::max(receiveBps_, options_.minBps));
        if (target < targetBps_) lastDecreaseBps_ = target;
        target = std::min(target, targetBps_);
    } else if (signal == Signal::Underuse) {
        state_ = State::Hold;
    } else if (limitedByApp) {
        // The source sends less than the target (a simple picture, or an
        // encoder under its rate): the target creeps up, bounded by twice
        // what flows, so a source that then has more to send (a bigger
        // picture) finds room, and a still screen asks for nothing.
        state_ = State::Hold;
        const int bound = 2 * std::max(sendBps_, receiveBps_) + options_.minBps;
        if (target < bound) target = std::min(bound, int(target * std::pow(1.02, seconds * 10)));
    } else {
        state_ = State::Increase;
        // Once the path has shown where it gives way, growth is gentle
        // around there (a packet per round trip); far from it, or before any
        // cut, it is quick.
        const bool nearLastCut = lastDecreaseBps_ > 0 && receiveBps_ < 2 * lastDecreaseBps_ && receiveBps_ > lastDecreaseBps_ / 2;
        if (nearLastCut) {
            const double rtt = std::clamp(rttMs > 0 ? rttMs : 100., 10., 1000.);
            target += int(1200 * 8 * 1000. / rtt * seconds);
        } else {
            target = int(target * std::pow(1.08, seconds * 10));  // 8% per 100 ms while probing upwards
        }
    }
    // Loss: more than a tenth lost while the path is being used, with delay
    // building or the round trip up (a queue filling and spilling), cuts the
    // rate, at most once per few round trips. Loss with no delay behind it is
    // the path losing at random (a mobile uplink): no rate helps that, repair
    // packets and repeats carry the stream across it.
    if (rttMs > 0) minRttMs_ = minRttMs_ > 0 ? std::min(minRttMs_, rttMs) : rttMs;
    // A cellular link's round trip wanders by tens of milliseconds and its
    // delay trend is seldom exactly flat: only a clear rise counts.
    const bool queueing = trend_ > threshold_ / 2 || (minRttMs_ > 0 && rttMs > 1.5 * minRttMs_ + 30);
    if (loss_ > 0.10 && !limitedByApp && queueing && receiveBps_ > 0 && nowMs - lastLossCutMs_ >= std::max(300., 3 * rttMs)) {
        target = std::min(target, int(targetBps_ * (1 - 0.5 * std::min(loss_, 0.5))));
        lastLossCutMs_ = nowMs; state_ = State::Decrease; lastDecreaseBps_ = target;
    }
    // Never far beyond what the path has been seen to carry, once it was asked to.
    if (receiveBps_ > 0 && !limitedByApp) target = std::min(target, int(MaxTargetOverReceived * receiveBps_) + options_.minBps);
    targetBps_ = std::clamp(target, options_.minBps, options_.maxBps);
}

}  // namespace ucd::screen
