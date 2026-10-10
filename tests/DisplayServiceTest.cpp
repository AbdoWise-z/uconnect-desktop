#include <QtTest>
#include <QSignalSpy>
#include <QJsonArray>
#include <atomic>
#include <deque>
#include "core/display/DisplayService.hpp"

using namespace ucd;
using namespace ucd::display;
using namespace ucd::screen;

namespace {

// A virtual monitor that only counts: the tests check that every way a
// session ends leaves none behind.
std::atomic_int alive{0};
std::atomic_int creates{0};
std::atomic_bool failCreate{false};
std::atomic_int createDelayMs{0};

class FakeMonitor final : public VirtualDisplay {
public:
    ~FakeMonitor() override { destroy(); }
    bool create(const QByteArray& identity, const QString&, const Mode& mode, QString* error) override {
        if (createDelayMs) QThread::msleep(createDelayMs);
        ++creates;
        if (failCreate) { if (error) *error = "no driver here"; return false; }
        if (identity.size() != 16) { if (error) *error = "bad identity"; return false; }
        mode_ = mode; made_ = true; ++alive;
        return true;
    }
    bool setMode(const Mode& mode, QString*) override { mode_ = mode; return true; }
    void destroy() override { if (made_) { made_ = false; --alive; } }
    void keepAlive() override {}
    QString captureId() const override { return made_ ? QStringLiteral("fake") : QString(); }
    QRect geometry() const override { return QRect(1920, 0, mode_.width, mode_.height); }
    QSize pixels() const override { return {mode_.width, mode_.height}; }
private:
    Mode mode_;
    bool made_ = false;
};

uconnect::DevId peer(int n) { uconnect::DevId p{}; p[0] = uint8_t(n); return p; }
QString hexOf(int n) { return QString::fromStdString(uconnect::to_hex(peer(n))); }

// Two devices in one process: the host (it gains a monitor) and the display
// device (it shows it). Each sees the other as peer(1) / peer(2).
struct Pair {
    qint64 time = 1000;
    PeerAccess hostSeesDisplay{"dddd", "Tablet", "fp-d", {}, true, true};
    PeerAccess displaySeesHost{"aaaa", "Laptop", "fp-h", {perm::DisplayShow}, true, true};
    std::deque<std::pair<bool, QJsonObject>> wire;  // (to the display device?, message)
    QString hostPath = "direct";

    ScreenService hostMedia{[this](const auto&, const auto& m) { wire.push_back({true, m}); return true; },
        [this](const auto&) { return hostSeesDisplay; }, [](const auto&) { return true; }, [](const auto&) {},
        [](const auto&, const auto&) { return true; }, nullptr, [this] { return time; }, {}, Kind::Display};
    ScreenService displayMedia{[this](const auto&, const auto& m) { wire.push_back({false, m}); return true; },
        [this](const auto&) { return displaySeesHost; }, [](const auto&) { return true; }, [](const auto&) {},
        [](const auto&, const auto&) { return true; }, nullptr, [this] { return time; }, {}, Kind::Display};
    DisplayService host{[this](const auto&, const auto& m) { wire.push_back({true, m}); return true; },
        [this](const auto&) { return hostSeesDisplay; }, &hostMedia, [] { return std::make_unique<FakeMonitor>(); },
        [] { return Status{Availability::Ready, {}}; }, nullptr, [this] { return time; }};
    DisplayService display{[this](const auto&, const auto& m) { wire.push_back({false, m}); return true; },
        [this](const auto&) { return displaySeesHost; }, &displayMedia, [] { return std::make_unique<FakeMonitor>(); },
        [] { return Status{Availability::Ready, {}}; }, nullptr, [this] { return time; }};

