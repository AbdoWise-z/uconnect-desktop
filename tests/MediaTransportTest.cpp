// The media layer (screen/camera 2.0) on a simulated path: packets, repair,
// feedback, the bandwidth estimate and pacing, with loss, delay and a
// bottleneck between a sender and a receiver run in lockstep.
#include <QtTest>
#include <random>
#include "core/screen/MediaPacket.hpp"
#include "core/screen/MediaReceiver.hpp"
#include "core/screen/MediaSender.hpp"

using namespace ucd::screen;

namespace {

// A one-way path: random loss, a fixed delay, and a bottleneck with a
// queue of bounded depth (a full one drops what arrives).
struct Link {
    double loss = 0;
    int delayMs = 20;
    int bottleneckBps = 0;  // 0: unlimited
    int queueMs = 300;      // the bottleneck's buffer, in time at its rate
    bool reliable = false;  // TCP-like: never drops, only waits (a deep queue)
    std::mt19937 rng{42};
    double busyUntil = 0;
    std::deque<std::pair<double, QByteArray>> inFlight;
    quint64 sent = 0, dropped = 0;
    // False if refused (a reliable path's queue is full: it pushes back).
    bool send(const QByteArray& packet, double nowMs) {
        ++sent;
        double at = nowMs;
        if (bottleneckBps > 0) {
            const double serve = packet.size() * 8000. / bottleneckBps;
            const double start = std::max(nowMs, busyUntil);
            if (start - nowMs > queueMs) {
                if (reliable) return false;
                ++dropped; return true;
            }
            busyUntil = start + serve; at = busyUntil;
        }
        if (!reliable && std::uniform_real_distribution<double>(0, 1)(rng) < loss) { ++dropped; return true; }
        inFlight.emplace_back(at + delayMs, packet);
        return true;
    }
    QVector<QByteArray> due(double nowMs) {
        QVector<QByteArray> out;
        std::sort(inFlight.begin(), inFlight.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        while (!inFlight.empty() && inFlight.front().first <= nowMs) { out.push_back(inFlight.front().second); inFlight.pop_front(); }
        return out;
    }
};

// Runs a session: a source producing frames at `fps` whose size follows the
// sender's target (an encoder doing as asked), through `forward` to the
// receiver, whose feedback returns through `back`.
struct Run {
    int fps = 30, seconds = 10;
    Link forward, back;
    MediaSender::Options options;
    // Results
    int framesSent = 0, framesDelivered = 0;
    QVector<double> lagMs;   // delivery time minus capture time, per delivered frame
    QVector<int> targets;    // the target rate once a second
    quint64 recovered = 0, repaired = 0, givenUp = 0;
    double rttMs = 0;
    int queuedAtEnd = 0, pendingAtEnd = 0, skipped = 0, keyframes = 0;
    quint64 late = 0, discarded = 0;
    QStringList perSecond;  // receive/send rates, loss, estimator state
    double maxLagInSecond = 0;
    bool lagNoted = false;
    void go() {
        MediaSender sender; sender.reset(options);
        MediaReceiver receiver; receiver.reset(options.session, options.kind, fps);
        const double interval = 1000. / fps;
        double nextFrame = 0; quint32 number = 0;
        for (qint64 now = 0; now < qint64(seconds) * 1000; ++now) {
            if (now >= nextFrame) {
                nextFrame += interval;
                if (!sender.shouldSkipCapture(now)) {
                    const bool key = number == 0 || sender.takeKeyframeWanted(now);
                    if (key) ++keyframes;
                    // Bytes per frame at the encoder's rate; a keyframe is 6x.
                    const int bytes = std::max(200, int(qint64(sender.encoderBps()) / 8 / fps * (key ? 6 : 1)));
                    EncodedFrame frame{options.session, number, quint32(now), key, QByteArray(bytes, char(number))};
                    if (sender.enqueue(frame, now)) ++framesSent;
                    ++number;
                } else {
                    ++skipped;  // not encoded: no frame number used
                }
            }
            sender.pump(now, [&](const QByteArray& packet) { return forward.send(packet, double(now)); });
            for (const auto& packet : forward.due(double(now))) {
                for (const auto& frame : receiver.receive(packet, now)) {
                    ++framesDelivered; lagMs.push_back(double(now - frame.captureMs));
                }
            }
            receiver.expire(now);
            if (receiver.feedbackDue(now)) {
                const auto fb = receiver.takeFeedback(now, 0, 2);
                if (!fb.isEmpty()) back.send(fb, double(now));
            }
            for (const auto& packet : back.due(double(now))) sender.onFeedback(packet, now);
            if (sender.lagMs() > 600 && !lagNoted) {
                lagNoted = true;
                qInfo() << "lag" << sender.lagMs() << "at" << now << "newest sent" << sender.lastSentCaptureMs() << "receiver latest"
                        << (receiver.latestCaptureMs() ? int(*receiver.latestCaptureMs()) : -1) << receiver.pendingText();
            }
            maxLagInSecond = std::max(maxLagInSecond, sender.lagMs());
            if (now % 1000 == 999) {
                targets.push_back(sender.targetBps());
                perSecond << QStringLiteral("[recv %1 send %2 loss %3% %4 rtt %5 q %6 lag %7 drop %8]").arg(sender.receiveBps() / 1000).arg(sender.sendBps() / 1000)
                                 .arg(sender.lossRate() * 100, 0, 'f', 0).arg(sender.estimatorState()).arg(sender.rttMs(), 0, 'f', 0).arg(sender.queuedBytes())
                                 .arg(maxLagInSecond, 0, 'f', 0).arg(sender.dropped());
                maxLagInSecond = 0;
            }
        }
        const auto stats = receiver.report();
        recovered = stats.recovered; repaired = stats.repaired; givenUp = stats.givenUp; late = stats.late; discarded = stats.discarded;
        rttMs = sender.rttMs(); queuedAtEnd = sender.queuedBytes(); pendingAtEnd = int(receiver.pendingFrames());
        perSecond << receiver.giveLog;
    }
    QString targetsText() const {
        QStringList parts; for (int t : targets) parts << QString::number(t / 1000);
        return parts.join(' ') + QStringLiteral(" kbit/s; frames %1 of %2 (skipped %7, keys %9, late %10, discarded %11), given up %3, lag p50 %4 p95 %5 ms, queued %6 B, pending %8 | ")
            .arg(framesDelivered).arg(framesSent).arg(givenUp).arg(lagPercentile(.5)).arg(lagPercentile(.95)).arg(queuedAtEnd).arg(skipped).arg(pendingAtEnd).arg(keyframes).arg(late).arg(discarded)
            + perSecond.join(' ');
    }
    double lagPercentile(double p, int fromSecond = 0) const {
        QVector<double> v;
        for (int i = 0; i < lagMs.size(); ++i) if (i >= fromSecond * fps) v.push_back(lagMs[i]);
        if (v.isEmpty()) return 0;
        std::sort(v.begin(), v.end());
        return v[std::min(v.size() - 1, qsizetype(v.size() * p))];
    }
};

}  // namespace

class MediaTransportTest : public QObject {
    Q_OBJECT
private slots:
    void packetsRoundTrip() {
        EncodedFrame frame{7, 42, 123456, true, QByteArray(3000, 'x')};
        quint32 seq = 100;
        const auto packets = packetize(1, frame, seq, 2);
        QCOMPARE(packets.size(), 3 + 2);  // 3 fragments, a repair after each 2
        QCOMPARE(seq, 105u);
        for (const auto& p : packets) {
            const auto bytes = encodeMedia(p);
            QVERIFY(bytes.size() <= MaxDatagram);
            const auto back = decodeMedia(bytes);
            QVERIFY(back.has_value());
            QCOMPARE(back->seq, p.seq); QCOMPARE(back->frame, 42u); QCOMPARE(back->captureMs, 123456u);
            QCOMPARE(back->type, p.type); QCOMPARE(back->payload, p.payload); QVERIFY(back->keyframe);
        }
        Feedback f; f.session = 7; f.baseSeq = 100; f.refTimeMs = 5000; f.heldMs = 3; f.latestFrame = 41; f.latestCaptureMs = 120000;
        f.haveLatest = true; f.keyframeWanted = true; f.arrivals = {4, Feedback::Missing, 12}; f.nacks = {90, 91};
        const auto decoded = Feedback::decode(f.encode(1));
        QVERIFY(decoded.has_value());
        QCOMPARE(decoded->arrivals, f.arrivals); QCOMPARE(decoded->nacks, f.nacks); QVERIFY(decoded->keyframeWanted);
        QCOMPARE(decoded->latestCaptureMs, 120000u); QCOMPARE(decoded->heldMs, quint16(3));
        QVERIFY(!decodeMedia(QByteArray(5, 'z')).has_value());
        QVERIFY(!Feedback::decode(QByteArray(10, 'z')).has_value());
    }

