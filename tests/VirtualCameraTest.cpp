// The virtual camera's frame preparation: fitting a watched camera's picture
// into the size apps ask for, and the RGB -> YUV 4:2:0 conversion both
// outputs (NV12 on Windows, I420 on Linux) use.
#include <QtTest>

#include <vector>

#include "app/vcam/VcamOutput.hpp"

using namespace ucd::vcam;

class VirtualCameraTest : public QObject {
    Q_OBJECT
private slots:
    void fitsWithBlackBars() {
        QImage wide(100, 50, QImage::Format_RGB32);
        wide.fill(Qt::red);
        const QImage out = fit(wide, 0, QSize(200, 200));
        QCOMPARE(out.size(), QSize(200, 200));
        QCOMPARE(out.pixelColor(100, 100), QColor(Qt::red));  // scaled to 200x100, centred
        QCOMPARE(out.pixelColor(100, 20), QColor(Qt::black));  // bar above
        QCOMPARE(out.pixelColor(100, 180), QColor(Qt::black));  // bar below
    }

    void rotatesBeforeFitting() {
        QImage wide(100, 50, QImage::Format_RGB32);
        wide.fill(Qt::red);
        const QImage out = fit(wide, 90, QSize(200, 200));  // now 50x100: bars at the sides
        QCOMPARE(out.pixelColor(100, 100), QColor(Qt::red));
        QCOMPARE(out.pixelColor(20, 100), QColor(Qt::black));
        QCOMPARE(out.pixelColor(100, 20), QColor(Qt::red));
    }

    void convertsToLimitedRangeYuv_data() {
        QTest::addColumn<QColor>("color");
        QTest::addColumn<int>("y");
        QTest::addColumn<int>("u");
        QTest::addColumn<int>("v");
        QTest::newRow("black") << QColor(Qt::black) << 16 << 128 << 128;
        QTest::newRow("white") << QColor(Qt::white) << 235 << 128 << 128;
        QTest::newRow("red") << QColor(Qt::red) << 82 << 90 << 240;
        QTest::newRow("blue") << QColor(Qt::blue) << 41 << 240 << 110;
    }
    void convertsToLimitedRangeYuv() {
        QFETCH(QColor, color);
        QFETCH(int, y);
        QFETCH(int, u);
        QFETCH(int, v);
        QImage image(4, 4, QImage::Format_RGB32);
        image.fill(color);

        // NV12: one interleaved chroma plane.
        std::vector<uint8_t> nv12(4 * 4 * 3 / 2);
        uint8_t*             uv = nv12.data() + 16;
        toYuv420(image, nv12.data(), 4, uv, uv + 1, 4, 2);
        QVERIFY(qAbs(nv12[0] - y) <= 1 && qAbs(nv12[15] - y) <= 1);
        QVERIFY(qAbs(uv[0] - u) <= 1 && qAbs(uv[1] - v) <= 1 && qAbs(uv[6] - u) <= 1 && qAbs(uv[7] - v) <= 1);

        // I420: separate planes.
        std::vector<uint8_t> i420(4 * 4 * 3 / 2);
        uint8_t*             up = i420.data() + 16;
        uint8_t*             vp = up + 4;
        toYuv420(image, i420.data(), 4, up, vp, 2, 1);
        QVERIFY(qAbs(i420[5] - y) <= 1);
        QVERIFY(qAbs(up[3] - u) <= 1 && qAbs(vp[3] - v) <= 1);
    }

    // Where the system has a virtual camera, the support check says so and
    // never claims an install that is not there.
    void reportsSupport() {
        const Support s = support();
        if (!s.supported) QVERIFY(!s.installed);
        if (!s.installed) QVERIFY(!s.hint.isEmpty());
    }
};

QTEST_GUILESS_MAIN(VirtualCameraTest)
#include "VirtualCameraTest.moc"
