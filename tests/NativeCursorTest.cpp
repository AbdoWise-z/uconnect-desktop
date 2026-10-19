// The host's pointer image, as sent to viewers: it must fit what a viewer
// accepts (at most 128x128, the hotspot inside), or viewers draw no pointer.
// On a Retina Mac the system arrow's largest form is 280x400 pixels.
#include <QtTest>

#include "core/screen/Cursor.hpp"

class NativeCursorTest : public QObject {
    Q_OBJECT
private slots:
    void imageFitsWhatViewersTake() {
        const auto cursor = ucd::screen::nativeCursor(0);
#ifdef Q_OS_MACOS
        QVERIFY2(!cursor.image.isNull(), "macOS always has a system cursor");
#endif
        if (cursor.image.isNull()) QSKIP("No cursor image here (no desktop session).");
        QVERIFY2(cursor.image.width() <= 128 && cursor.image.height() <= 128,
                 qPrintable(QStringLiteral("%1x%2").arg(cursor.image.width()).arg(cursor.image.height())));
        QVERIFY(cursor.hotspot.x() >= 0 && cursor.hotspot.x() < cursor.image.width());
        QVERIFY(cursor.hotspot.y() >= 0 && cursor.hotspot.y() < cursor.image.height());
        // The same shape again: nothing new to send.
        QVERIFY(ucd::screen::nativeCursor(cursor.shape).image.isNull());
    }
};

QTEST_MAIN(NativeCursorTest)
#include "NativeCursorTest.moc"
