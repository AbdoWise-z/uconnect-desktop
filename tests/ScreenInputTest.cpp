#include <QtTest>
#include <QSignalSpy>
#include <QInputMethodEvent>
#include <QJsonArray>
#include "app/InputCapture.hpp"
#include "core/screen/ScreenService.hpp"

using namespace ucd;
using namespace ucd::screen;
struct RecordingInput : InputBackend {
    QSet<quint16> held;
    int buttons = 0, calls = 0;
    QPoint point;
    QString typed;
    bool fail = false;
    QString available() const override { return {}; }
    bool key(quint16 usage, bool down) override { ++calls; if (fail) return false; if (down) held.insert(usage); else held.remove(usage); return true; }
    bool mouse(const QPoint& at, int, int next) override { ++calls; if (fail) return false; point = at; buttons = next; return true; }
    bool wheel(int, int) override { ++calls; return !fail; }
    bool text(const QString& value) override { ++calls; typed += value; return !fail; }
};
class ScreenInputTest : public QObject {
    Q_OBJECT
    static uconnect::DevId peer(int n = 1) { uconnect::DevId p{}; p[0] = uint8_t(n); return p; }
private slots:
    void physicalMappings() {
        QSet<int> usages, windows, linux;
        for (const auto& key : keyMappings()) {
            QVERIFY(!usages.contains(key.usage)); usages.insert(key.usage);
            QVERIFY(!windows.contains(key.windowsScan)); windows.insert(key.windowsScan);
            QVERIFY(!linux.contains(key.linuxCode)); linux.insert(key.linuxCode);
            // Deliberately use a conflicting logical key: native position wins.
            QKeyEvent win(QEvent::KeyPress, Qt::Key_Z, Qt::NoModifier, key.windowsScan, 0, 0);
            QCOMPARE(hidUsage(win, KeyboardPlatform::Windows), key.usage);
            QKeyEvent x11(QEvent::KeyPress, Qt::Key_Z, Qt::NoModifier, key.linuxCode + 8, 0, 0);
            QCOMPARE(hidUsage(x11, KeyboardPlatform::X11), key.usage);
            if (key.macCode >= 0) {
                QKeyEvent mac(QEvent::KeyPress, Qt::Key_Z, Qt::NoModifier, 0, key.macCode, 0);
                QCOMPARE(hidUsage(mac, KeyboardPlatform::Mac), key.usage);
            }
        }
        QKeyEvent extended(QEvent::KeyPress, Qt::Key_Control, Qt::ControlModifier, 0xe01d, 0, 0);
        QCOMPARE(hidUsage(extended, KeyboardPlatform::Windows), quint16(0xe4));
    }
    void validationAndRelease() {
        auto backend = std::make_unique<RecordingInput>(); auto* record = backend.get(); InputState input(std::move(backend));
        input.setBounds({-1920, 0, 1920, 1080});
        QVERIFY(input.apply({{"t", "in.key"}, {"usage", 0xe0}, {"down", true}}));
        QVERIFY(input.apply({{"t", "in.mouse"}, {"x", 1.}, {"y", 1.}, {"buttons", 1}}));
        QCOMPARE(record->point, QPoint(-1, 1079)); QCOMPARE(input.pressedCount(), 2);
        const int calls = record->calls;
        for (const auto& msg : QVector<QJsonObject>{
            {{"t", "in.key"}, {"usage", 4.5}, {"down", true}},
            {{"t", "in.key"}, {"usage", 255}, {"down", true}},
            {{"t", "in.key"}, {"usage", 4}, {"down", 1}},
            {{"t", "in.mouse"}, {"x", -0.1}, {"y", 0.}, {"buttons", 1}},
            {{"t", "in.mouse"}, {"x", 0.}, {"y", 0.}, {"buttons", 1.5}},
            {{"t", "in.wheel"}, {"dx", -2147483648.}, {"dy", 0}},
            {{"t", "in.text"}, {"text", QString(257, 'x')}}}) QVERIFY(!input.apply(msg));
        QCOMPARE(record->calls, calls);
        QVERIFY(input.apply({{"t", "in.text"}, {"text", QString::fromUtf8("é日本😀")}}));
        QCOMPARE(record->typed, QString::fromUtf8("é日本😀"));
        record->fail = true; input.release(); QCOMPARE(input.pressedCount(), 2);
        record->fail = false; input.release(); QCOMPARE(input.pressedCount(), 0); QVERIFY(record->held.isEmpty()); QCOMPARE(record->buttons, 0);
    }
    void captureTextShortcutsAndFocus() {
        QObject window; InputCapture capture; capture.attach(&window); QSignalSpy events(&capture, &InputCapture::input);
        capture.setCaptured(true);
        QKeyEvent press(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, "é"), up(QEvent::KeyRelease, Qt::Key_A, Qt::NoModifier);
        QCoreApplication::sendEvent(&window, &press); QCoreApplication::sendEvent(&window, &up);
        QCOMPARE(events.size(), 1); QCOMPARE(events.back()[0].toJsonObject().value("t"), "in.text");
        QKeyEvent shortcut(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier, keyboardPlatform() == KeyboardPlatform::X11 ? 54 : 46, 8, 0);
        QCoreApplication::sendEvent(&window, &shortcut); QCOMPARE(events.back()[0].toJsonObject().value("usage"), 6);
        QInputMethodEvent ime; ime.setCommitString(QString::fromUtf8("日本😀")); QCoreApplication::sendEvent(&window, &ime);
        QCOMPARE(events.back()[0].toJsonObject().value("text"), QString::fromUtf8("日本😀"));
        QEvent focus(QEvent::WindowDeactivate); QCoreApplication::sendEvent(&window, &focus);
        QVERIFY(!capture.captured()); QCOMPARE(events.back()[0].toJsonObject().value("t"), "in.release");
        capture.setCaptured(true);
        QKeyEvent release(QEvent::KeyPress, Qt::Key_Control, Qt::ControlModifier, keyboardPlatform() == KeyboardPlatform::X11 ? 105 : 0x11d, 62, 0);
        QCoreApplication::sendEvent(&window, &release); QVERIFY(!capture.captured());
    }
    void controlConsentAndEveryStop() {
        for (int stop = 0; stop < 6; ++stop) {
            auto backend = std::make_unique<RecordingInput>(); auto* record = backend.get();
            qint64 time = 0; PeerAccess access{"key", "Alice", "fingerprint", {perm::ScreenView, perm::ScreenDirect, perm::ControlInput}, true};
            QVector<QJsonObject> sent;
            ScreenService service([&](const auto&, const auto& msg) { sent.push_back(msg); return true; },
                [&](const auto&) { return access; }, [](const auto&) { return true; }, [](const auto&) {},
                [](const auto&, const auto&) { return true; }, nullptr, [&] { return time; }, std::move(backend));
            ScreenSettings settings; settings.monitors = {QVariantMap{{"width", 1920}, {"height", 1080}}}; service.configure(settings);
            service.setOffered(true); service.setControlOffered(true);
            service.handle(peer(), {{"t", "scr.request"}, {"request", 1}, {"codecs", QJsonArray{"h264"}}});
            const int token = sent.back().value("session").toInt(); QVERIFY(token > 0); QVERIFY(!sent.back().value("control").toBool());
            auto key = QJsonObject{{"t", "in.key"}, {"session", token}, {"usage", 4}, {"down", true}};
            service.handle(peer(), key); QCOMPARE(record->calls, 0);
            QSignalSpy prompts(&service, &ScreenService::consentRequested);
            service.handle(peer(), {{"t", "scr.control"}, {"session", token}, {"request", 2}, {"on", true}});
            QCOMPARE(prompts.size(), 1);
            const QString id = QString::fromStdString(uconnect::to_hex(peer()));
            service.consent(id, 3, true, false); service.handle(peer(), key); QCOMPARE(record->calls, 0);
            service.consent(id, 2, true, false); QVERIFY(sent.back().value("on").toBool());
            key["session"] = token + 1; service.handle(peer(), key); QCOMPARE(record->calls, 0);
            key["session"] = token; service.handle(peer(), key); QCOMPARE(record->held.size(), 1);
            service.handle(peer(), {{"t", "in.mouse"}, {"session", token}, {"x", .5}, {"y", .5}, {"buttons", 1}});
            QCOMPARE(record->buttons, 1);
            if (stop == 0) service.peerGone(peer());
            if (stop == 1) service.stopHosting();
            if (stop == 2) { access.permissions.remove(perm::ControlInput); service.policyChanged(); }
            if (stop == 3) service.stopControl();
            if (stop == 4) { time = 5001; service.tick(); }
            if (stop == 5) { access.trusted = false; service.policyChanged(); }
            QVERIFY(record->held.isEmpty()); QCOMPARE(record->buttons, 0);
            const int releasedCalls = record->calls; service.handle(peer(), key); QCOMPARE(record->calls, releasedCalls);
        }
    }
    void oneControllerAmongViewers() {
        auto backend = std::make_unique<RecordingInput>(); auto* record = backend.get();
        PeerAccess access{"key", "Viewer", "fp", {perm::ScreenView, perm::ScreenDirect, perm::ControlInput, perm::ControlDirect}, true};
        std::map<uconnect::DevId, QVector<QJsonObject>> sent;
        ScreenService service([&](const auto& p, const auto& msg) { sent[p].push_back(msg); return true; },
            [&](const auto&) { return access; }, [](const auto&) { return true; }, [](const auto&) {},
            [](const auto&, const auto&) { return true; }, nullptr, {}, std::move(backend));
        ScreenSettings settings; settings.maxViewers = 2; settings.monitors = {QVariantMap{{"width", 1920}, {"height", 1080}}};
        service.configure(settings); service.setOffered(true); service.setControlOffered(true);
        service.handle(peer(1), {{"t", "scr.request"}, {"request", 1}, {"codecs", QJsonArray{"h264"}}, {"wantControl", true}});
        const int first = sent[peer(1)].back().value("session").toInt(); QVERIFY(sent[peer(1)].back().value("control").toBool());
        service.handle(peer(1), {{"t", "in.key"}, {"session", first}, {"usage", 4}, {"down", true}}); QCOMPARE(record->held.size(), 1);
        service.handle(peer(2), {{"t", "scr.request"}, {"request", 2}, {"codecs", QJsonArray{"h264"}}});
        const int second = sent[peer(2)].back().value("session").toInt(); QVERIFY(second);
        service.handle(peer(2), {{"t", "scr.control"}, {"request", 3}, {"session", second}, {"on", true}});
        QVERIFY(!sent[peer(2)].back().value("on").toBool()); QCOMPARE(record->held.size(), 1);
        const int before = record->calls;
        service.handle(peer(2), {{"t", "in.key"}, {"session", second}, {"usage", 5}, {"down", true}}); QCOMPARE(record->calls, before);
        service.peerGone(peer(2)); QCOMPARE(record->held.size(), 1);
        service.stopControl(); QVERIFY(record->held.isEmpty());
    }
    void altGrAndDeadKeys() {
        QObject window; InputCapture capture; capture.attach(&window); capture.setCaptured(true); QSignalSpy events(&capture, &InputCapture::input);
        QKeyEvent altGr(QEvent::KeyPress, Qt::Key_AltGr, Qt::GroupSwitchModifier);
        QCoreApplication::sendEvent(&window, &altGr);
        QKeyEvent character(QEvent::KeyPress, Qt::Key_Q, Qt::ControlModifier | Qt::AltModifier, "@");
        QCoreApplication::sendEvent(&window, &character);
        QCOMPARE(events.back()[0].toJsonObject().value("t"), "in.text"); QCOMPARE(events.back()[0].toJsonObject().value("text"), "@");
        QKeyEvent dead(QEvent::KeyPress, Qt::Key_Dead_Acute, Qt::NoModifier);
        const int count = events.size(); QCoreApplication::sendEvent(&window, &dead); QCOMPARE(events.size(), count);
    }
    void viewerRequiresRequestedControl() {
        for (bool camera : {false, true}) {
            QVector<QJsonObject> sent;
            ScreenService service([&](const auto&, const auto& msg) { sent.push_back(msg); return true; },
                [](const auto&) { return PeerAccess{"key", "Host", "fp", {}, true}; },
                [](const auto&) { return true; }, [](const auto&) {}, [](const auto&, const auto&) { return true; }, nullptr, {}, {}, camera);
            QSignalSpy state(&service, &ScreenService::viewerChanged);
            service.request(peer());
            service.handle(peer(), {{"t", camera ? "cam.started" : "scr.started"}, {"request", 1}, {"session", 14}, {"codec", "h264"}, {"control", true}});
            QVERIFY(!state.back()[1].toMap().value("control").toBool());
            const int before = sent.size();
            service.sendInput(peer(), {{"t", "in.key"}, {"usage", 4}, {"down", true}}); QCOMPARE(sent.size(), before);
            service.requestControl(peer(), true);
            if (camera) { QCOMPARE(sent.size(), before); continue; }
            const auto id = sent.back().value("request");
            service.handle(peer(), {{"t", "scr.controlled"}, {"request", 0}, {"session", 14}, {"on", true}});
            QVERIFY(!state.back()[1].toMap().value("control").toBool());
            service.handle(peer(), {{"t", "scr.controlled"}, {"request", id}, {"session", 14}, {"on", true}});
            QVERIFY(state.back()[1].toMap().value("control").toBool());
        }
    }
    void cursorWire() {
        const CursorPosition source{65535, 0xffffffff, {-1920, 100}, true};
        const auto wire = cursorPacket(source); QCOMPARE(wire.size(), 16);
        const auto parsed = parseCursor(wire); QVERIFY(parsed); QCOMPARE(parsed->point, source.point); QCOMPARE(parsed->sequence, source.sequence);
        QVERIFY(cursorNewer(0, 0xffffffff)); QVERIFY(!cursorNewer(0xffffffff, 0)); QVERIFY(!cursorNewer(5, 5));
        QVERIFY(!parseCursor(wire.left(15))); auto invalid = wire; invalid[15] = 2; QVERIFY(!parseCursor(invalid));
        invalid = wire; invalid[0] = 1; QVERIFY(!parseCursor(invalid));
    }
};
QTEST_GUILESS_MAIN(ScreenInputTest)
#include "ScreenInputTest.moc"
