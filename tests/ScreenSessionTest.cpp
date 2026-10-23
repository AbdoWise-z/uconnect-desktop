#include <QtTest>
#include <QSignalSpy>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QVideoFrame>
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
        // No feedback at all: while the media path is still opening, a session gets 40 s; 5 s after.
        time += 5001; service.tick(); QVERIFY(!hosting.back()[0].toList().isEmpty());
        time += 35001; service.tick(); QVERIFY(hosting.back()[0].toList().isEmpty());
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
        // The viewer's feedback keeps the session alive, even with nothing to report.
        Feedback keepalive; keepalive.session = quint16(token);
        const auto report = keepalive.encode(1);
        time = 4000; service.receive(peer(1), report); service.tick(); QCOMPARE(statuses(), 0);
        time = 5001; service.tick(); QCOMPARE(statuses(), 1);
        QVERIFY(sent.back().value("detail").toString().contains("not produced a picture"));
        QVERIFY(std::any_of(activity.begin(), activity.end(), [](const auto& a) { return a[0].toString().contains("not produced a picture"); }));
        time = 6000; service.receive(peer(1), report); service.tick(); QCOMPARE(statuses(), 1);
    }

    // Unplugging another monitor (ending an extended display, say) moves the
    // shared one in the list; the share follows it. It ends with its own
    // monitor, and never carries on with another in its place.
    void shareFollowsItsMonitor() {
        PeerAccess access{"key", "Viewer", "fp", {perm::ScreenView, perm::ScreenDirect}, true};
        QVector<QJsonObject> sent;
        ScreenService service([&](const auto&, const auto& msg) { sent.push_back(msg); return true; },
            [&](const auto&) { return access; }, [](const auto&) { return true; }, [](const auto&) {},
            [](const auto&, const auto&) { return true; });
        const auto monitor = [](const char* id, int x) {
            return QVariantMap{{"nativeId", id}, {"name", id}, {"width", 256}, {"height", 128}, {"inputX", x}, {"inputY", 0}};
        };
        ScreenSettings settings; settings.gpuCapture = false;
        settings.monitors = {monitor("A", 0), monitor("B", 256)};
        settings.monitor = 1;  // B
        service.configure(settings); service.setOffered(true);
        QSignalSpy hosting(&service, &ScreenService::hostingChanged), capture(&service, &ScreenService::captureChanged);
        service.handle(peer(1), request());
        QVERIFY(!hosting.back()[0].toList().isEmpty());

        settings.monitors = {monitor("B", 0)};  // A unplugged: B is now first
        settings.monitor = 0;
        service.configure(settings);
        QVERIFY(!hosting.back()[0].toList().isEmpty());
        QCOMPARE(sent.back().value("t").toString(), "scr.monitorChanged");
        QCOMPARE(sent.back().value("index").toInt(), 0);
        QCOMPARE(capture.back()[0].toBool(), true);
        QCOMPARE(capture.back()[1].toInt(), 0);

        settings.monitors = {monitor("C", 0)};  // B gone, another in its place
        service.configure(settings);
        QVERIFY(hosting.back()[0].toList().isEmpty());
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

    void twoRealSessionsSyntheticVideo_data() {
        QTest::addColumn<bool>("relay");
        QTest::newRow("punched") << false;
        QTest::newRow("fallback") << true;  // as when punching fails: relay, datagrams over TCP
    }
    void twoRealSessionsSyntheticVideo() {
        QFETCH(bool, relay);
        if (qEnvironmentVariableIsEmpty("UCD_TEST_OPENH264")) QSKIP("Install Cisco OpenH264 for synthetic end-to-end test");
        uconnect::server::RendezvousConfig config; config.port = 0;
        uconnect::server::Rendezvous server(config); QVERIFY(server.open());
        std::jthread serverThread([&](std::stop_token stop) { while (!stop.stop_requested()) server.poll_once(5ms); });
        Session host, viewer;
        const auto ih = Identity::generate(), iv = Identity::generate();
        host.setIdentity(ih); viewer.setIdentity(iv);
        AccessPolicy hp, vp;
        hp.special.insert(keyToHex(iv.publicKey()), {perm::ScreenView, perm::CameraView});
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
        options.forceRelay = relay;
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

    // The sending pipeline and the viewer's playout: frames offered faster
    // than asked are paced to the viewer's rate, converted on several threads
    // and encoded in order; the viewer shows them in order at their capture
    // pace (its playout buffer), hears how the host keeps up (scr.stats), and
    // the host keeps under the bitrate the viewer asked for.
    void pipelineKeepsOrderPaceAndReports() {
        if (qEnvironmentVariableIsEmpty("UCD_TEST_OPENH264")) QSKIP("Install Cisco OpenH264 for synthetic end-to-end test");
        uconnect::server::RendezvousConfig config; config.port = 0;
        uconnect::server::Rendezvous server(config); QVERIFY(server.open());
        std::jthread serverThread([&](std::stop_token stop) { while (!stop.stop_requested()) server.poll_once(5ms); });
        Session host, viewer;
        const auto ih = Identity::generate(), iv = Identity::generate();
        host.setIdentity(ih); viewer.setIdentity(iv);
        AccessPolicy hp;
        hp.special.insert(keyToHex(iv.publicKey()), {perm::ScreenView, perm::ScreenDirect});
        host.setAccessPolicy(hp); viewer.setAccessPolicy({});
        host.setLocalHello({"Host", "test", ServiceScreen}); viewer.setLocalHello({"Viewer", "test", 0});
        ScreenSettings settings; settings.ciscoLibrary = qEnvironmentVariable("UCD_TEST_OPENH264");
        settings.gpuCapture = false; settings.encoderThreads = 3; settings.queueFrames = 6;
        settings.monitors = {QVariantMap{{"name", "Synthetic"}, {"width", 320}, {"height", 240}}};
        host.screen()->configure(settings);
        QString hostId;
        connect(&host, &Session::snapshotChanged, this, [&](const auto& s) { if (!s.selfId.isEmpty()) hostId = s.selfId; });
        bool proven = false;
        connect(&viewer, &Session::peersChanged, this, [&](const auto& peers) {
            for (const auto& p : peers) if (p.identityState == "verified" && p.stateCode == 3) proven = true;
        });
        const auto creds = uconnect::TopicCreds::generate_keyed();
        Session::Options options{QString::fromStdString("127.0.0.1:" + std::to_string(server.port())), QString::fromStdString(creds.to_uri())};
        host.start(options); viewer.start(options);
        QTRY_VERIFY_WITH_TIMEOUT(proven && !hostId.isEmpty(), 20000);
        const auto remote = *uconnect::from_hex<uconnect::kDevIdLen>(hostId.toStdString());

        // Shown pictures: when, and which (each frame's grey level is its number).
        struct Shown { qint64 at; int level; };
        QVector<Shown> shown;
        QElapsedTimer clock; clock.start();
        connect(viewer.screen(), &ScreenService::frameReady, this, [&](const QString&, const QVideoFrame& frame) {
            const QImage image = QVideoFrame(frame).toImage();
            shown.push_back({clock.elapsed(), image.isNull() ? -1 : qGray(image.pixel(image.width() / 2, image.height() / 2))});
        });
        QVariantMap state;
        connect(viewer.screen(), &ScreenService::viewerChanged, this, [&](const QString&, const QVariantMap& update) {
            for (auto it = update.begin(); it != update.end(); ++it) state[it.key()] = it.value();
        });
        viewer.screen()->request(remote, false, 320, 20, 1000000, 150);  // 20 fps, 1 Mbit/s, 150 ms buffer
        QTRY_VERIFY_WITH_TIMEOUT(state.value("state") == "live", 5000);

        // Offered every 10 ms (100 fps): the host keeps one every 50 ms.
        int level = 0;
        QTimer source; source.setInterval(10);
        connect(&source, &QTimer::timeout, this, [&] {
            QImage image(320, 240, QImage::Format_RGBA8888);
            image.fill(QColor(level % 256, level % 256, level % 256));
            host.screen()->submitImage(image); level = (level + 1) % 250;
        });
        source.start();
        QTRY_VERIFY_WITH_TIMEOUT(shown.size() >= 20, 15000);
        const auto before = shown.size();
        QTest::qWait(3000);
        source.stop();

        // In order: the grey level never goes back (until it wraps).
        int backwards = 0;
        for (qsizetype i = before + 1; i < shown.size(); ++i)
            if (shown[i].level >= 0 && shown[i - 1].level >= 0 && shown[i].level < shown[i - 1].level && shown[i - 1].level - shown[i].level < 100)
                ++backwards;
        QCOMPARE(backwards, 0);
        // At the asked pace: about 20 a second, evenly.
        const auto count = shown.size() - before;
        QVERIFY2(count >= 45 && count <= 75, qPrintable(QString::number(count)));
        QVector<qint64> gaps;
        for (qsizetype i = before + 1; i < shown.size(); ++i) gaps.push_back(shown[i].at - shown[i - 1].at);
        std::sort(gaps.begin(), gaps.end());
        const qint64 median = gaps[gaps.size() / 2];
        QVERIFY2(median >= 35 && median <= 65, qPrintable(QString::number(median)));
        // The host's own report reached the viewer, within the asked bitrate.
        const auto hostStats = state.value("host").toMap();
        QVERIFY(hostStats.value("fps").toDouble() > 0);
        QVERIFY(hostStats.value("targetKbps").toDouble() <= 1000);
        QVERIFY(hostStats.value("threads").toInt() == 3);
        QCOMPARE(state.value("bufferMs").toInt(), 150);
        viewer.screen()->stop(remote);
        host.stop(); viewer.stop();
    }

    // Two devices watching each other at once: each is host and viewer of
    // the same peer. Stopping one direction leaves the other running.
    void twoRealSessionsWatchEachOther() {
        if (qEnvironmentVariableIsEmpty("UCD_TEST_OPENH264")) QSKIP("Install Cisco OpenH264 for synthetic end-to-end test");
        uconnect::server::RendezvousConfig config; config.port = 0;
        uconnect::server::Rendezvous server(config); QVERIFY(server.open());
        std::jthread serverThread([&](std::stop_token stop) { while (!stop.stop_requested()) server.poll_once(5ms); });
        Session a, b;
        const auto ia = Identity::generate(), ib = Identity::generate();
        a.setIdentity(ia); b.setIdentity(ib);
        AccessPolicy pa, pb;
        pa.special.insert(keyToHex(ib.publicKey()), {perm::ScreenView, perm::ScreenDirect});
        pb.special.insert(keyToHex(ia.publicKey()), {perm::ScreenView, perm::ScreenDirect});
        a.setAccessPolicy(pa); b.setAccessPolicy(pb);
        a.setLocalHello({"A", "test", ServiceScreen}); b.setLocalHello({"B", "test", ServiceScreen});
        ScreenSettings settings; settings.ciscoLibrary = qEnvironmentVariable("UCD_TEST_OPENH264"); settings.gpuCapture = false;
        settings.monitors = {QVariantMap{{"name", "Synthetic"}, {"width", 256}, {"height", 128}}};
        a.screen()->configure(settings); b.screen()->configure(settings);
        QString idA, idB;
        connect(&a, &Session::snapshotChanged, this, [&](const auto& s) { if (!s.selfId.isEmpty()) idA = s.selfId; });
        connect(&b, &Session::snapshotChanged, this, [&](const auto& s) { if (!s.selfId.isEmpty()) idB = s.selfId; });
        int proven = 0;
        connect(&a, &Session::peersChanged, this, [&](const auto& peers) { for (const auto& p : peers) if (p.identityState == "verified" && p.stateCode == 3) proven |= 1; });
        connect(&b, &Session::peersChanged, this, [&](const auto& peers) { for (const auto& p : peers) if (p.identityState == "verified" && p.stateCode == 3) proven |= 2; });
        const auto creds = uconnect::TopicCreds::generate_keyed();
        Session::Options options{QString::fromStdString("127.0.0.1:" + std::to_string(server.port())), QString::fromStdString(creds.to_uri())};
        a.start(options); b.start(options);
        QTRY_VERIFY_WITH_TIMEOUT(proven == 3, 20000);
        QTRY_VERIFY_WITH_TIMEOUT(!idA.isEmpty() && !idB.isEmpty(), 2000);
        const auto devA = *uconnect::from_hex<uconnect::kDevIdLen>(idA.toStdString());
        const auto devB = *uconnect::from_hex<uconnect::kDevIdLen>(idB.toStdString());
        QSignalSpy atA(a.screen(), &ScreenService::frameReady), atB(b.screen(), &ScreenService::frameReady);
        QStringList log;
        connect(a.screen(), &ScreenService::activity, this, [&](const QString& l) { log << "A: " + l; });
        connect(b.screen(), &ScreenService::activity, this, [&](const QString& l) { log << "B: " + l; });
        a.screen()->request(devB);  // A watches B
        QTest::qWait(200);
        b.screen()->request(devA);  // and B watches A
        QImage red(256, 128, QImage::Format_RGBA8888); red.fill(QColor(200, 40, 40));
        QImage blue(256, 128, QImage::Format_RGBA8888); blue.fill(QColor(40, 40, 200));
        for (int i = 0; i < 120 && (atA.isEmpty() || atB.isEmpty()); ++i) {
            a.screen()->submitImage(red); b.screen()->submitImage(blue); QTest::qWait(35);
        }
        QVERIFY2(!atA.isEmpty() && !atB.isEmpty(), "Both directions should deliver pictures");
        // A stops watching B: B keeps watching A.
        a.screen()->stopViewer(devB);  // as closing the viewer window does
        QTest::qWait(300);
        const auto before = atB.size();
        for (int i = 0; i < 60 && atB.size() == before; ++i) { a.screen()->submitImage(red); b.screen()->submitImage(blue); QTest::qWait(35); }
        QVERIFY2(atB.size() > before, qPrintable("Stopping one direction stopped the other: " + log.join(" | ")));
        b.screen()->stopViewer(devA);
        QTest::qWait(300);
        a.stop(); b.stop();
    }

    // An extended display over two real Sessions: the reversed handshake, a
    // (fake) virtual monitor, video on datagram kinds 4 and 5, and the monitor
    // removed when the display device stops.
    void twoRealSessionsExtendedDisplay() {
        if (qEnvironmentVariableIsEmpty("UCD_TEST_OPENH264")) QSKIP("Install Cisco OpenH264 for synthetic end-to-end test");
        static std::atomic_int monitors{0};
        struct Fake final : display::VirtualDisplay {
            ~Fake() override { destroy(); }
            bool create(const QByteArray&, const QString&, const display::Mode& m, QString*) override { mode = m; made = true; ++monitors; return true; }
            bool setMode(const display::Mode& m, QString*) override { mode = m; return true; }
            void destroy() override { if (made) { made = false; --monitors; } }
            void keepAlive() override {}
            QString captureId() const override { return "fake"; }
            QRect geometry() const override { return {0, 0, mode.width, mode.height}; }
            QSize pixels() const override { return {mode.width, mode.height}; }
            display::Mode mode; bool made = false;
        };
        uconnect::server::RendezvousConfig config; config.port = 0;
        uconnect::server::Rendezvous server(config); QVERIFY(server.open());
        std::jthread serverThread([&](std::stop_token stop) { while (!stop.stop_requested()) server.poll_once(5ms); });
        Session host, shower;
        const auto ih = Identity::generate(), is = Identity::generate();
        host.setIdentity(ih); shower.setIdentity(is);
        AccessPolicy hp, sp;
        hp.special.insert(keyToHex(is.publicKey()), {});  // the host asks only devices its rules cover
        sp.special.insert(keyToHex(ih.publicKey()), {perm::DisplayShow, perm::DisplayDirect});
        host.setAccessPolicy(hp); shower.setAccessPolicy(sp);
        host.setLocalHello({"Host", "test", 0}); shower.setLocalHello({"Tablet", "test", ServiceDisplay});
        host.displays()->setBackend([] { return std::make_unique<Fake>(); }, [] { return display::Status{display::Availability::Ready, {}}; });
        host.displays()->setNativeCapture(false);
        ScreenSettings codec; codec.ciscoLibrary = qEnvironmentVariable("UCD_TEST_OPENH264");
        host.displays()->setCodecSettings(codec);
        shower.displays()->setLocalScreens({QVariantMap{{"width", 640}, {"height", 480}, {"fps", 30}, {"dpi", 96}}});
        QString hostId, showerId;
        connect(&host, &Session::snapshotChanged, this, [&](const auto& s) { if (!s.selfId.isEmpty()) hostId = s.selfId; });
        connect(&shower, &Session::snapshotChanged, this, [&](const auto& s) { if (!s.selfId.isEmpty()) showerId = s.selfId; });
        int proven = 0;
        connect(&host, &Session::peersChanged, this, [&](const auto& peers) {
            for (const auto& p : peers) if (p.identityState == "verified" && p.stateCode == 3) proven |= 1;
        });
        connect(&shower, &Session::peersChanged, this, [&](const auto& peers) {
            for (const auto& p : peers) if (p.identityState == "verified" && p.stateCode == 3) proven |= 2;
        });
        const auto creds = uconnect::TopicCreds::generate_keyed();
        Session::Options options{QString::fromStdString("127.0.0.1:" + std::to_string(server.port())), QString::fromStdString(creds.to_uri())};
        host.start(options); shower.start(options);
        QTRY_VERIFY_WITH_TIMEOUT(proven == 3, 20000);
        QTRY_VERIFY_WITH_TIMEOUT(!hostId.isEmpty() && !showerId.isEmpty(), 2000);
        const auto showerDev = *uconnect::from_hex<uconnect::kDevIdLen>(showerId.toStdString());
        const auto hostDev = *uconnect::from_hex<uconnect::kDevIdLen>(hostId.toStdString());
        QSignalSpy hostState(host.displays(), &display::DisplayService::hostStateChanged);
        QSignalSpy shown(shower.displays(), &display::DisplayService::showRequested);
        QSignalSpy images(shower.display(), &ScreenService::frameReady);
        host.displays()->offer(showerDev, {});
        QTRY_VERIFY_WITH_TIMEOUT(!hostState.isEmpty() && hostState.back()[1].toMap().value("state") == "live", 5000);
        QCOMPARE(shown.size(), 1);
        QCOMPARE(monitors.load(), 1);
        QImage synthetic(640, 480, QImage::Format_RGBA8888); synthetic.fill(QColor(200, 60, 30));
        for (int i = 0; i < 90 && images.isEmpty(); ++i) { host.display()->submitImage(synthetic); QTest::qWait(35); }
        QVERIFY2(!images.isEmpty(), "No extended display frame crossed the real Session datagram channel");
        QCOMPARE(qvariant_cast<QVideoFrame>(images.back()[1]).size(), synthetic.size());
        shower.displays()->stop(hostDev);
        QTRY_VERIFY_WITH_TIMEOUT(hostState.back()[1].toMap().value("state") == "ended", 3000);
        QCOMPARE(monitors.load(), 0);
        host.stop(); shower.stop();
    }
};
QTEST_GUILESS_MAIN(ScreenSessionTest)
#include "ScreenSessionTest.moc"
