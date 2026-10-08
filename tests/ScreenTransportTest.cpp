#include <QtTest>
#include <algorithm>
#include <random>
#include "core/screen/MediaTransport.hpp"

using namespace ucd::screen;

class ScreenTransportTest : public QObject {
    Q_OBJECT
    static EncodedFrame frame(quint32 number, bool key = false, int size = 9000) {
        QByteArray bytes(size, '\0');
        for (int i = 0; i < size; ++i) bytes[i] = char((i * 37 + number) & 255);
        return {42, number, number * 33, key, bytes};
    }
private slots:
    void wireAndReorder() {
        Reassembler receiver; receiver.reset(42);
        const auto input = frame(0, true);
        auto packets = fragment(input);
        QCOMPARE(packets.size(), 9);
        QCOMPARE(packets.first().size(), 1100);
        QCOMPARE(packets.first().left(4).toHex(), QByteArray("0101002a"));
        QVector<EncodedFrame> output;
        for (int i = packets.size() - 1; i >= 0; --i) {
            output += receiver.receive(packets[i], 33);
            QVERIFY(receiver.receive(packets[i], 34).isEmpty());
        }
        QCOMPARE(output.size(), 1); QCOMPARE(output[0].bytes, input.bytes);
        QVERIFY(!receiver.waitingForKeyframe()); QCOMPARE(receiver.bufferedBytes(), 0);
        // Completing a later delta first must not decode across a missing reference.
        for (const auto& packet : fragment(frame(2))) QVERIFY(receiver.receive(packet, 66).isEmpty());
        for (const auto& packet : fragment(frame(1))) output += receiver.receive(packet, 70);
        QCOMPARE(output.size(), 3); QCOMPARE(output[1].number, 1u); QCOMPARE(output[2].number, 2u);
    }

    void keyframeNackAndRecovery() {
        Reassembler receiver; receiver.reset(42);
        PacedSender sender;
        const auto key = frame(0, true);
        QVERIFY(sender.enqueue(key, 0, 33));
        auto packets = fragment(key);
        for (int i = 0; i < packets.size(); ++i) if (i != 2) receiver.receive(packets[i], 0);
        auto recovery = receiver.expire(67);
        QCOMPARE(recovery.nacks.size(), 1); QCOMPARE(recovery.nacks[0].indexes, QVector<int>{2});
        auto repairs = sender.retransmit(0, recovery.nacks[0].indexes, 68);
        QCOMPARE(repairs.size(), 1); QCOMPARE(quint8(repairs[0][1]), quint8(3));
        const auto output = receiver.receive(repairs[0], 69);
        QCOMPARE(output.size(), 1); QCOMPARE(output[0].bytes, key.bytes);
        QVERIFY(receiver.expire(100).nacks.isEmpty());
        QVERIFY(sender.retransmit(0, {2}, 501).isEmpty());
        QVERIFY(sender.retransmit(0, {-1, MaxFragments + 1}, 70).isEmpty());
        // A delta frame with lost fragments is asked for once too...
        receiver.receive(fragment(frame(1)).first(), 100);
        recovery = receiver.expire(167);
        QCOMPARE(recovery.nacks.size(), 1); QCOMPARE(recovery.nacks[0].frame, 1u);
        // ...and if the repair does not come, deltas wait for the next keyframe.
        QVERIFY(receiver.expire(167 + 40).keyframe);
        for (const auto& packet : fragment(frame(2))) QVERIFY(receiver.receive(packet, 168).isEmpty());
        QVector<EncodedFrame> healed;
        for (const auto& packet : fragment(frame(3, true))) healed += receiver.receive(packet, 170);
        QCOMPARE(healed.size(), 1); QCOMPARE(healed[0].number, 3u);
    }