    Pair() {
        host.setNativeCapture(false);
        display.setOffered(true);
        display.setLocalScreens({QVariantMap{{"name", "Built-in"}, {"width", 2732}, {"height", 2048}, {"fps", 120}, {"dpi", 264}}});
    }
    // Delivers queued messages until none are left.
    void pump() {
        for (int i = 0; i < 1000 && !wire.empty(); ++i) {
            auto [toDisplay, message] = wire.front(); wire.pop_front();
            if (toDisplay) { if (!display.handle(peer(2), message)) displayMedia.handle(peer(2), message); }
            else if (!host.handle(peer(1), message)) hostMedia.handle(peer(1), message);
        }
    }
    QStringList sentTypes(bool toDisplay) const {
        QStringList out; for (const auto& [to, m] : wire) if (to == toDisplay) out << m.value("t").toString(); return out;
    }
};

// Pumps while waiting for `condition` (the monitor is made on a worker thread).
template <typename F>
bool settle(Pair& pair, F condition, int ms = 3000) {
    QElapsedTimer timer; timer.start();
    while (timer.elapsed() < ms) {
        pair.pump();
        if (condition()) return true;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(5);
    }
    pair.pump();
    return condition();
}

QString lastState(const QSignalSpy& spy) { return spy.isEmpty() ? QString() : spy.back()[1].toMap().value("state").toString(); }
QString lastDetail(const QSignalSpy& spy) { return spy.isEmpty() ? QString() : spy.back()[1].toMap().value("detail").toString(); }

}  // namespace

class DisplayServiceTest : public QObject {
    Q_OBJECT
private slots:
    void init() { alive = 0; creates = 0; failCreate = false; createDelayMs = 0; }

    void modes() {
        // Matching a 2732x2048 120 Hz retina tablet: 4K-class limits keep it,
        // H.264's macroblock rate caps the frame rate.
        auto mode = resolveMode({}, {2732, 2048, 120, 264}, 3840, 165, true);
        QCOMPARE(mode.width, 2732); QCOMPARE(mode.height, 2048); QVERIFY(mode.hiDpi);
        QVERIFY(mode.refreshMilliHz / 1000 <= 120);
        QVERIFY(qint64((2732 + 15) / 16) * ((2048 + 15) / 16) * (mode.refreshMilliHz / 1000) <= 2073600);
        // A wide screen above the encoder's width keeps its shape.
        mode = resolveMode({}, {5120, 1440, 60, 110}, 3840, 165, false);
        QCOMPARE(mode.width, 3840); QCOMPARE(mode.height, 1080); QVERIFY(!mode.hiDpi);
        // A fixed request, within the decoder's limits.
        mode = resolveMode({1920, 1080, 144}, {1280, 800, 60, 110}, 1920, 60, false);
        QCOMPARE(mode.width, 1920); QCOMPARE(mode.refreshMilliHz, 60000);
        // Never below the driver's minimum.
        mode = resolveMode({}, {320, 200, 10, 96}, 3840, 165, false);
        QVERIFY(mode.width >= 640 && mode.height >= 480 && mode.refreshMilliHz >= 24000);
    }

    // Asked, allowed, live, then stopped from the display device: the monitor
    // exists only while the session does.
    void consentedSessionLifecycle() {
        Pair pair;
        QSignalSpy hostState(&pair.host, &DisplayService::hostStateChanged);
        QSignalSpy prompt(&pair.display, &DisplayService::consentRequested), show(&pair.display, &DisplayService::showRequested);
        pair.host.offer(peer(1), {});
        pair.pump();
        QCOMPARE(prompt.size(), 1); QCOMPARE(lastState(hostState), QString("waiting")); QCOMPARE(alive.load(), 0);
        const auto request = prompt.back()[1].toUInt();
        QCOMPARE(prompt.back()[2].toMap().value("screens").toList().size(), 1);
        QSignalSpy viewer(&pair.displayMedia, &ScreenService::viewerChanged);
        pair.display.consent(hexOf(2), request, true, false, 0);
        QCOMPARE(show.size(), 1);
        QVERIFY(settle(pair, [&] { return lastState(hostState) == "live"; }));
        QCOMPARE(alive.load(), 1); QCOMPARE(pair.host.monitorCount(), 1);
        QCOMPARE(hostState.back()[1].toMap().value("width").toInt(), 2732);
        // The display device saw the media session start.
        QVERIFY(std::any_of(viewer.begin(), viewer.end(), [](const auto& v) { return v[1].toMap().value("state") == "live"; }));

        pair.display.stop(peer(2));
        pair.pump();
        QCOMPARE(alive.load(), 0); QCOMPARE(lastState(hostState), QString("ended"));
        QCOMPARE(pair.host.monitorCount(), 0);
    }