    void repairRebuildsOneLostFragmentPerGroup() {
        EncodedFrame frame{7, 1, 1000, true, QByteArray()};
        for (int i = 0; i < 5000; ++i) frame.bytes.append(char(i * 7));
        quint32 seq = 0;
        const auto packets = packetize(1, frame, seq, 2);  // 5 fragments: groups of 2, 2, 1
        MediaReceiver receiver; receiver.reset(7, 1, 30);
        QVector<EncodedFrame> out;
        for (const auto& p : packets) {
            if (p.type == PacketType::Media && (p.frag == 0 || p.frag == 3 || p.frag == 4)) continue;  // one lost per group
            out += receiver.receive(encodeMedia(p), 10);
        }
        QCOMPARE(out.size(), 1);
        QCOMPARE(out[0].bytes, frame.bytes);
        QCOMPARE(receiver.report().recovered, quint64(3));
        // A group of one: the repair packet alone rebuilds its fragment.
        quint32 seq2 = 0;
        MediaReceiver doubled; doubled.reset(7, 1, 30);
        QVector<EncodedFrame> twice;
        for (const auto& p : packetize(1, frame, seq2, 1)) {
            if (p.type == PacketType::Media) continue;  // every fragment lost
            twice += doubled.receive(encodeMedia(p), 10);
        }
        QCOMPARE(twice.size(), 1);
        QCOMPARE(twice[0].bytes, frame.bytes);
    }

