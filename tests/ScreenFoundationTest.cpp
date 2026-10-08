#include <QtTest>
#include <atomic>
#include <mutex>
#include <thread>
#include "core/Permissions.hpp"
#include "core/screen/DatagramChannel.hpp"
#include "rendezvous.hpp"
#ifdef UCD_HAS_SCREEN
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}
#endif

using namespace uconnect;
using namespace std::chrono_literals;

class ScreenFoundationTest : public QObject {
    Q_OBJECT
private slots:
    void trustedOnly() {
        const ucd::PermissionSet grants{ucd::perm::ScreenView, ucd::perm::ScreenDirect,
            ucd::perm::ControlInput, ucd::perm::ControlDirect, ucd::perm::StorageObserve};
        QCOMPARE(ucd::permissionsForUntrusted(grants), ucd::PermissionSet{ucd::perm::StorageObserve});
        QVERIFY(ucd::isKnownPermission(ucd::perm::ScreenDirect));
        QVERIFY(ucd::isTrustedOnlyPermission(ucd::perm::ControlDirect));
    }

    void datagrams_data() {
        QTest::addColumn<int>("path");
        QTest::addColumn<int>("fallback");
        QTest::addColumn<bool>("relay");
        QTest::newRow("direct UDP") << int(DatagramPath::Direct) << int(DatagramFallback::None) << false;
        QTest::newRow("UDP relay") << int(DatagramPath::Relayed) << int(DatagramFallback::Relay) << true;
        QTest::newRow("TCP fallback") << int(DatagramPath::Tcp) << int(DatagramFallback::Tcp) << true;
    }

    void datagrams() {
        QFETCH(int, path);
        QFETCH(int, fallback);
        QFETCH(bool, relay);
        server::RendezvousConfig serverConfig;
        serverConfig.port = 0;
        server::Rendezvous server(serverConfig);
        QVERIFY2(server.open(), server.error().c_str());
        std::jthread serverThread([&](std::stop_token stop) {
            while (!stop.stop_requested()) server.poll_once(5ms);
        });
        Node::Config config;
        config.server = "127.0.0.1:" + std::to_string(server.port());
        config.force_relay = relay;
        config.datagram_fallback = DatagramFallback(fallback);
        std::mutex mutex;
        int receivedA = 0, receivedB = 0;
        ucd::screen::DatagramChannel channelA([&](const DevId&, uint8_t kind, auto bytes) {
            if (kind == 2 && bytes.size() == 3) ++receivedA;
        });
        ucd::screen::DatagramChannel channelB([&](const DevId&, uint8_t kind, auto bytes) {
            if (kind == 1 && bytes.size() == 3) ++receivedB;
        });
        // Nodes die before the callback targets and server.
        Node a(config), b(config);
        a.run_in_background(); b.run_in_background();
        const auto credentials = TopicCreds::generate_keyed();
        auto& ta = a.join(credentials); auto& tb = b.join(credentials);
        channelA.attach(&ta); channelB.attach(&tb);
        ta.on_datagram([&](DevId peer, auto bytes) { std::lock_guard lock(mutex); channelA.receive(peer, bytes); });
        tb.on_datagram([&](DevId peer, auto bytes) { std::lock_guard lock(mutex); channelB.receive(peer, bytes); });
        QVERIFY(ta.publish()); QVERIFY(tb.publish());
        const auto ida = *ta.self(), idb = *tb.self();
        (void)ta.peers(); (void)tb.peers();
        ta.connect(idb); tb.connect(ida);
        QTRY_VERIFY_WITH_TIMEOUT(ta.state(idb) == PeerState::Connected && tb.state(ida) == PeerState::Connected, 20000);
        const std::array<uint8_t, 3> video{1, 4, 5}, cursor{2, 4, 5}, unknown{255, 4, 5};
        {
            std::lock_guard lock(mutex);
            QVERIFY(!channelA.send(idb, video));
            QVERIFY(!channelA.receive(idb, video));
            QVERIFY(channelA.open(idb, DatagramFallback(fallback)));
        }
        // Let the remote node process the open first: its application has not
        // authorized media yet, but a subsequent consented open must adopt it.
        QTest::qWait(100);
        { std::lock_guard lock(mutex);
            QVERIFY(!channelB.receive(ida, video));
            QVERIFY(channelB.open(ida, DatagramFallback(fallback)));
        }
        QTRY_VERIFY_WITH_TIMEOUT(int(ta.datagram_path(idb)) == path && int(tb.datagram_path(ida)) == path, 15000);
        for (int i = 0; i < 20; ++i) {
            { std::lock_guard lock(mutex);
              QVERIFY(channelA.send(idb, video)); QVERIFY(channelB.send(ida, cursor)); }
            QTest::qWait(5);
        }
        auto delivered = [&] { std::lock_guard lock(mutex); return receivedA >= 15 && receivedB >= 15; };
        QTRY_VERIFY_WITH_TIMEOUT(delivered(), 3000);
        { std::lock_guard lock(mutex);
          QVERIFY(!channelA.receive(idb, unknown));
          QVERIFY(!channelA.send(idb, std::vector<uint8_t>(1101, 1)));
          channelA.close(idb);
          QVERIFY(!channelA.receive(idb, video)); }
    }

    void codecRoundTrip() {
#ifdef UCD_HAS_SCREEN
        const QByteArray config(avcodec_configuration());
        QVERIFY(!config.contains("--enable-gpl"));
        QVERIFY(!config.contains("--enable-nonfree"));
        const auto* encoder = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
        const auto* decoder = avcodec_find_decoder(AV_CODEC_ID_MPEG4);
        QVERIFY(encoder); QVERIFY(decoder);
        auto* enc = avcodec_alloc_context3(encoder);
        auto* dec = avcodec_alloc_context3(decoder);
        enc->width = 64; enc->height = 64; enc->pix_fmt = AV_PIX_FMT_YUV420P;
        enc->time_base = {1, 30}; enc->bit_rate = 1000000; enc->max_b_frames = 0;
        QVERIFY(avcodec_open2(enc, encoder, nullptr) >= 0);
        QVERIFY(avcodec_open2(dec, decoder, nullptr) >= 0);
        auto* frame = av_frame_alloc(); auto* output = av_frame_alloc(); auto* packet = av_packet_alloc();
        frame->width = 64; frame->height = 64; frame->format = enc->pix_fmt;
        QVERIFY(av_frame_get_buffer(frame, 32) >= 0);
        for (int plane = 0; plane < 3; ++plane)
            for (int y = 0; y < (plane ? 32 : 64); ++y)
                memset(frame->data[plane] + y * frame->linesize[plane], plane ? 128 : 100, plane ? 32 : 64);
        QVERIFY(avcodec_send_frame(enc, frame) >= 0);
        QVERIFY(avcodec_receive_packet(enc, packet) >= 0);
        QVERIFY(avcodec_send_packet(dec, packet) >= 0);
        QVERIFY(avcodec_receive_frame(dec, output) >= 0);
        QCOMPARE(output->width, 64); QCOMPARE(output->height, 64);
        QVERIFY(std::abs(int(output->data[0][0]) - 100) <= 3);
        av_packet_free(&packet); av_frame_free(&frame); av_frame_free(&output);
        avcodec_free_context(&enc); avcodec_free_context(&dec);
#else
        QSKIP("Optional FFmpeg/Qt Multimedia dependencies not installed");
#endif
    }
};

QTEST_GUILESS_MAIN(ScreenFoundationTest)
#include "ScreenFoundationTest.moc"