    void pacingAndTcpBackpressure() {
        PacedSender sender;
        const int fragments = fragment(frame(0, true, 100000)).size();
        QVERIFY(sender.enqueue(frame(0, true, 100000), 0, 33));
        // A frame arriving while the previous one still goes out waits its turn.
        QVERIFY(sender.enqueue(frame(1), 1, 33));
        QCOMPARE(sender.queuedFrames(), 2);
        int sent = 0;
        QVERIFY(sender.pump(0, [&](const QByteArray&) { ++sent; return true; }));
        QVERIFY(sent <= 4);
        QVERIFY(sender.pump(10, [&](const QByteArray&) { ++sent; return true; }));
        QVERIFY2(sent > 20 && sent < fragments, qPrintable(QString::number(sent)));  // spread, not a burst
        // A refused datagram (TCP fallback full, path still opening) is kept.
        QVERIFY(sender.pump(11, [&](const QByteArray&) { return false; }));
        QCOMPARE(sender.dropped(), 0u);
        // Half an interval later both frames are out, nothing lost.
        for (int time = 12; time <= 40; ++time) sender.pump(time, [&](const QByteArray&) { ++sent; return true; });
        QVERIFY(!sender.busy());
        QCOMPARE(sent, fragments + fragment(frame(1)).size());

        // At most three frames wait; a fourth is dropped.
        for (quint32 n = 2; n < 5; ++n) QVERIFY(sender.enqueue(frame(n), 100, 33));
        QVERIFY(!sender.enqueue(frame(5), 100, 33));
        QCOMPARE(sender.dropped(), 1u);
        // Frames too old to be worth showing go, with all behind them.
        QVERIFY(!sender.pump(251, [](const QByteArray&) { return false; }));
        QVERIFY(!sender.busy()); QCOMPARE(sender.dropped(), 4u);
        QVERIFY(sender.enqueue(frame(6), 300, 33));
        for (int time = 300; time <= 320; ++time) sender.pump(time, [](const QByteArray&) { return true; });
        QVERIFY(!sender.busy());
    }

    // A 165 Hz display at full rate: frames every 6.06 ms, none dropped.
    void highRefreshKeepsUp() {
        PacedSender sender;
        int frames = 0; double next = 0;
        for (qint64 time = 0; time < 5000; ++time) {
            if (time >= next) {
                QVERIFY(sender.enqueue(frame(quint32(frames), frames == 0, 20000), time, 1000 / MaxFps));
                ++frames; next += 1000. / MaxFps;
            }
            QVERIFY(sender.pump(time, [](const QByteArray&) { return true; }));
        }
        QCOMPARE(sender.dropped(), 0u);
        QVERIFY(frames >= 5 * MaxFps - 1);
    }

    // 60 fps with frames that take most of an interval to send: none dropped.
    void sixtyFramesPerSecondKeepUp() {
        PacedSender sender;
        int frames = 0;
        for (qint64 time = 0; time < 5000; time += 2) {
            if (time % 16 == 0 && sender.enqueue(frame(quint32(time / 16), time == 0, 30000), time, 16)) ++frames;
            QVERIFY(sender.pump(time + qint64(time % 3), [](const QByteArray&) { return true; }));
        }
        QCOMPARE(sender.dropped(), 0u);
        QVERIFY(frames >= 312);
    }

    void boundedMalformedAndSessionIsolation() {
        Reassembler receiver; receiver.reset(42);
        auto packet = fragment(frame(0, true)).first();
        auto wrong = packet; wrong[3] = 43;
        QVERIFY(receiver.receive(wrong, 0).isEmpty()); QCOMPARE(receiver.bufferedBytes(), 0);
        wrong = packet; wrong[10] = char(255); wrong[11] = char(255);
        QVERIFY(receiver.receive(wrong, 0).isEmpty()); QCOMPARE(receiver.pendingFrames(), size_t(0));
        QVERIFY(receiver.receive(packet.left(40), 0).isEmpty());
        for (int i = 0; i < 10000; ++i) receiver.receive(fragment(frame(i)).first(), 0);
        QVERIFY(receiver.pendingFrames() <= 8); QVERIFY(receiver.bufferedBytes() <= MaxFrameBytes * 2);
        QVERIFY(receiver.expire(100).keyframe);
        QCOMPARE(receiver.pendingFrames(), size_t(0));
        QVERIFY(fragment(frame(0, true, MaxFrameBytes + 1)).isEmpty());
        receiver.receive(packet, 110);
        wrong = packet; wrong[1] = 0;
        receiver.receive(wrong, 111);
        QCOMPARE(receiver.pendingFrames(), size_t(0));
        QVERIFY(receiver.expire(310).keyframe);
    }

    void timestampAndFrameWrap() {
        Reassembler receiver; receiver.reset(42);
        QVector<EncodedFrame> output;
        for (quint32 n : {quint32(0xffffffff), quint32(0)})
            for (const auto& packet : fragment(frame(n, n == 0xffffffff))) output += receiver.receive(packet, 100);
        QCOMPARE(output.size(), 2); QCOMPARE(output.back().number, 0u);
        QVERIFY(std::isfinite(receiver.report().jitterMs));
    }

