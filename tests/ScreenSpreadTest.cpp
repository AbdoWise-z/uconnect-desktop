#include <QtTest>
#include <QGuiApplication>
#include <QScreen>
#include <QJsonArray>
#include <QSignalSpy>
#include <utility>
#ifdef Q_OS_MACOS
#include <ApplicationServices/ApplicationServices.h>
#endif
#include "core/screen/ScreenService.hpp"

using namespace ucd;
using namespace ucd::screen;
class ScreenSpreadTest : public QObject {
    Q_OBJECT
    static uconnect::DevId peer(int n) { uconnect::DevId p{}; p[0] = uint8_t(n); return p; }
    static QJsonObject request(const char* type, int id, int width = 640) {
        return {{"t", type}, {"request", id}, {"maxWidth", width}, {"codecs", QJsonArray{"h264"}}};
    }
private slots:
    void sharedEncoderAndIndependentStops() {
        std::map<uconnect::DevId, QVector<QJsonObject>> messages;
        std::map<uconnect::DevId, QVector<QByteArray>> packets;
        ScreenService service([&](const auto& p, const auto& msg) { messages[p].push_back(msg); return true; },
            [](const auto&) { return PeerAccess{"key", "Viewer", "fp", {perm::ScreenView, perm::ScreenDirect}, true}; },
            [](const auto&) { return true; }, [](const auto&) {},
            [&](const auto& p, const auto& data) { packets[p].push_back(data); return true; });
        ScreenSettings settings; settings.maxViewers = 2; settings.ciscoLibrary = qEnvironmentVariable("UCD_TEST_OPENH264");
        settings.monitors = {QVariantMap{{"name", "Synthetic"}, {"width", 800}, {"height", 400}}};
        service.configure(settings); service.setOffered(true);
        QSignalSpy capture(&service, &ScreenService::captureChanged);
        service.handle(peer(1), request("scr.request", 1, 640));
        service.handle(peer(2), request("scr.request", 2, 320)); QCOMPARE(capture.size(), 1);
        service.handle(peer(3), request("scr.request", 3)); QCOMPARE(messages[peer(3)].back().value("code"), "busy");
        MediaReceiver first, second;
        first.reset(messages[peer(1)].front().value("session").toInt(), 1, 30);
        second.reset(messages[peer(2)].front().value("session").toInt(), 1, 30);
        QImage source(800,400,QImage::Format_RGBA8888); source.fill(QColor(45,90,160));
        QVector<EncodedFrame> a, b;
        for (int attempt = 0; attempt < 35 && (a.isEmpty() || b.isEmpty()); ++attempt) {
            service.submitImage(source); QTest::qWait(40); service.tick();
            for (const auto& packet : std::exchange(packets[peer(1)], {})) a += first.receive(packet, ScreenService::monotonicMs());
            for (const auto& packet : std::exchange(packets[peer(2)], {})) b += second.receive(packet, ScreenService::monotonicMs());
        }
        QVERIFY2(!a.isEmpty() && !b.isEmpty(), "Shared encoded video did not reach both viewers");
        QCOMPARE(a.front().bytes, b.front().bytes); QCOMPARE(a.front().number, b.front().number);
        Decoder decoder; QCOMPARE(decoder.decode(a.front()).size(), QSize(320,160));
        service.peerGone(peer(2)); QCOMPARE(capture.size(), 1); // remaining viewer keeps capture
        service.peerGone(peer(1)); QCOMPARE(capture.size(), 2); QVERIFY(!capture.back()[0].toBool());
    }
    void cameraConsentAndWireIsolation() {
        QVector<QJsonObject> sent; QVector<QByteArray> packets;
        PeerAccess access{"key", "Viewer", "fp", {perm::CameraView, perm::ScreenDirect, perm::ControlDirect}, true};
        ScreenService service([&](const auto&, const auto& msg) { sent.push_back(msg); return true; },
            [&](const auto&) { return access; }, [](const auto&) { return true; }, [](const auto&) {},
            [&](const auto&, const auto& data) { packets.push_back(data); return true; }, nullptr, {}, {}, Kind::Camera);
        ScreenSettings settings; settings.ciscoLibrary = qEnvironmentVariable("UCD_TEST_OPENH264"); service.configure(settings); service.setOffered(true);
        QSignalSpy prompt(&service, &ScreenService::consentRequested);
        QVERIFY(!service.handle(peer(1), request("scr.request", 1)));
        access.trusted = false; service.handle(peer(1), request("cam.request", 1)); QCOMPARE(sent.back().value("code"), "forbidden");
        access.trusted = true; auto control = request("cam.request", 2); control["wantControl"] = true;
        service.handle(peer(1), control); QVERIFY(sent.back().value("code").toString().contains("forbidden"));
        service.handle(peer(1), request("cam.request", 3)); QCOMPARE(prompt.size(), 1); QCOMPARE(sent.back().value("t"), "cam.wait");
        service.consent(QString::fromStdString(uconnect::to_hex(peer(1))), 3, true, true);
        QCOMPARE(sent.back().value("t"), "cam.started"); QVERIFY(!sent.back().value("control").toBool());
        QImage image(320,160,QImage::Format_RGBA8888); image.fill(Qt::darkGreen);
        for (int i = 0; i < 25 && packets.isEmpty(); ++i) { service.submitImage(image); QTest::qWait(40); service.tick(); }
        QVERIFY(!packets.isEmpty()); for (const auto& packet : packets) QCOMPARE(packet.front(), char(3));
    }
    void nativeDesktop_data() {
        QTest::addColumn<QString>("encoder");
#ifdef Q_OS_WIN
        QTest::newRow("NVENC") << QString("h264_nvenc");
        QTest::newRow("QSV") << QString("h264_qsv");
        QTest::newRow("AMF") << QString("h264_amf");
#else
        QTest::newRow("native") << QString();
#endif
    }
    void nativeDesktop() {
        if (!qEnvironmentVariableIsSet("UCD_TEST_DESKTOP_CAPTURE")) QSKIP("Enable real desktop capture explicitly");
        auto native = desktopEncoder(); if (!native) QSKIP("No native GPU capture backend on this platform");
        QFETCH(QString, encoder);
        CodecOptions options{640,30,3000000,encoder,{}};
        QString monitor = QGuiApplication::primaryScreen()->name();
#ifdef Q_OS_MACOS
        monitor = QString::number(CGMainDisplayID());
#endif
        Decoder decoder; QImage image; QString pipeline;
        for (int i = 0; i < 30 && image.isNull(); ++i) {
            const auto result = native->encode(monitor, options, i, quint32(ScreenService::monotonicMs()), i == 0);
            if (!result.active) QSKIP(qPrintable(result.detail.isEmpty() ? QString("Native hardware encoder unavailable") : result.detail));
            pipeline = result.pipeline;
            if (result.frame) image = decoder.decode(*result.frame);
            QTest::qWait(35);
        }
        QVERIFY2(!image.isNull(), qPrintable(decoder.error())); QCOMPARE(image.width(), 640);
        const auto screen = QGuiApplication::primaryScreen();
        QCOMPARE(image.height(), int(qint64(screen->size().height()) * 640 / screen->size().width()) & ~1);
        // A small bitrate step right away is held back (it would cost a
        // keyframe): frames keep coming, the unchanged desktop encoded again.
        options.bitrate = 2400000;
        int frames = 0; bool keyframes = false;
        for (quint32 i = 100; i < 110; ++i) {
            const auto result = native->encode(monitor, options, i, quint32(ScreenService::monotonicMs()), false);
            QVERIFY(result.active);
            if (result.frame) { ++frames; keyframes |= result.frame->keyframe; image = decoder.decode(*result.frame); }
            QTest::qWait(35);
        }
        QVERIFY(frames > 0); QVERIFY(!image.isNull());
        QVERIFY(!keyframes);
        // A new width reopens only the encoder: the duplicated desktop is
        // reused, so a picture comes at once even if nothing moves.
        options.width = 480; image = {};
        for (quint32 i = 200; i < 205 && image.isNull(); ++i) {
            const auto result = native->encode(monitor, options, i, quint32(ScreenService::monotonicMs()), false);
            QVERIFY(result.active);
            if (result.frame) image = decoder.decode(*result.frame);
        }
        QCOMPARE(image.width(), 480);
        qInfo().noquote() << "Verified native pipeline:" << pipeline;
    }
};
QTEST_MAIN(ScreenSpreadTest)
#include "ScreenSpreadTest.moc"
