#include <QtTest>
#include <QGuiApplication>
#include <QProcess>
#include "core/display/VirtualDisplay.hpp"
#include "core/screen/Codec.hpp"
#include "core/screen/DesktopEncoder.hpp"
#include "core/screen/ScreenService.hpp"
#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <CoreGraphics/CoreGraphics.h>
#endif

using namespace ucd;
using namespace ucd::screen;

// Real virtual monitors (the uConnect driver on Windows, CGVirtualDisplay on
// macOS), captured and encoded the way an extended display session does.
// Opt in with UCD_TEST_VIRTUAL_DISPLAY=1: it plugs monitors into this desktop.
class VirtualDisplayTest : public QObject {
    Q_OBJECT

    static bool exists(const QString& captureId) {
#ifdef Q_OS_WIN
        DEVMODEW mode{}; mode.dmSize = sizeof(mode);
        return EnumDisplaySettingsExW(reinterpret_cast<const wchar_t*>(captureId.utf16()), ENUM_CURRENT_SETTINGS, &mode, 0);
#else
        // CoreGraphics in a long-running process can keep a stale display
        // list; a fresh process sees the window server's. Test monitors are
        // all named "uConnect test", and one exists at a time.
        if (captureId.isEmpty()) return false;
        QProcess profiler;
        profiler.start("system_profiler", {"SPDisplaysDataType"});
        profiler.waitForFinished(15000);
        return profiler.readAllStandardOutput().contains("uConnect test:");
#endif
    }
    // Encodes the monitor until a frame decodes; returns its size.
    static QSize capture(const QString& captureId, int width) {
        auto native = desktopEncoder();
        if (!native) return {};
        CodecOptions options{width, 60, 20000000, {}, {}};
        Decoder decoder;
        for (int i = 0; i < 60; ++i) {
            const auto result = native->encode(captureId, options, quint32(i), quint32(ScreenService::monotonicMs()), i == 0);
            if (!result.active && i == 59) qWarning().noquote() << "capture:" << result.detail << result.pipeline;
            if (result.frame) {
                const auto image = decoder.decode(*result.frame);
                if (!image.isNull()) return image.size();
            }
            QTest::qWait(30);
        }
        return {};
    }

private slots:
    void initTestCase() {
        if (!qEnvironmentVariableIsSet("UCD_TEST_VIRTUAL_DISPLAY")) QSKIP("Opt in with UCD_TEST_VIRTUAL_DISPLAY=1: it adds monitors to this desktop");
        const auto status = display::status();
        if (status.availability != display::Availability::Ready) QSKIP(qPrintable(status.detail));
    }

    void createCaptureChangeAndRemove() {
        auto monitor = display::create();
        QVERIFY(monitor);
        QString error;
        const auto identity = display::monitorIdentity(QString(64, 'a'));
        QVERIFY2(monitor->create(identity, "uConnect test", {1920, 1080, 60000, false}, &error), qPrintable(error));
        const QString id = monitor->captureId();
        QVERIFY(!id.isEmpty()); QVERIFY(exists(id));
        QCOMPARE(monitor->pixels(), QSize(1920, 1080));
        const auto info = display::monitorInfo(*monitor, "Test");
        QCOMPARE(info.value("nativeId").toString(), id);
        QCOMPARE(info.value("width").toInt(), 1920);
        QCOMPARE(capture(id, 1920), QSize(1920, 1080));

        QVERIFY2(monitor->setMode({2560, 1440, 60000, false}, &error), qPrintable(error));
        QCOMPARE(monitor->pixels(), QSize(2560, 1440));
        QCOMPARE(capture(id, 2560), QSize(2560, 1440));

        monitor->keepAlive();
        monitor->destroy();
        QTRY_VERIFY_WITH_TIMEOUT(!exists(id), 8000);
    }

    // The same device comes back as the same monitor.
    void sameIdentitySameMonitor() {
        const auto identity = display::monitorIdentity(QString(64, 'b'));
        QString first, second, error;
        {
            auto monitor = display::create();
            QVERIFY2(monitor->create(identity, "uConnect test", {1920, 1080, 60000, false}, &error), qPrintable(error));
            first = monitor->captureId();
        }  // the destructor removes it
        QTRY_VERIFY_WITH_TIMEOUT(!exists(first), 8000);
        auto monitor = display::create();
        QVERIFY2(monitor->create(identity, "uConnect test", {1920, 1080, 60000, false}, &error), qPrintable(error));
        second = monitor->captureId();
        QVERIFY(!second.isEmpty());
        monitor->destroy();
        QTRY_VERIFY_WITH_TIMEOUT(!exists(second), 8000);
    }

    // A process that dies takes its monitor with it.
    void killedOwnerRemovesTheMonitor() {
        QProcess child;
        child.setProcessChannelMode(QProcess::MergedChannels);
        child.start(QCoreApplication::applicationFilePath(), {"--owner"});
        QVERIFY(child.waitForStarted());
        QString output, id;
        for (int i = 0; i < 300 && id.isEmpty() && child.state() == QProcess::Running; ++i) {
            child.waitForReadyRead(50);
            output += QString::fromLatin1(child.readAll());
            id = output.section("MONITOR ", 1, 1).section('\n', 0, 0).trimmed();
        }
        QVERIFY2(!id.isEmpty(), qPrintable(QString("child said: %1 (exit %2)").arg(output).arg(child.exitCode())));
        QVERIFY(exists(id));
        child.kill();
        child.waitForFinished();
        QTRY_VERIFY_WITH_TIMEOUT(!exists(id), 8000);
    }
};

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    // The child half of killedOwnerRemovesTheMonitor: make a monitor, say
    // which, keep it alive until killed.
    if (app.arguments().contains("--owner")) {
        auto monitor = display::create();
        QString error;
        if (!monitor || !monitor->create(display::monitorIdentity(QString(64, 'c')), "uConnect test", {1280, 720, 60000, false}, &error))
            return 1;
        printf("MONITOR %s\n", qPrintable(monitor->captureId()));
        fflush(stdout);
        QTimer ping;
        QObject::connect(&ping, &QTimer::timeout, [&] { monitor->keepAlive(); });
        ping.start(500);
        return app.exec();
    }
    // Diagnosis: make a monitor, remove it after 3 s, stay alive 5 s more, so
    // another process can watch it go while this one still runs.
    if (app.arguments().contains("--destroy-check")) {
        auto monitor = display::create();
        QString error;
        if (!monitor || !monitor->create(display::monitorIdentity(QString(64, 'd')), "uConnect test", {1280, 720, 60000, false}, &error))
            return 1;
        printf("MONITOR %s\n", qPrintable(monitor->captureId())); fflush(stdout);
        QTimer::singleShot(3000, [&] { monitor->destroy(); printf("DESTROYED\n"); fflush(stdout); });
        QTimer::singleShot(8000, &app, &QCoreApplication::quit);
        return app.exec();
    }
    VirtualDisplayTest test;
    QStringList args = app.arguments();
    return QTest::qExec(&test, args);
}

#include "VirtualDisplayTest.moc"
