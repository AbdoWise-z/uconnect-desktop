// KeepAwake asks the system to keep the screen on, and lets go. On Linux it
// talks to stand-ins for the desktops' inhibit services on the session bus
// (run it under dbus-run-session to keep it off a real desktop's bus).
#include <QtTest>

#include "app/KeepAwake.hpp"

#ifdef UCD_HAS_DBUS
#include <QDBusConnection>
#include <QDBusConnectionInterface>

// What KDE and most desktops offer.
class FakeScreenSaver : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.ScreenSaver")
public:
    QStringList calls;
public slots:
    Q_SCRIPTABLE uint Inhibit(const QString& app, const QString& reason) {
        calls << QStringLiteral("Inhibit %1: %2").arg(app, reason);
        return 42;
    }
    Q_SCRIPTABLE void UnInhibit(uint cookie) { calls << QStringLiteral("UnInhibit %1").arg(cookie); }
};

// What GNOME offers.
class FakeGnomeSession : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.gnome.SessionManager")
public:
    QStringList calls;
public slots:
    Q_SCRIPTABLE uint Inhibit(const QString& app, uint toplevel, const QString&, uint flags) {
        calls << QStringLiteral("Inhibit %1 %2 %3").arg(app).arg(toplevel).arg(flags);
        return 7;
    }
    Q_SCRIPTABLE void Uninhibit(uint cookie) { calls << QStringLiteral("Uninhibit %1").arg(cookie); }
};
#endif

class KeepAwakeTest : public QObject {
    Q_OBJECT
private slots:
#ifdef UCD_HAS_DBUS
    void init() {
        if (!QDBusConnection::sessionBus().isConnected()) QSKIP("No D-Bus session bus.");
    }

    void asksTheFreedesktopScreenSaver() {
        auto bus = QDBusConnection::sessionBus();
        FakeScreenSaver saver;
        if (!bus.registerService("org.freedesktop.ScreenSaver"))
            QSKIP("A real screen saver owns the name; run under dbus-run-session.");
        QVERIFY(bus.registerObject("/org/freedesktop/ScreenSaver", &saver, QDBusConnection::ExportScriptableSlots));
        {
            ucd::KeepAwake awake;
            awake.set(true);
            QVERIFY(awake.held());
            QCOMPARE(saver.calls, QStringList{"Inhibit uConnect: Showing an extended display"});
            awake.set(true);  // already held: not asked twice
            QCOMPARE(saver.calls.size(), 1);
            awake.set(false);
            QVERIFY(!awake.held());
            QCOMPARE(saver.calls.last(), "UnInhibit 42");
            awake.set(true);
        }  // let go when it goes away
        QCOMPARE(saver.calls.last(), "UnInhibit 42");
        bus.unregisterObject("/org/freedesktop/ScreenSaver");
        bus.unregisterService("org.freedesktop.ScreenSaver");
    }

    void fallsBackToGnome() {
        auto bus = QDBusConnection::sessionBus();
        FakeGnomeSession gnome;
        if (!bus.registerService("org.gnome.SessionManager"))
            QSKIP("A real GNOME session owns the name; run under dbus-run-session.");
        QVERIFY(bus.registerObject("/org/gnome/SessionManager", &gnome, QDBusConnection::ExportScriptableSlots));
        ucd::KeepAwake awake;
        awake.set(true);
        QVERIFY(awake.held());
        QCOMPARE(gnome.calls, QStringList{"Inhibit uConnect 0 8"});  // 8: idle
        awake.set(false);
        QCOMPARE(gnome.calls.last(), "Uninhibit 7");
        bus.unregisterObject("/org/gnome/SessionManager");
        bus.unregisterService("org.gnome.SessionManager");
    }

    void nothingToAsk() {
        const auto names = QDBusConnection::sessionBus().interface();
        if (names->isServiceRegistered("org.freedesktop.ScreenSaver") || names->isServiceRegistered("org.gnome.SessionManager"))
            QSKIP("A desktop's inhibit service is running.");
        ucd::KeepAwake awake;
        awake.set(true);
        QVERIFY(!awake.held());
        awake.set(false);
    }
#else
    void theSystemTakesTheRequest() {
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
        ucd::KeepAwake awake;
        awake.set(true);
        QVERIFY(awake.held());
        awake.set(false);
        QVERIFY(!awake.held());
#else
        QSKIP("Built without D-Bus: nothing to ask.");
#endif
    }
#endif
};

QTEST_GUILESS_MAIN(KeepAwakeTest)
#include "KeepAwakeTest.moc"
