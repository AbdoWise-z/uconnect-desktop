#pragma once

#include <QString>
#include <QVector>
#include <deque>

// How much the path carries, estimated the way real-time services do
// (Google congestion control): from the receiver's report of when each packet
// arrived, a queue building on the path shows as arrivals spreading out
// compared with departures, before anything is lost. That trend lowers the
// rate; its absence raises it, quickly while far from what the path has
// taken before and gently near it. Loss lowers it too. The result is what
// the encoder is asked for.
namespace ucd::screen {

class BandwidthEstimator {
public:
    struct Options {
        int startBps = 4000000, minBps = 150000, maxBps = 100000000;
    };
    // One packet the receiver reported on: when it left, when it arrived (the
    // receiver's clock; only differences are used) or that it was lost.
    struct PacketResult {
        qint64 sendMs = 0;
        double arrivalMs = 0;
        int bytes = 0;
        bool received = false;
    };
    void reset(const Options& options);
    // `results` in send order; `rttMs` the current round trip, if known.
    void onFeedback(const QVector<PacketResult>& results, qint64 nowMs, double rttMs);
    int targetBps() const { return targetBps_; }
    int receiveBps() const { return receiveBps_; }
    int sendBps() const { return sendBps_; }
    double lossRate() const { return loss_; }
    double trendMs() const { return trend_; }  // the delay trend's current modified value
    QString state() const;
    static constexpr double OveruseThresholdMs = 12.5;
private:
    enum class Signal { Normal, Overuse, Underuse };
    enum class State { Hold, Increase, Decrease };
    Signal detect(double trend, double dtMs);
    Options options_;
    int targetBps_ = 4000000, receiveBps_ = 0, sendBps_ = 0, lastDecreaseBps_ = 0;
    double loss_ = 0, trend_ = 0, threshold_ = OveruseThresholdMs, minRttMs_ = 0;
    State state_ = State::Increase;
    qint64 lastUpdateMs_ = 0, overusingSinceMs_ = -1, lastLossCutMs_ = -1000000;
    double previousTrend_ = 0;
    // Delay variation accumulated over recent packets, with a trend line
    // through its last samples (arrival time, smoothed delay).
    double accumulatedDelayMs_ = 0, smoothedDelayMs_ = 0;
    std::deque<std::pair<double, double>> samples_;
    int deltas_ = 0;
    double firstArrivalMs_ = 0;
    bool haveSample_ = false;
    qint64 previousSendMs_ = 0; double previousArrivalMs_ = 0;
    // Bytes received by arrival time, for the receive rate.
    std::deque<std::pair<double, int>> received_;
    double lastArrivalMs_ = 0;
    std::deque<std::pair<qint64, int>> sent_;  // bytes reported on, by send time
    qint64 lastSendMs_ = 0;
};

}  // namespace ucd::screen