    void declinedRequestMakesNoMonitor() {
        Pair pair;
        QSignalSpy hostState(&pair.host, &DisplayService::hostStateChanged), prompt(&pair.display, &DisplayService::consentRequested);
        pair.host.offer(peer(1), {}); pair.pump();
        pair.display.consent(hexOf(2), prompt.back()[1].toUInt(), false, false, 0); pair.pump();
        QCOMPARE(lastState(hostState), QString("ended")); QVERIFY(lastDetail(hostState).contains("declined"));
        QCOMPARE(creates.load(), 0);
    }

    void refusedWithoutTrustOrPermission() {
        {   // the display device does not grant display.show
            Pair pair; pair.displaySeesHost.permissions.clear();
            QSignalSpy hostState(&pair.host, &DisplayService::hostStateChanged), prompt(&pair.display, &DisplayService::consentRequested);
            pair.host.offer(peer(1), {}); pair.pump();
            QCOMPARE(prompt.size(), 0); QCOMPARE(lastState(hostState), QString("ended"));
            QVERIFY(lastDetail(hostState).contains("trust"));
        }
        {   // the display device does not trust the host
            Pair pair; pair.displaySeesHost.trusted = false;
            QSignalSpy prompt(&pair.display, &DisplayService::consentRequested);
            pair.host.offer(peer(1), {}); pair.pump();
            QCOMPARE(prompt.size(), 0);
        }
        {   // the host only asks devices it trusts
            Pair pair; pair.hostSeesDisplay.trusted = false;
            pair.host.offer(peer(1), {});
            QVERIFY(pair.wire.empty());
        }
        {   // a device not offering the display service
            Pair pair; pair.display.setOffered(false);
            QSignalSpy prompt(&pair.display, &DisplayService::consentRequested);
            pair.host.offer(peer(1), {}); pair.pump();
            QCOMPARE(prompt.size(), 0);
        }
        QCOMPARE(creates.load(), 0);
    }

    void directPermissionSkipsThePrompt() {
        Pair pair; pair.displaySeesHost.permissions.insert(perm::DisplayDirect);
        QSignalSpy hostState(&pair.host, &DisplayService::hostStateChanged), prompt(&pair.display, &DisplayService::consentRequested);
        pair.host.offer(peer(1), {1920, 1080, 60});
        QVERIFY(settle(pair, [&] { return lastState(hostState) == "live"; }));
        QCOMPARE(prompt.size(), 0); QCOMPARE(alive.load(), 1);
        QCOMPARE(hostState.back()[1].toMap().value("width").toInt(), 1920);
        pair.host.stop(peer(1)); pair.pump();
        QCOMPARE(alive.load(), 0);
    }

    void rememberedConsentGrantsDirect() {
        Pair pair;
        QSignalSpy remember(&pair.display, &DisplayService::rememberPermission), prompt(&pair.display, &DisplayService::consentRequested);
        pair.host.offer(peer(1), {}); pair.pump();
        pair.display.consent(hexOf(2), prompt.back()[1].toUInt(), true, true, 0);
        QCOMPARE(remember.size(), 1); QCOMPARE(remember.back()[1].toString(), perm::DisplayDirect);
        pair.pump(); pair.host.reset(); pair.pump();
        QTRY_COMPARE(alive.load(), 0);
    }