    void fivePercentLossSimulation() {
        std::mt19937 random(183);
        Reassembler receiver; receiver.reset(42);
        PacedSender sender;
        CongestionController controller; controller.reset("direct");
        int delivered = 0, recoveredKeys = 0;
        quint32 lastDelivered = 0;
        bool wantKey = true;
        struct Delivery { qint64 at; QByteArray bytes; };
        QVector<Delivery> wire;
        auto accept = [&](const QVector<EncodedFrame>& frames) {
            for (const auto& result : frames) {
                QCOMPARE(result.bytes, frame(result.number).bytes);
                if (!result.keyframe && delivered) QCOMPARE(result.number, lastDelivered + 1);
                if (result.keyframe) ++recoveredKeys;
                lastDelivered = result.number; ++delivered;
            }
        };
        // 20 seconds, reproducible loss, random reorder/duplicates and occasional
        // delay spikes. Recovery requests cause a new key on the next capture.
        for (qint64 time = 0; time < 20000; ++time) {
            if (time % 33 == 0) {
                const auto number = quint32(time / 33);
                const bool key = wantKey || number % 120 == 0;
                if (sender.enqueue(frame(number, key), time, 33)) wantKey = false;
            }
            sender.pump(time, [&](const QByteArray& bytes) {
                if (random() % 100 >= 5) {
                    wire.push_back({time + qint64(random() % 20) + (random() % 200 == 0 ? 150 : 0), bytes});
                    if (random() % 50 == 0) wire.push_back({time + 25, bytes});
                }
                return true;
            });
            std::shuffle(wire.begin(), wire.end(), random);
            for (int i = wire.size() - 1; i >= 0; --i) if (wire[i].at <= time) {
                accept(receiver.receive(wire[i].bytes, time)); wire.removeAt(i);
            }
            const auto recovery = receiver.expire(time);
            if (recovery.keyframe) wantKey = true;
            for (const auto& nack : recovery.nacks)
                for (const auto& repair : sender.retransmit(nack.frame, nack.indexes, time))
                    if (random() % 100 >= 5) wire.push_back({time + 10, repair});
            if (time % 300 == 0) controller.update(receiver.report());
            QVERIFY(receiver.pendingFrames() <= 8);
            QVERIFY(receiver.bufferedBytes() <= MaxFrameBytes * 2);
        }
        QVERIFY2(delivered > 100, qPrintable(QString::number(delivered)));
        QVERIFY(recoveredKeys > 5);
        QVERIFY(controller.bitrate() >= CongestionController::MinBitrate); QVERIFY(controller.bitrate() <= 8000000);
        qInfo() << "5% loss:" << delivered << "frames," << recoveredKeys << "keyframe recoveries,"
                << controller.bitrate() << "bit/s";
    }

    void congestionAndReportValidation() {
        CongestionController controller; controller.reset("relayed", 30, 1920, 2000000);
        QCOMPARE(controller.bitrate(), 1500000);
        // Sustained loss: the bitrate falls (not more than once per few
        // reports) to its floor, then the frame rate gives.
        controller.update({100, 10, 0, 5, 10, 2});
        QCOMPARE(controller.bitrate(), 1200000);
        controller.update({100, 10, 0, 5, 10, 2});
        QCOMPARE(controller.bitrate(), 1200000);  // same bad patch: no second cut yet
        for (int i = 0; i < 40; ++i) controller.update({100, 10, 0, 5, 10, 2});
        QCOMPARE(controller.bitrate(), CongestionController::MinBitrate); QVERIFY(controller.fps() < 30);
        for (int i = 0; i < 200; ++i) controller.update({100, 0, 0, 0, 0, 2});
        QCOMPARE(controller.bitrate(), 2000000); QCOMPARE(controller.fps(), 30); QCOMPARE(controller.width(), 1920);
        // A viewer that decodes slowly gets fewer frames, not a worse picture.
        for (int i = 0; i < 6; ++i) controller.update({100, 0, 0, 0, 0, 80});
        QVERIFY(controller.fps() < 30); QCOMPARE(controller.bitrate(), 2000000);
        QVERIFY(Feedback::parse(Feedback{100, 2, 0, 4, -3, 5}.json()));
        auto bad = Feedback{}.json(); bad["lost"] = -1; QVERIFY(!Feedback::parse(bad));
        bad["lost"] = "100"; QVERIFY(!Feedback::parse(bad));
        controller.reset("tcp"); QCOMPARE(controller.bitrate(), 1000000);
    }
};

QTEST_GUILESS_MAIN(ScreenTransportTest)
#include "ScreenTransportTest.moc"
