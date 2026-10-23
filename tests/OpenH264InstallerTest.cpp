// The OpenH264 installer: which of Cisco's binaries each system gets, and,
// with UCD_TEST_NETWORK=1, a real download from Cisco (checked, unpacked,
// saved with its license).
#include <QtTest>

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QLibrary>
#include <QSignalSpy>
#include <QStandardPaths>

#include "core/OpenH264Installer.hpp"

using ucd::OpenH264Installer;

class OpenH264InstallerTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void picksCiscosBuildForEachSystem() {
        QCOMPARE(OpenH264Installer::release("windows", "x64").first, QStringLiteral("openh264-2.6.0-win64.dll"));
        QCOMPARE(OpenH264Installer::release("macos", "arm64").first, QStringLiteral("libopenh264-2.6.0-mac-arm64.dylib"));
        QCOMPARE(OpenH264Installer::release("linux", "x64").first, QStringLiteral("libopenh264-2.6.0-linux64.8.so"));
        QCOMPARE(OpenH264Installer::release("windows", "x64").second.size(), 32);  // a SHA-256
        QVERIFY(OpenH264Installer::release("linux", "riscv64").first.isEmpty());
    }

    void installsIntoItsDataFolder() {
        OpenH264Installer installer;
        if (!installer.supported()) QSKIP("Cisco has no build for this system");
        QVERIFY(installer.installPath().endsWith("/codecs/" + installer.fileName()));
        QVERIFY(installer.license().contains("Cisco"));
        QVERIFY(!installer.exists(QString()));
    }

    void downloadsFromCisco() {
        if (qEnvironmentVariable("UCD_TEST_NETWORK") != "1") QSKIP("set UCD_TEST_NETWORK=1 to download from Cisco");
        OpenH264Installer installer;
        if (!installer.supported()) QSKIP("Cisco has no build for this system");
        QFile::remove(installer.installPath());
        QSignalSpy installed(&installer, &OpenH264Installer::installed);
        installer.install();
        QCOMPARE(installer.state(), QStringLiteral("downloading"));
        QTRY_VERIFY_WITH_TIMEOUT(installer.state() != "downloading", 60000);
        QVERIFY2(installer.state() == "done", qPrintable(installer.error()));
        QCOMPARE(installed.count(), 1);
        const QString path = installed.first().first().toString();
        QCOMPARE(path, installer.installPath());
        QVERIFY(QFileInfo(path).size() > 500 * 1024);
        QVERIFY(QFileInfo::exists(QFileInfo(path).absoluteDir().filePath("OpenH264-BINARY-LICENSE.txt")));

        // It is Cisco's 2.6 library: it loads and reports that version.
        QLibrary library(path);
        QVERIFY2(library.load(), qPrintable(library.errorString()));
        struct Version { unsigned major, minor, revision, reserved; };
        const auto version = reinterpret_cast<void (*)(Version*)>(library.resolve("WelsGetCodecVersionEx"));
        QVERIFY(version);
        Version v{};
        version(&v);
        QCOMPARE(v.major, 2u);
        QCOMPARE(v.minor, 6u);
        library.unload();
    }
};

QTEST_GUILESS_MAIN(OpenH264InstallerTest)
#include "OpenH264InstallerTest.moc"
