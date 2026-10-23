// Streaming benchmark, opt in with UCD_BENCH_ROLE: the real screen-sharing
// pipeline (Session, encoder, paced datagrams, reassembly, decoder) carrying
// an animated synthetic desktop, measured where the viewer shows it.
//
//   UCD_BENCH_ROLE     both   host and viewer here, over a local rendezvous
//                      topic  print a fresh topic link for the two below
//                      host   only the host (another machine views it)
//                      viewer only the viewer (another machine hosts)
//   UCD_BENCH_SERVER   rendezvous host:port (host/viewer roles)
//   UCD_BENCH_TOPIC    the topic link both machines use (host/viewer roles)
//   UCD_BENCH_SECONDS  how long to measure (default 30)
//   UCD_BENCH_FPS, UCD_BENCH_WIDTH, UCD_BENCH_BITRATE   what to ask for / allow
//                      (defaults 60, 1920, 8000000)
//   UCD_BENCH_BUFFER   the viewer's playout buffer in ms (default 100; 0 shows at once)
//   UCD_BENCH_ASK_BITRATE  the bitrate the viewer asks for (default 0: the host's limit)
//   UCD_BENCH_SNAPSHOTS    a folder: every 60th picture the viewer shows is saved there
//   UCD_BENCH_NO_UDP       1: no punched UDP; media takes the TCP fallback
//   UCD_BENCH_IDENTITY     a file keeping the viewer's identity across runs
//   UCD_BENCH_HOST_NAME    the device the viewer watches (default "Bench host"; e.g. a phone)
//   UCD_TEST_OPENH264  Cisco's library, for a system without a hardware encoder
//
// Per-second [stats] lines from the services (UCD_SCREEN_STATS) show each
// stage; the viewer ends with a summary: frames shown per second and the gaps
// between them (a gap over twice the frame interval is a visible stutter).
#include <QtTest>

#include <QElapsedTimer>
#include <QTimer>
#include <QPainter>
#include <QVideoFrame>

#include <algorithm>
#include <cmath>
#include <thread>

#include "core/Identity.hpp"
#include "core/Permissions.hpp"
#include "core/Session.hpp"
#include "core/screen/ScreenService.hpp"
#include "rendezvous.hpp"

using namespace ucd;
using namespace ucd::screen;
using namespace std::chrono_literals;

namespace {

int envInt(const char* name, int fallback) {
    bool ok = false;
    const int value = qEnvironmentVariableIntValue(name, &ok);
    return ok ? value : fallback;
}

// A desktop that keeps changing everywhere: sliding colour bars, bouncing
// boxes and a frame counter, like a video or a fast scroll.
QImage drawFrame(int n, int width, int height) {
    QImage image(width, height, QImage::Format_RGB32);
    QPainter p(&image);
    p.fillRect(image.rect(), QColor(27, 31, 42));
    for (int i = 0; i < 12; ++i) {
        const int x = (n * 6 + i * width / 12) % (width + 200) - 100;
        p.fillRect(x, 0, 60, height, QColor::fromHsv((i * 30 + n * 2) % 360, 180, 200));
    }
    for (int i = 0; i < 20; ++i) {
        const double x = (std::sin(n / 40.0 + i) * .45 + .5) * width, y = (std::cos(n / 50.0 + i * 1.7) * .45 + .5) * height;
        p.fillRect(QRectF(x - 40, y - 40, 80, 80), QColor::fromHsv((i * 18) % 360, 200, 230));
    }
    p.setPen(Qt::white);
    p.setFont(QFont("monospace", 80, QFont::Bold));
    p.drawText(60, 160, QString::number(n));
    return image;
}

double percentile(QVector<double> values, double q) {
    if (values.isEmpty()) return 0;
    std::sort(values.begin(), values.end());
    return values[std::min<qsizetype>(values.size() - 1, qsizetype(q * values.size()))];
}

}  // namespace

class StreamBenchTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { qputenv("UCD_SCREEN_STATS", "1"); }

    void stream() {
        const QString role = qEnvironmentVariable("UCD_BENCH_ROLE");
        if (role.isEmpty()) QSKIP("set UCD_BENCH_ROLE to both, host, viewer or topic");
        if (role == "topic") {  // a fresh topic for host and viewer on two machines
            qInfo().noquote() << "[bench] topic" << QString::fromStdString(uconnect::TopicCreds::generate_keyed().to_uri());
            return;
        }
        const int seconds = envInt("UCD_BENCH_SECONDS", 30), fps = envInt("UCD_BENCH_FPS", 60);
        const int width = envInt("UCD_BENCH_WIDTH", 1920), bitrate = envInt("UCD_BENCH_BITRATE", 8000000);
        const int height = width * 9 / 16;
        const bool hosting = role == "both" || role == "host", viewing = role == "both" || role == "viewer";

        // Where to meet.
        std::unique_ptr<uconnect::server::Rendezvous> server;
        std::jthread serverThread;
        QString address = qEnvironmentVariable("UCD_BENCH_SERVER"), topic = qEnvironmentVariable("UCD_BENCH_TOPIC");
        if (role == "both") {
            uconnect::server::RendezvousConfig config; config.port = 0;
            server = std::make_unique<uconnect::server::Rendezvous>(config);
            QVERIFY(server->open());
            serverThread = std::jthread([&](std::stop_token stop) { while (!stop.stop_requested()) server->poll_once(5ms); });
            address = QString::fromStdString("127.0.0.1:" + std::to_string(server->port()));
            topic = QString::fromStdString(uconnect::TopicCreds::generate_keyed().to_uri());
        }
        QVERIFY2(!address.isEmpty() && !topic.isEmpty(), "set UCD_BENCH_SERVER and UCD_BENCH_TOPIC");
        Session::Options options{address, topic};
        // UCD_BENCH_NO_UDP=1: no punched UDP, so media goes over the TCP fallback.
        options.forceRelay = qEnvironmentVariableIntValue("UCD_BENCH_NO_UDP") == 1;

        // The host: any device in the topic may view its synthetic desktop without a prompt.
        std::unique_ptr<Session> host;
        QTimer source;
        QImage canvas;
        int frameNumber = 0;
        if (hosting) {
            host = std::make_unique<Session>();
            host->setIdentity(Identity::generate());
            AccessPolicy policy;
            policy.defaults = {perm::ScreenView, perm::ScreenDirect};
            host->setAccessPolicy(policy);
            host->setLocalHello({"Bench host", QSysInfo::productType(), ServiceScreen});
            ScreenSettings settings;
            settings.offered = true; settings.gpuCapture = false;
            settings.maxFps = fps; settings.maxWidth = width; settings.maxBitrate = bitrate;
            settings.ciscoLibrary = qEnvironmentVariable("UCD_TEST_OPENH264");
            settings.monitors = {QVariantMap{{"name", "Synthetic"}, {"width", width}, {"height", height}}};
            host->screen()->configure(settings);
            connect(host.get(), &Session::logMessage, this, [](const QString& line) { qInfo().noquote() << "[host]" << line; });
            host->start(options);
            // Frames are offered at the target rate; the service encodes what it
            // can. They pan across a canvas drawn once, so making one costs a
            // copy, not a redraw, and the whole picture still changes each time.
            canvas = drawFrame(0, width * 2, height);
            QElapsedTimer cost; cost.start();
            const auto frameAt = [&](int n) { return canvas.copy((n * 12) % width, 0, width, height); };
            for (int i = 0; i < 30; ++i) frameAt(i);
            qInfo().noquote() << QStringLiteral("[bench] a source frame costs %1 ms").arg(cost.elapsed() / 30., 0, 'f', 2);
            source.setTimerType(Qt::PreciseTimer);
            // Offered a bit faster than asked, as a capture source does: the
            // service paces them to the target itself.
            source.setInterval(std::max(1, 1000 / (fps + fps / 2)));
            connect(&source, &QTimer::timeout, this, [&, frameAt] { host->screen()->submitImage(frameAt(frameNumber++)); });
            source.start();
        }

        if (!viewing) {
            qInfo().noquote() << QStringLiteral("[bench] hosting %1x%2 at up to %3 fps, %4 kbit/s, for %5 s")
                                     .arg(width).arg(height).arg(fps).arg(bitrate / 1000).arg(seconds);
            QTest::qWait(seconds * 1000);
            return;
        }

        // The viewer: finds the bench host, asks for its screen and times every frame shown.
        Session viewer;
        // UCD_BENCH_IDENTITY=<file>: the same identity every run, so a device's
        // special rules for "Bench viewer" keep applying.
        const QString identityFile = qEnvironmentVariable("UCD_BENCH_IDENTITY");
        viewer.setIdentity(identityFile.isEmpty() ? Identity::generate() : Identity::loadOrCreate(identityFile, nullptr));
        viewer.setAccessPolicy({});
        viewer.setLocalHello({"Bench viewer", QSysInfo::productType(), 0});
        QString hostId;
        const QString hostName = qEnvironmentVariable("UCD_BENCH_HOST_NAME", QStringLiteral("Bench host"));
        connect(&viewer, &Session::peersChanged, this, [&, hostName](const auto& peers) {
            for (const auto& p : peers)
                if (p.name == hostName && p.identityState == "verified" && p.stateCode == 3) hostId = p.devId;
        });
        viewer.start(options);
        QTRY_VERIFY_WITH_TIMEOUT(!hostId.isEmpty(), 30000);

        QElapsedTimer clock; clock.start();
        QVector<qint64> shown;
        QVariantMap lastState;
        // UCD_BENCH_SNAPSHOTS=<folder>: every 60th picture shown, as a PNG.
        const QString snapshots = qEnvironmentVariable("UCD_BENCH_SNAPSHOTS");
        connect(viewer.screen(), &ScreenService::frameReady, this, [&, snapshots](const QString&, const QVideoFrame& frame) {
            shown.push_back(clock.elapsed());
            if (!snapshots.isEmpty() && shown.size() % 60 == 0)
                QVideoFrame(frame).toImage().save(QStringLiteral("%1/frame-%2.png").arg(snapshots).arg(shown.size()));
        });
        connect(viewer.screen(), &ScreenService::viewerChanged, this, [&](const QString&, const QVariantMap& update) {
            for (auto it = update.begin(); it != update.end(); ++it) lastState[it.key()] = it.value();
            if (update.contains("state"))
                qInfo().noquote() << "[viewer] state" << update.value("state").toString() << update.value("detail").toString();
        });
        const auto ask = [&] {
            viewer.screen()->request(*uconnect::from_hex<uconnect::kDevIdLen>(hostId.toStdString()), false, width, fps,
                                     envInt("UCD_BENCH_ASK_BITRATE", 0), envInt("UCD_BENCH_BUFFER", 100));
        };
        ask();
        // Refused or ended (e.g. while a phone is still being given the rights): ask again.
        QTimer retry; retry.setInterval(5000);
        connect(&retry, &QTimer::timeout, this, [&] {
            const auto state = lastState.value("state").toString();
            if (shown.isEmpty() && (state == "denied" || state == "busy" || state == "ended" || state == "failed")) ask();
        });
        retry.start();
        QTRY_VERIFY_WITH_TIMEOUT(!shown.isEmpty(), envInt("UCD_BENCH_FIRST_MS", 20000));

        // Measure after a few seconds of ramp-up, a line per second.
        QTest::qWait(3000);
        const qint64 from = clock.elapsed();
        for (int s = 0; s < seconds; ++s) {
            QTest::qWait(1000);
            const qint64 end = clock.elapsed();
            const auto count = std::count_if(shown.begin(), shown.end(), [&](qint64 t) { return t > end - 1000 && t <= end; });
            qInfo().noquote() << QStringLiteral("[bench] second %1: shown %2 fps, path %3, %4 kbit/s, loss %5%, jitter %6 ms, decode %7 ms")
                                     .arg(s + 1).arg(count).arg(lastState.value("path").toString())
                                     .arg(lastState.value("bitrate").toDouble() / 1000, 0, 'f', 0)
                                     .arg(lastState.value("loss").toDouble() * 100, 0, 'f', 1)
                                     .arg(lastState.value("jitterMs").toDouble(), 0, 'f', 1)
                                     .arg(lastState.value("decodeMs").toDouble(), 0, 'f', 1)
                              + QStringLiteral(", decoded %1 fps, waiting %2 (%3 ms), late %4, skipped %5")
                                     .arg(lastState.value("fps").toDouble(), 0, 'f', 1).arg(lastState.value("waitingFrames").toInt())
                                     .arg(lastState.value("waitingMs").toDouble(), 0, 'f', 0).arg(lastState.value("late").toInt())
                                     .arg(lastState.value("skipped").toInt())
                              + QStringLiteral(", host sends over %1, lag %2 ms, host %3 fps %4 kbit/s")
                                     .arg(lastState.value("host").toMap().value("transport").toString())
                                     .arg(lastState.value("host").toMap().value("lagMs").toDouble(), 0, 'f', 0)
                                     .arg(lastState.value("host").toMap().value("fps").toDouble(), 0, 'f', 0)
                                     .arg(lastState.value("host").toMap().value("sentKbps").toDouble(), 0, 'f', 0)
                              + QStringLiteral(" (target %1 kbit/s %2 fps %3 px, %4, rtt %5 ms, loss %6%, fec %7, repeats %8, dropped %9/s)")
                                     .arg(lastState.value("host").toMap().value("targetKbps").toDouble(), 0, 'f', 0)
                                     .arg(lastState.value("host").toMap().value("targetFps").toDouble(), 0, 'f', 0)
                                     .arg(lastState.value("host").toMap().value("width").toDouble(), 0, 'f', 0)
                                     .arg(lastState.value("host").toMap().value("estimator").toString())
                                     .arg(lastState.value("host").toMap().value("rttMs").toDouble(), 0, 'f', 0)
                                     .arg(lastState.value("host").toMap().value("lossPct").toDouble(), 0, 'f', 1)
                                     .arg(lastState.value("host").toMap().value("fec").toDouble(), 0, 'f', 0)
                                     .arg(lastState.value("host").toMap().value("repeats").toDouble(), 0, 'f', 0)
                                     .arg(lastState.value("host").toMap().value("dropped").toDouble(), 0, 'f', 1);
        }
        const qint64 to = clock.elapsed();
        QVector<double> gaps;
        qint64 previous = -1;
        int frames = 0;
        for (qint64 t : shown) {
            if (t < from || t > to) continue;
            if (previous >= 0) gaps.push_back(double(t - previous));
            previous = t; ++frames;
        }
        const double interval = 1000. / fps;
        const auto stutters = std::count_if(gaps.begin(), gaps.end(), [&](double g) { return g > 2 * interval; });
        const auto freezes = std::count_if(gaps.begin(), gaps.end(), [](double g) { return g > 250; });
        qInfo().noquote() << QStringLiteral("[bench] summary: %1 fps shown of %2 asked over %3 s; gap p50 %4 ms, p95 %5 ms, p99 %6 ms, max %7 ms;"
                                            " stutters (> 2 frames) %8, freezes (> 250 ms) %9")
                                 .arg(frames * 1000. / std::max<qint64>(1, to - from), 0, 'f', 1).arg(fps).arg((to - from) / 1000)
                                 .arg(percentile(gaps, .5), 0, 'f', 1).arg(percentile(gaps, .95), 0, 'f', 1)
                                 .arg(percentile(gaps, .99), 0, 'f', 1).arg(percentile(gaps, 1.), 0, 'f', 0)
                                 .arg(stutters).arg(freezes);
        // The services report while they shut down: not into these locals.
        disconnect(viewer.screen(), nullptr, this, nullptr);
        disconnect(&viewer, nullptr, this, nullptr);
        QVERIFY(frames > 0);
    }
};

QTEST_MAIN(StreamBenchTest)
#include "StreamBenchTest.moc"
