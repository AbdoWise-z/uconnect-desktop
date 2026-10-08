#include <QtTest>
#include <QSignalSpy>
#include <QJsonArray>
#include <atomic>
#include <thread>
#include "core/Session.hpp"
#include "rendezvous.hpp"

using namespace ucd;
using namespace ucd::screen;
using namespace std::chrono_literals;

class ScreenSessionTest : public QObject {
    Q_OBJECT
    static uconnect::DevId peer(int n) { uconnect::DevId p{}; p[0] = uint8_t(n); return p; }
    static QJsonObject request(quint32 n = 1) {
        return {{"t", "scr.request"}, {"request", double(n)}, {"maxWidth", 1920}, {"maxFps", 30}, {"codecs", QJsonArray{"h264"}}};
    }
private slots:
    void consentPolicyAndLifecycle() {
        qint64 time = 0;
        PeerAccess access{"key", "Alice", "fingerprint", {perm::ScreenView}, true};
        QVector<QJsonObject> sent; int opens = 0, closes = 0;
        ScreenService service([&](const auto&, const auto& msg) { sent.push_back(msg); return true; },
            [&](const auto&) { return access; }, [&](const auto&) { ++opens; return true; },
            [&](const auto&) { ++closes; }, [](const auto&, const auto&) { return true; }, nullptr, [&] { return time; });
        service.setOffered(true);
        QSignalSpy prompt(&service, &ScreenService::consentRequested), hosting(&service, &ScreenService::hostingChanged);
        access.trusted = false;
        QVERIFY(service.handle(peer(1), request())); QCOMPARE(sent.back().value("code"), "forbidden"); QCOMPARE(prompt.size(), 0);
        access.trusted = true; service.handle(peer(1), request()); QCOMPARE(prompt.size(), 1); QCOMPARE(opens, 0);
        service.handle(peer(2), request()); QCOMPARE(sent.back().value("code"), "busy");
        const auto id = QString::fromStdString(uconnect::to_hex(peer(1)));
        service.consent(id, 99, true, false); QCOMPARE(opens, 0);
        service.consent(id, 1, false, false); QCOMPARE(opens, 0);
        service.handle(peer(1), request(2)); time = 30001; service.tick();
        service.consent(id, 2, true, false); QCOMPARE(opens, 0);
        service.handle(peer(1), request(3));
        service.handle(peer(1), {{"t", "scr.stop"}, {"request", 3}});
        service.consent(id, 3, true, false); QCOMPARE(opens, 0);
        service.handle(peer(1), request(4)); service.consent(id, 4, true, false); QCOMPARE(opens, 1);
        QVERIFY(!hosting.back()[0].toList().isEmpty());
        access.permissions.clear(); service.policyChanged(); QVERIFY(hosting.back()[0].toList().isEmpty());
        access.permissions = {perm::ScreenView, perm::ScreenDirect};
        service.handle(peer(1), request(5)); QCOMPARE(opens, 2);
        service.peerGone(peer(1)); QVERIFY(hosting.back()[0].toList().isEmpty()); QVERIFY(closes >= 2);
        service.handle(peer(1), request(6)); QCOMPARE(opens, 3);
        time += 5001; service.tick(); QVERIFY(hosting.back()[0].toList().isEmpty());
    }

    void twoRealSessionsSyntheticVideo() {
        if (qEnvironmentVariableIsEmpty("UCD_TEST_OPENH264")) QSKIP("Install Cisco OpenH264 for synthetic end-to-end test");
        uconnect::server::RendezvousConfig config; config.port = 0;
        uconnect::server::Rendezvous server(config); QVERIFY(server.open());
        std::jthread serverThread([&](std::stop_token stop) { while (!stop.stop_requested()) server.poll_once(5ms); });
        Session host, viewer;
        const auto ih = Identity::generate(), iv = Identity::generate();
        host.setIdentity(ih); viewer.setIdentity(iv);
        AccessPolicy hp, vp;
        hp.trusted.insert(keyToHex(iv.publicKey()), {perm::ScreenView});
        vp.trusted.insert(keyToHex(ih.publicKey()), {});
        host.setAccessPolicy(hp); viewer.setAccessPolicy(vp);
        host.setLocalHello({"Host", "test", ServiceScreen}); viewer.setLocalHello({"Viewer", "test", 0});
        ScreenSettings settings; settings.ciscoLibrary = qEnvironmentVariable("UCD_TEST_OPENH264");
        settings.monitors = {QVariantMap{{"name", "Synthetic"}, {"width", 256}, {"height", 128}}};
        host.screen()->configure(settings);
        QString hostId, viewerId;
        connect(&host, &Session::snapshotChanged, this, [&](const auto& s) { if (!s.selfId.isEmpty()) hostId = s.selfId; });
        connect(&viewer, &Session::snapshotChanged, this, [&](const auto& s) { if (!s.selfId.isEmpty()) viewerId = s.selfId; });
        bool proven = false;
        connect(&viewer, &Session::peersChanged, this, [&](const auto& peers) {
            for (const auto& p : peers) if (p.trusted && p.stateCode == 3) proven = true;
        });
        const auto creds = uconnect::TopicCreds::generate_keyed();
        Session::Options options{QString::fromStdString("127.0.0.1:" + std::to_string(server.port())), QString::fromStdString(creds.to_uri())};
        host.start(options); viewer.start(options);
        QTRY_VERIFY_WITH_TIMEOUT(proven, 20000);
        QTRY_VERIFY_WITH_TIMEOUT(!hostId.isEmpty() && !viewerId.isEmpty(), 2000);
        const auto remote = *uconnect::from_hex<uconnect::kDevIdLen>(hostId.toStdString());
        QSignalSpy prompt(host.screen(), &ScreenService::consentRequested), images(viewer.screen(), &ScreenService::imageReady);
        QSignalSpy hostState(host.screen(), &ScreenService::hostingChanged);
        viewer.screen()->request(remote);
        QTRY_COMPARE_WITH_TIMEOUT(prompt.size(), 1, 3000);
        QVERIFY(images.isEmpty());
        host.screen()->consent(viewerId, prompt[0][1].toUInt(), true, false);
        QTRY_VERIFY_WITH_TIMEOUT(!hostState.isEmpty(), 3000);
        QImage synthetic(256, 128, QImage::Format_RGBA8888); synthetic.fill(QColor(40, 100, 150));
        // First capture may precede datagram negotiation, so keep the synthetic
        // desktop changing until the channel/keyframe recovery delivers it.
        for (int i = 0; i < 90 && images.isEmpty(); ++i) { host.screen()->submitImage(synthetic); QTest::qWait(35); }
        QVERIFY2(!images.isEmpty(), "No decoded image crossed the real Session datagram channel");
        QCOMPARE(qvariant_cast<QImage>(images.back()[1]).size(), synthetic.size());
        viewer.screen()->stop(remote);
        QTRY_VERIFY_WITH_TIMEOUT(hostState.back()[0].toList().isEmpty(), 3000);
        host.stop(); viewer.stop();
    }
};
QTEST_GUILESS_MAIN(ScreenSessionTest)
#include "ScreenSessionTest.moc"