    // Two fragments of a frame are lost; the feedback asks for them and the
    // repeats complete the frame.
    void repeatsFillWhatWasLost() {
        MediaSender sender; MediaSender::Options options; options.session = 7; options.startBps = 2000000; sender.reset(options);
        MediaReceiver receiver; receiver.reset(7, 1, 30);
        EncodedFrame frame{7, 0, 100, true, QByteArray(16 * MediaPayload, 'k')};
        QVERIFY(sender.enqueue(frame, 100));
        QVector<EncodedFrame> out;
        int sentCount = 0;
        for (qint64 now = 100; now < 400; ++now) {
            sender.pump(now, [&](const QByteArray& bytes) {
                const auto p = decodeMedia(bytes); ++sentCount;
                if (p && !p->retransmit && (p->frag == 3 || p->frag == 7)) return true;  // lost on the way
                out += receiver.receive(bytes, now + 5);
                return true;
            });
            receiver.expire(now);
            if (receiver.feedbackDue(now)) {
                const auto fb = receiver.takeFeedback(now, 0, 1);
                const auto decoded = Feedback::decode(fb);
                QVERIFY(decoded.has_value());
                if (now >= 150 && now < 160) QVERIFY2(!decoded->nacks.isEmpty(), qPrintable(QString::number(now)));
                sender.onFeedback(fb, now + 5);
            }
            if (!out.isEmpty()) break;
        }
        QCOMPARE(out.size(), 1);
        QCOMPARE(out[0].bytes, frame.bytes);
        QCOMPARE(sender.retransmitted(), quint64(2));
        QCOMPARE(receiver.report().repaired, quint64(2));
    }

    void cleanPathDeliversEverythingWithLowLag() {
        Run run; run.options.session = 7; run.options.startBps = 4000000; run.seconds = 6;
        run.go(); qInfo().noquote() << QTest::currentTestFunction() << run.targetsText();
        QVERIFY2(run.framesDelivered >= run.framesSent - 2, qPrintable(run.targetsText()));  // the last may still be on its way
        // The first second queues up to the lag budget while the rate climbs.
        QVERIFY2(run.lagPercentile(.95, 2) < 80, qPrintable(run.targetsText()));
        QVERIFY(run.rttMs > 30 && run.rttMs < 70);  // 20 ms each way plus the feedback interval
        QVERIFY2(run.targets.last() > 4000000, qPrintable(run.targetsText()));  // room to grow
    }

    void lossIsRepairedAndRepeated() {
        Run run; run.options.session = 7; run.options.startBps = 2000000; run.forward.loss = 0.25; run.seconds = 10;
        run.go(); qInfo().noquote() << QTest::currentTestFunction() << run.targetsText();
        // Repair packets and repeats bring most frames through on a path losing a quarter.
        QVERIFY2(run.framesDelivered > run.framesSent * 0.85, qPrintable(run.targetsText()));
        QVERIFY(run.recovered > 0); QVERIFY(run.repaired > 0);
        QVERIFY2(run.lagPercentile(.95, 2) < 250, qPrintable(run.targetsText()));
    }

    void bottleneckIsFoundAndLagStaysBounded() {
        Run run; run.options.session = 7; run.options.startBps = 4000000;
        run.forward.bottleneckBps = 1500000; run.forward.queueMs = 400; run.seconds = 12;
        run.go(); qInfo().noquote() << QTest::currentTestFunction() << run.targetsText();
        // Converged under the bottleneck, not far under it.
        const int target = run.targets.last();
        QVERIFY2(target < 1650000 && target > 700000, qPrintable(run.targetsText()));  // at the bottleneck, a little over at most
        QVERIFY2(run.lagPercentile(.95, 4) < 400, qPrintable(run.targetsText()));
        QVERIFY2(run.framesDelivered > run.framesSent * 0.7, qPrintable(run.targetsText()));  // the first seconds start above the path
    }
};

QTEST_GUILESS_MAIN(MediaTransportTest)
#include "MediaTransportTest.moc"
