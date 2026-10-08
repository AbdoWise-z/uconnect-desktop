#include <QtTest>
#include <QGuiApplication>
#include <QMediaCaptureSession>
#include <QScreenCapture>
#include <QVideoSink>
#include <QVideoFrame>
#include "core/screen/Codec.hpp"

class ScreenCaptureTest : public QObject {
    Q_OBJECT
private slots:
    void realDesktopCapture() {
        if (!qEnvironmentVariableIsSet("UCD_TEST_DESKTOP_CAPTURE")) QSKIP("Opt in on a real desktop with UCD_TEST_DESKTOP_CAPTURE=1");
        QMediaCaptureSession session;
        QScreenCapture capture;
        QVideoSink sink;
        session.setScreenCapture(&capture); session.setVideoSink(&sink);
        capture.setScreen(QGuiApplication::primaryScreen());
        QSignalSpy errors(&capture, &QScreenCapture::errorOccurred), frames(&sink, &QVideoSink::videoFrameChanged);
        capture.start();
        QTRY_VERIFY_WITH_TIMEOUT(!frames.isEmpty() || !errors.isEmpty(), 10000);
        QVERIFY2(errors.isEmpty(), qPrintable(capture.errorString()));
        const auto frame = qvariant_cast<QVideoFrame>(frames.back()[0]);
        capture.stop();
        QVERIFY(frame.isValid());
        const auto image = frame.toImage(); QVERIFY(!image.isNull());
        qInfo() << "Desktop capture:" << image.size();
        ucd::screen::Encoder encoder;
        ucd::screen::CodecOptions options{1920, 30, 4000000, {}, qEnvironmentVariable("UCD_TEST_OPENH264")};
        std::optional<ucd::screen::EncodedFrame> encoded;
        for (int i = 0; i < 16 && !encoded; ++i) encoded = encoder.encode(image, options, 1, i, i * 33, true);
        QVERIFY2(encoded, qPrintable(encoder.error()));
        ucd::screen::Decoder decoder;
        QVERIFY2(!decoder.decode(*encoded).isNull(), qPrintable(decoder.error()));
    }
};
QTEST_MAIN(ScreenCaptureTest)
#include "ScreenCaptureTest.moc"