    // Every way a live session ends removes the monitor.
    void everyEndingRemovesTheMonitor_data() {
        QTest::addColumn<QString>("how");
        for (const char* how : {"host stop", "peer gone", "silence", "relay path", "trust withdrawn", "reset", "destroyed"})
            QTest::newRow(how) << QString(how);
    }
    void everyEndingRemovesTheMonitor() {
        QFETCH(QString, how);
        auto pair = std::make_unique<Pair>();
        pair->displaySeesHost.permissions.insert(perm::DisplayDirect);
        QSignalSpy hostState(&pair->host, &DisplayService::hostStateChanged);
        pair->host.offer(peer(1), {});
        QVERIFY(settle(*pair, [&] { return lastState(hostState) == "live"; }));
        QCOMPARE(alive.load(), 1);
        if (how == "host stop") pair->host.stop(peer(1));
        else if (how == "peer gone") { pair->hostMedia.peerGone(peer(1)); pair->host.peerGone(peer(1)); }
        else if (how == "silence") { pair->time += 6000; pair->hostMedia.tick(); }
        else if (how == "relay path") pair->hostMedia.pathChanged(peer(1), "relayed");
        else if (how == "trust withdrawn") { pair->hostSeesDisplay.trusted = false; pair->hostMedia.policyChanged(); pair->host.policyChanged(); }
        else if (how == "reset") pair->host.reset();
        else if (how == "destroyed") pair.reset();
        if (pair) pair->pump();
        QCOMPARE(alive.load(), 0);
        if (pair) QCOMPARE(lastState(hostState), QString("ended"));
    }

    // The session ends while the monitor is still being made: it is removed
    // as soon as it exists.
    void endingWhileCreating() {
        createDelayMs = 300;
        Pair pair; pair.displaySeesHost.permissions.insert(perm::DisplayDirect);
        QSignalSpy hostState(&pair.host, &DisplayService::hostStateChanged);
        pair.host.offer(peer(1), {}); pair.pump();
        QCOMPARE(lastState(hostState), QString("creating"));
        pair.host.stop(peer(1)); pair.pump();
        QVERIFY(settle(pair, [&] { return creates.load() == 1; }));
        QTRY_COMPARE(alive.load(), 0);
        QCOMPARE(pair.host.monitorCount(), 0);
    }

    void creationFailureEndsBothSides() {
        failCreate = true;
        Pair pair; pair.displaySeesHost.permissions.insert(perm::DisplayDirect);
        QSignalSpy hostState(&pair.host, &DisplayService::hostStateChanged), viewer(&pair.displayMedia, &ScreenService::viewerChanged);
        pair.host.offer(peer(1), {});
        QVERIFY(settle(pair, [&] { return lastState(hostState) == "ended"; }));
        QVERIFY(lastDetail(hostState).contains("no driver here"));
        QVERIFY(std::any_of(viewer.begin(), viewer.end(), [](const auto& v) { return v[1].toMap().value("state") == "ended"; }));
        QCOMPARE(alive.load(), 0);
    }

    void unansweredRequestsExpire() {
        Pair pair;
        QSignalSpy hostState(&pair.host, &DisplayService::hostStateChanged), closed(&pair.display, &DisplayService::consentClosed);
        pair.host.offer(peer(1), {}); pair.pump();
        pair.time += 36000;
        QTRY_VERIFY_WITH_TIMEOUT(lastState(hostState) == "ended", 2000);
        pair.pump();
        QTRY_VERIFY_WITH_TIMEOUT(!closed.isEmpty(), 2000);
        QCOMPARE(creates.load(), 0);
    }

    void oneDisplayAtATime() {
        Pair pair;
        pair.host.offer(peer(1), {}); pair.pump();
        // A second host asks the same display device.
        QVector<QJsonObject> answers;
        pair.display.handle(peer(3), {{"t", "dsp.request"}, {"request", 7}, {"mode", "match"}, {"codecs", QJsonArray{"h264"}}});
        for (const auto& [to, m] : pair.wire) if (!to && m.value("request").toInt() == 7) answers << m;
        QVERIFY(std::any_of(answers.begin(), answers.end(), [](const auto& m) { return m.value("code") == "busy"; }));
    }
};

QTEST_GUILESS_MAIN(DisplayServiceTest)
#include "DisplayServiceTest.moc"
