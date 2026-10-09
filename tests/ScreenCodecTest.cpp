#include <QtTest>
#include <cmath>
#include "core/screen/Codec.hpp"

using namespace ucd::screen;
class ScreenCodecTest : public QObject {
    Q_OBJECT
private slots:
    void bitrateChangesAreBatched() {
        QVERIFY(!bitrateWorthChanging(4000000, 4000000, 60000));
        // Cuts: soon, but not twice within a second.
        QVERIFY(bitrateWorthChanging(4000000, 3200000, 1000)); QVERIFY(!bitrateWorthChanging(4000000, 3200000, 500));
        // Raises: in larger steps, every few seconds.
        QVERIFY(!bitrateWorthChanging(4000000, 4400000, 6000)); QVERIFY(!bitrateWorthChanging(4000000, 6000000, 3000));
        QVERIFY(bitrateWorthChanging(4000000, 6000000, 5000));
        // Small drift catches up eventually.
        QVERIFY(bitrateWorthChanging(4000000, 4400000, 15000)); QVERIFY(!bitrateWorthChanging(4000000, 4100000, 60000));
    }
    // Captured frames are encoded in their own layout, packed or planar.
    void encodesVideoFramesInTheirLayout() {
        QImage source(320, 180, QImage::Format_RGB32);
        for (int y = 0; y < source.height(); ++y) for (int x = 0; x < source.width(); ++x)
            source.setPixelColor(x, y, QColor(x / 2 + 40, y + 30, (x + y) / 3 + 50));
        // NV12 by hand (BT.601, video range), as cameras deliver it.
        QVideoFrame nv12(QVideoFrameFormat(source.size(), QVideoFrameFormat::Format_NV12));
        QVERIFY(nv12.map(QVideoFrame::WriteOnly));
        for (int y = 0; y < source.height(); ++y) for (int x = 0; x < source.width(); ++x) {
            const QColor c = source.pixelColor(x, y);
            nv12.bits(0)[y * nv12.bytesPerLine(0) + x] = uchar(16 + (66 * c.red() + 129 * c.green() + 25 * c.blue() + 128) / 256);
            if (x % 2 == 0 && y % 2 == 0) {
                uchar* uv = nv12.bits(1) + (y / 2) * nv12.bytesPerLine(1) + x;
                uv[0] = uchar(128 + (-38 * c.red() - 74 * c.green() + 112 * c.blue() + 128) / 256);
                uv[1] = uchar(128 + (112 * c.red() - 94 * c.green() - 18 * c.blue() + 128) / 256);
            }
        }
        nv12.unmap();
        // Same size (a plain copy when the layouts match) and scaled down.
        for (const int width : {320, 160}) for (const auto& frame : {QVideoFrame(source), nv12}) {
            Encoder encoder; Decoder decoder;
            CodecOptions options{width, 30, 2000000, {}, qEnvironmentVariable("UCD_TEST_OPENH264")};
            const QImage expected = source.scaledToWidth(width, Qt::SmoothTransformation);
            std::optional<EncodedFrame> encoded;
            for (int i = 0; i < 8 && !encoded; ++i) encoded = encoder.encode(frame, options, 1, i, i * 33, true);
            if (!encoded && encoder.name().isEmpty()) QSKIP(qPrintable("No usable H.264 encoder: " + encoder.error()));
            QVERIFY2(encoded, qPrintable(encoder.error()));
            const QImage image = decoder.decode(*encoded);
            QVERIFY2(!image.isNull(), qPrintable(decoder.error())); QCOMPARE(image.size(), expected.size());
            double error = 0;
            for (int y = 0; y < expected.height(); ++y) for (int x = 0; x < expected.width(); ++x) {
                const auto a = expected.pixelColor(x, y), b = image.pixelColor(x, y);
                error += std::pow(a.red() - b.red(), 2) + std::pow(a.green() - b.green(), 2) + std::pow(a.blue() - b.blue(), 2);
            }
            const double psnr = 10 * std::log10(255. * 255 / std::max(1e-9, error / (expected.width() * expected.height() * 3)));
            QVERIFY2(psnr > 28, qPrintable(QString("layout %1, width %2: PSNR %3 dB").arg(int(frame.pixelFormat())).arg(width).arg(psnr)));
        }
    }
    void roundTrip_data() {
        QTest::addColumn<QString>("name");
        for (const auto& name : Encoder::candidates()) QTest::newRow(qPrintable(name)) << name;
    }
    void roundTrip() {
        QFETCH(QString, name);
        CodecOptions options{640, 30, 4000000, name, qEnvironmentVariable("UCD_TEST_OPENH264")};
        Encoder encoder; Decoder decoder, frames;
        QImage source(640, 360, QImage::Format_RGBA8888);
        for (int y = 0; y < source.height(); ++y) for (int x = 0; x < source.width(); ++x)
            source.setPixelColor(x, y, QColor(x / 8 + 50, y / 4 + 30, (x + y) / 6 + 40));
        int decoded = 0;
        for (int i = 0; i < 12; ++i) {
            const auto encoded = encoder.encode(source, options, 3, i, i * 33, i == 0 || i == 8);
            if (!encoded) {
                if (i == 0 && !encoder.error().isEmpty()) {
                    if (name == "cisco" && !options.ciscoLibrary.isEmpty()) QFAIL(qPrintable(encoder.error()));
                    QSKIP(qPrintable(encoder.error()));
                }
                continue;
            }
            if (decoded == 0) QVERIFY(encoded->keyframe);
            const auto image = decoder.decode(*encoded);
            QVERIFY2(!image.isNull(), qPrintable(decoder.error())); QCOMPARE(image.size(), source.size());
            const auto psnr = [&](const QImage& out) {
                double error = 0;
                for (int y = 0; y < source.height(); ++y) for (int x = 0; x < source.width(); ++x) {
                    const auto a = source.pixelColor(x, y), b = out.pixelColor(x, y);
                    error += std::pow(a.red() - b.red(), 2) + std::pow(a.green() - b.green(), 2) + std::pow(a.blue() - b.blue(), 2);
                }
                const double mse = error / (source.width() * source.height() * 3);
                return mse ? 10 * std::log10(255. * 255 / mse) : 100;
            };
            QVERIFY2(psnr(image) > 28, qPrintable(QString("PSNR %1 dB").arg(psnr(image))));
            // The viewer's path: YUV frames, labelled so the sink converts them right.
            bool produced = false;
            const auto frame = frames.decodeFrame(*encoded, true, &produced);
            QVERIFY(produced); QVERIFY(frame.isValid()); QCOMPARE(frame.size(), source.size());
            QVERIFY(frame.pixelFormat() != QVideoFrameFormat::Format_RGBA8888);
            const auto shown = frame.toImage().convertToFormat(QImage::Format_RGBA8888);
            QVERIFY2(psnr(shown) > 28, qPrintable(QString("frame PSNR %1 dB").arg(psnr(shown)))); ++decoded;
            if (i == 5) options.bitrate = 2000000;
        }
        QVERIFY(decoded >= 8);
        options.width = 320;
        std::optional<EncodedFrame> smaller;
        for (int i = 20; i < 32 && !smaller; ++i) smaller = encoder.encode(source, options, 3, i, i * 33, true);
        QVERIFY2(smaller, qPrintable(encoder.error()));
        const auto scaled = decoder.decode(*smaller); QCOMPARE(scaled.size(), QSize(320, 180));
    }
};
QTEST_GUILESS_MAIN(ScreenCodecTest)
#include "ScreenCodecTest.moc"
