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

    // A session whose capture never delivers says so, once, to both sides.
    void hostSaysWhenNothingIsCaptured() {
        qint64 time = 0;
        PeerAccess access{"key", "Viewer", "fp", {perm::ScreenView, perm::ScreenDirect}, true};
        QVector<QJsonObject> sent;
        ScreenService service([&](const auto&, const auto& msg) { sent.push_back(msg); return true; },
            [&](const auto&) { return access; }, [](const auto&) { return true; }, [](const auto&) {},
            [](const auto&, const auto&) { return true; }, nullptr, [&] { return time; });
        ScreenSettings settings; settings.gpuCapture = false;
        settings.monitors = {QVariantMap{{"name", "Synthetic"}, {"width", 256}, {"height", 128}}};
        service.configure(settings); service.setOffered(true);
        QSignalSpy activity(&service, &ScreenService::activity);
        service.handle(peer(1), request());
        const int token = sent.back().value("session").toInt(); QVERIFY(token > 0);
        const auto statuses = [&] { return std::count_if(sent.begin(), sent.end(), [](const auto& m) { return m.value("t") == "scr.status"; }); };
        const QJsonObject report{{"t", "scr.report"}, {"session", token}, {"received", 0}, {"lost", 0}, {"late", 0},
                                 {"jitterMs", 0}, {"owdTrendMs", 0}, {"decodeMs", 0}};
        time = 4000; service.handle(peer(1), report); service.tick(); QCOMPARE(statuses(), 0);
        time = 5001; service.tick(); QCOMPARE(statuses(), 1);
        QVERIFY(sent.back().value("detail").toString().contains("not produced a picture"));
        QVERIFY(std::any_of(activity.begin(), activity.end(), [](const auto& a) { return a[0].toString().contains("not produced a picture"); }));
        time = 6000; service.handle(peer(1), report); service.tick(); QCOMPARE(statuses(), 1);
    }

    // Native capture is tried first; Qt's capture is wanted only once it fails,
    // and only then does Qt's failure end hosting.
    void captureFailureWaitsForNativePath() {
        for (bool gpu : {false, true}) {
            PeerAccess access{"key", "Viewer", "fp", {perm::ScreenView, perm::ScreenDirect}, true};
            QVector<QJsonObject> sent;
            ScreenService service([&](const auto&, const auto& msg) { sent.push_back(msg); return true; },
                [&](const auto&) { return access; }, [](const auto&) { return true; }, [](const auto&) {},
                [](const auto&, const auto&) { return true; });
            ScreenSettings settings; settings.gpuCapture = gpu;
            settings.monitors = {QVariantMap{{"name", "No such monitor"}, {"width", 256}, {"height", 128}}};
            service.configure(settings); service.setOffered(true);
            QSignalSpy hosting(&service, &ScreenService::hostingChanged), qt(&service, &ScreenService::qtCaptureWanted);
            service.handle(peer(1), request());
            QVERIFY(!hosting.back()[0].toList().isEmpty());
            if (gpu) {
                QVERIFY(qt.isEmpty());  // the native path goes first
                service.captureFailed("not running");  // a late error from Qt's capture, stopped meanwhile
                QVERIFY(!hosting.back()[0].toList().isEmpty());
                // It cannot find the monitor: Qt's capture takes over.
                QTRY_VERIFY_WITH_TIMEOUT(!qt.isEmpty() && qt.back()[0].toBool(), 3000);
            } else {
                QVERIFY(qt.back()[0].toBool());
            }
            service.captureFailed("capture failed");
            QVERIFY(hosting.back()[0].toList().isEmpty());
            QVERIFY(!qt.back()[0].toBool());
        }
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
        hp.trusted.insert(keyToHex(iv.publicKey()), {perm::ScreenView, perm::CameraView});
        // Only the host trusts: the viewer watching it needs no trust of its own.
        host.setAccessPolicy(hp); viewer.setAccessPolicy(vp);
        host.setLocalHello({"Host", "test", ServiceScreen | ServiceCamera}); viewer.setLocalHello({"Viewer", "test", 0});
        ScreenSettings settings; settings.ciscoLibrary = qEnvironmentVariable("UCD_TEST_OPENH264");
        settings.monitors = {QVariantMap{{"name", "Synthetic"}, {"width", 256}, {"height", 128}}};
        host.screen()->configure(settings); host.camera()->configure(settings);
        QString hostId, viewerId;
        connect(&host, &Session::snapshotChanged, this, [&](const auto& s) { if (!s.selfId.isEmpty()) hostId = s.selfId; });
        connect(&viewer, &Session::snapshotChanged, this, [&](const auto& s) { if (!s.selfId.isEmpty()) viewerId = s.selfId; });
        bool proven = false;
        connect(&viewer, &Session::peersChanged, this, [&](const auto& peers) {
            for (const auto& p : peers) if (p.identityState == "verified" && p.stateCode == 3) proven = true;
        });
        const auto creds = uconnect::TopicCreds::generate_keyed();
        Session::Options options{QString::fromStdString("127.0.0.1:" + std::to_string(server.port())), QString::fromStdString(creds.to_uri())};
        host.start(options); viewer.start(options);
        QTRY_VERIFY_WITH_TIMEOUT(proven, 20000);
        QTRY_VERIFY_WITH_TIMEOUT(!hostId.isEmpty() && !viewerId.isEmpty(), 2000);
        const auto remote = *uconnect::from_hex<uconnect::kDevIdLen>(hostId.toStdString());
        QSignalSpy prompt(host.screen(), &ScreenService::consentRequested), images(viewer.screen(), &ScreenService::frameReady);
        QSignalSpy viewerState(viewer.screen(), &ScreenService::viewerChanged);
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
        QVERIFY2(!images.isEmpty(), qPrintable(QString("No decoded image crossed the real Session datagram channel; last state: %1")
            .arg(viewerState.isEmpty() ? QString("none") : QString::fromUtf8(QJsonDocument::fromVariant(viewerState.back()[1]).toJson(QJsonDocument::Compact)))));
        QCOMPARE(qvariant_cast<QVideoFrame>(images.back()[1]).size(), synthetic.size());
        QSignalSpy cameraPrompt(host.camera(), &ScreenService::consentRequested), cameraImages(viewer.camera(), &ScreenService::frameReady);
        viewer.camera()->request(remote);
        QTRY_COMPARE_WITH_TIMEOUT(cameraPrompt.size(), 1, 3000);
        host.camera()->consent(viewerId, cameraPrompt[0][1].toUInt(), true, false);
        for (int i = 0; i < 90 && cameraImages.isEmpty(); ++i) { host.camera()->submitImage(synthetic); host.screen()->submitImage(synthetic); QTest::qWait(35); }
        QVERIFY2(!cameraImages.isEmpty(), "Camera did not use the existing real datagram connection");
        viewer.camera()->stop(remote); QTest::qWait(100);
        const auto before = images.size();
        for (int i = 0; i < 50 && images.size() == before; ++i) { host.screen()->submitImage(synthetic); QTest::qWait(35); }
        QVERIFY2(images.size() > before, "Stopping camera closed the screen's shared datagram connection");
        viewer.screen()->stop(remote);
        QTRY_VERIFY_WITH_TIMEOUT(hostState.back()[0].toList().isEmpty(), 3000);
        host.stop(); viewer.stop();
    }
};
QTEST_GUILESS_MAIN(ScreenSessionTest)
#include "ScreenSessionTest.moc"
