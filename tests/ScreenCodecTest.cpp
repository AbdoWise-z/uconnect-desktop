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
    void roundTrip_data() {
        QTest::addColumn<QString>("name");
        for (const auto& name : Encoder::candidates()) QTest::newRow(qPrintable(name)) << name;
    }
    void roundTrip() {
        QFETCH(QString, name);
        CodecOptions options{640, 30, 4000000, name, qEnvironmentVariable("UCD_TEST_OPENH264")};
        Encoder encoder; Decoder decoder;
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
            double error = 0;
            for (int y = 0; y < source.height(); ++y) for (int x = 0; x < source.width(); ++x) {
                const auto a = source.pixelColor(x, y), b = image.pixelColor(x, y);
                error += std::pow(a.red() - b.red(), 2) + std::pow(a.green() - b.green(), 2) + std::pow(a.blue() - b.blue(), 2);
            }
            const double mse = error / (source.width() * source.height() * 3);
            const double psnr = mse ? 10 * std::log10(255. * 255 / mse) : 100;
            QVERIFY2(psnr > 28, qPrintable(QString("PSNR %1 dB").arg(psnr))); ++decoded;
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
