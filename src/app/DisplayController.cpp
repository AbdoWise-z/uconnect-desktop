#include "DisplayController.hpp"

#include <QBuffer>
#include <QGuiApplication>
#include <QScreen>
#include <QSettings>
#include <QThread>
#ifdef UCD_HAS_SCREEN
#include "core/display/DriverSetup.hpp"
#endif
#ifdef UCD_HAS_SCREEN
#include "core/screen/Cursor.hpp"
#endif
#ifdef Q_OS_WIN
#include <windows.h>
#elif defined(Q_OS_MACOS)
#include <IOKit/pwr_mgt/IOPMLib.h>
#elif defined(UCD_HAS_DBUS)
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#endif

namespace ucd {
namespace {
std::optional<uconnect::DevId> parse(const QString& id) { return uconnect::from_hex<uconnect::kDevIdLen>(id.toStdString()); }

QString stateName(display::Availability availability) {
    switch (availability) {
    case display::Availability::Ready: return QStringLiteral("ready");
    case display::Availability::NotInstalled: return QStringLiteral("notInstalled");
    case display::Availability::Outdated: return QStringLiteral("outdated");
    case display::Availability::NotSupported: break;
    }
    return QStringLiteral("notSupported");
}
}  // namespace

DisplayController::DisplayController(Session* session, QObject* parent) : QObject(parent), session_(session) {
    // width 0: match the display device's screen.
    const QVariantMap defaults{{"width", 0}, {"height", 0}, {"fps", 60}, {"maxBitrate", 40000000}, {"keepAwake", true}};
    QSettings store;
    for (auto it = defaults.begin(); it != defaults.end(); ++it) settings_[it.key()] = store.value("display/" + it.key(), it.value());
    refreshDriver();
#ifdef UCD_HAS_SCREEN
    auto* displays = session_->displays();
    auto* media = session_->display();
    connect(displays, &display::DisplayService::hostStateChanged, this, [this](const QString& peer, const QVariantMap& update) {
        auto& state = hostStates_[peer];
        for (auto it = update.begin(); it != update.end(); ++it) state[it.key()] = it.value();
        if (update.value("state") == "ended" && !update.value("detail").toString().isEmpty()
            && update.value("detail") != "stopped") emit notify(update.value("detail").toString());
        const bool live = std::any_of(hostStates_.begin(), hostStates_.end(), [](const auto& s) { return s.value("state") == "live"; });
        if (live && !cursorTimer_.isActive()) cursorTimer_.start(); else if (!live) cursorTimer_.stop();
        emit hostStateChanged(peer); emit hostingChanged();
    });
    connect(displays, &display::DisplayService::consentRequested, this, [this](const QString& peer, quint32 request, QVariantMap info) {
        info["peer"] = peer; info["request"] = request; consents_.push_back(info); emit consentChanged();
    });
    connect(displays, &display::DisplayService::consentClosed, this, [this](const QString& peer, quint32 request) {
        for (int i = consents_.size() - 1; i >= 0; --i) {
            const auto info = consents_[i].toMap();
            if (info.value("peer") == peer && info.value("request").toUInt() == request) consents_.removeAt(i);
        }
        emit consentChanged();
    });
    connect(displays, &display::DisplayService::showRequested, this, &DisplayController::showRequested);
    connect(displays, &display::DisplayService::rememberPermission, this, &DisplayController::rememberPermission);
    connect(media, &screen::ScreenService::viewerChanged, this, [this](const QString& peer, const QVariantMap& update) {
        auto& state = showStates_[peer];
        for (auto it = update.begin(); it != update.end(); ++it) state[it.key()] = it.value();
        const bool showing = std::any_of(showStates_.begin(), showStates_.end(), [](const auto& s) {
            const auto name = s.value("state").toString(); return name == "live" || name == "requesting";
        });
        setKeepAwake(showing && settings_.value("keepAwake").toBool());
        emit showStateChanged(peer);
    });
    connect(media, &screen::ScreenService::frameReady, this, [this](const QString& peer, const QVideoFrame& frame) {
        if (const auto sink = sinks_.value(peer)) sink->setVideoFrame(frame);
    });
    // The pointer moves onto the virtual monitor like onto any other.
    cursorTimer_.setInterval(16);
    connect(&cursorTimer_, &QTimer::timeout, this, [this, media] {
        const auto cursor = screen::nativeCursor(cursorShape_); cursorShape_ = cursor.shape;
        QByteArray png;
        if (!cursor.image.isNull()) { QBuffer buffer(&png); buffer.open(QIODevice::WriteOnly); cursor.image.save(&buffer, "PNG"); }
        QMetaObject::invokeMethod(media, [media, cursor, png] { media->submitCursor(cursor.position, cursor.visible, png, cursor.hotspot); });
    });
    connect(qGuiApp, &QGuiApplication::screenAdded, this, &DisplayController::updateScreens);
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, &DisplayController::updateScreens);
    refreshCodec();
#endif
    updateScreens();
}

DisplayController::~DisplayController() { setKeepAwake(false); }

bool DisplayController::available() const {
#ifdef UCD_HAS_SCREEN
    return true;
#else
    return false;
#endif
}

QVariantList DisplayController::hosting() const {
    QVariantList out;
    for (auto it = hostStates_.begin(); it != hostStates_.end(); ++it) {
        if (it.value().value("state") == "ended") continue;
        auto entry = it.value(); entry["devId"] = it.key();
        out.push_back(entry);
    }
    return out;
}

void DisplayController::updateScreens() {
    QVariantList screens;
    for (const auto* screen : QGuiApplication::screens()) {
        QSize pixels = screen->size() * screen->devicePixelRatio();
#ifdef Q_OS_WIN
        DEVMODEW mode{}; mode.dmSize = sizeof(mode);
        if (EnumDisplaySettingsExW(reinterpret_cast<const wchar_t*>(screen->name().utf16()), ENUM_CURRENT_SETTINGS, &mode, 0))
            pixels = QSize(int(mode.dmPelsWidth), int(mode.dmPelsHeight));
#endif
        const double inches = screen->physicalSize().width() / 25.4;
        const double dpi = inches > 1 ? pixels.width() / inches : 96.0 * screen->devicePixelRatio();
        const QString label = screen->model().isEmpty() ? screen->name() : screen->model();
        screens.push_back(QVariantMap{{"name", label}, {"width", pixels.width()}, {"height", pixels.height()},
            {"fps", qRound(screen->refreshRate())}, {"dpi", qRound(dpi)}});
    }
    screens_ = screens;
#ifdef UCD_HAS_SCREEN
    auto* displays = session_->displays();
    QMetaObject::invokeMethod(displays, [displays, screens] { displays->setLocalScreens(screens); });
#endif
    emit screensChanged();
}

void DisplayController::refreshCodec() {
#ifdef UCD_HAS_SCREEN
    QSettings store;
    screen::ScreenSettings codec;
    if (store.value("screen/softwareCodec", false).toBool()) codec.ciscoLibrary = store.value("screen/codecLibrary").toString();
    auto* displays = session_->displays();
    QMetaObject::invokeMethod(displays, [displays, codec] { displays->setCodecSettings(codec); });
#endif
}

void DisplayController::configure(const QVariantMap& values) {
    for (auto it = values.begin(); it != values.end(); ++it) if (settings_.contains(it.key())) settings_[it.key()] = it.value();
    settings_["fps"] = std::clamp(settings_["fps"].toInt(), 24, 165);
    settings_["maxBitrate"] = std::clamp(settings_["maxBitrate"].toInt(), 2000000, 100000000);
    QSettings store;
    for (auto it = settings_.begin(); it != settings_.end(); ++it) store.setValue("display/" + it.key(), it.value());
    emit settingsChanged();
}

void DisplayController::offer(const QString& devId, const QVariantMap& mode) {
#ifdef UCD_HAS_SCREEN
    const auto peer = parse(devId); if (!peer) return;
    display::Request request;
    request.width = mode.value("width", 0).toInt(); request.height = mode.value("height", 0).toInt();
    request.fps = mode.value("fps", settings_.value("fps")).toInt();
    request.maxBitrate = settings_.value("maxBitrate").toInt();
    hostStates_[devId] = {{"state", "offering"}};
    emit hostStateChanged(devId); emit hostingChanged();
    auto* displays = session_->displays();
    QMetaObject::invokeMethod(displays, [displays, peer = *peer, request] { displays->offer(peer, request); });
#else
    Q_UNUSED(devId); Q_UNUSED(mode);
#endif
}

void DisplayController::stop(const QString& devId) {
#ifdef UCD_HAS_SCREEN
    const auto peer = parse(devId); if (!peer) return;
    auto* displays = session_->displays();
    QMetaObject::invokeMethod(displays, [displays, peer = *peer] { displays->stop(peer); });
#else
    Q_UNUSED(devId);
#endif
}

void DisplayController::stopAll() {
    for (auto it = hostStates_.begin(); it != hostStates_.end(); ++it) if (it.value().value("state") != "ended") stop(it.key());
    for (auto it = showStates_.begin(); it != showStates_.end(); ++it) if (it.value().value("state") != "ended") stop(it.key());
}

void DisplayController::consent(const QString& devId, quint32 request, bool allow, bool remember, int screen) {
#ifdef UCD_HAS_SCREEN
    auto* displays = session_->displays();
    QMetaObject::invokeMethod(displays, [=] { displays->consent(devId, request, allow, remember, screen); });
#else
    Q_UNUSED(devId); Q_UNUSED(request); Q_UNUSED(allow); Q_UNUSED(remember); Q_UNUSED(screen);
#endif
}

void DisplayController::attachSink(const QString& devId, QObject* sink) {
#ifdef UCD_HAS_SCREEN
    sinks_[devId] = qobject_cast<QVideoSink*>(sink);
#else
    Q_UNUSED(devId); Q_UNUSED(sink);
#endif
}

void DisplayController::refreshDriver() {
#ifdef UCD_HAS_SCREEN
    const auto status = display::status();
    driver_ = {{"state", stateName(status.availability)}, {"detail", status.detail},
#ifdef Q_OS_WIN
        {"needsDriver", true},
#else
        {"needsDriver", false},
#endif
        {"bundled", !display::bundledDriverInf().isEmpty()}, {"busy", driver_.value("busy", false)}};
#else
    driver_ = {{"state", "notSupported"}, {"detail", tr("This build has no screen features.")}, {"needsDriver", false}};
#endif
    emit driverChanged();
}

void DisplayController::changeDriver(const QStringList& arguments, const QString& done) {
    if (driver_.value("busy").toBool()) return;
    driver_["busy"] = true; emit driverChanged();
    // The UAC prompt and the install take a while: off the GUI thread.
    auto* worker = QThread::create([this, arguments, done] {
        QString error = tr("Not supported in this build.");
#ifdef UCD_HAS_SCREEN
        const bool ok = display::runElevated(arguments, &error);
#else
        const bool ok = false;
#endif
        QMetaObject::invokeMethod(this, [this, ok, error, done] {
            driver_["busy"] = false;
            refreshDriver();
            emit notify(ok ? done : error);
        });
    });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void DisplayController::installDriver() {
#ifdef UCD_HAS_SCREEN
    const QString inf = display::bundledDriverInf();
#else
    const QString inf;
#endif
    if (inf.isEmpty()) { emit notify(tr("No display driver package ships with this build (drivers/uconnect-vdd).")); return; }
    changeDriver({"--display-driver", "install", inf}, tr("The display driver is installed."));
}

void DisplayController::removeDriver() {
    changeDriver({"--display-driver", "remove"}, tr("The display driver was removed."));
}

void DisplayController::setKeepAwake(bool on) {
    if (on == awake_) return;
    awake_ = on;
    // The display device's screen is someone's monitor: it must not dim or sleep.
#ifdef Q_OS_WIN
    SetThreadExecutionState(on ? ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED : ES_CONTINUOUS);
#elif defined(Q_OS_MACOS)
    if (on) {
        IOPMAssertionID id = 0;
        if (IOPMAssertionCreateWithName(kIOPMAssertPreventUserIdleDisplaySleep, kIOPMAssertionLevelOn,
                CFSTR("uConnect is showing an extended display"), &id) == kIOReturnSuccess) awakeAssertion_ = id;
    } else if (awakeAssertion_) {
        IOPMAssertionRelease(awakeAssertion_); awakeAssertion_ = 0;
    }
#elif defined(UCD_HAS_DBUS)
    // Desktops offer one of these (KDE and most: the freedesktop one; GNOME:
    // its session manager's). Either ends with the process too.
    const auto bus = QDBusConnection::sessionBus();
    QDBusInterface saver(QStringLiteral("org.freedesktop.ScreenSaver"), QStringLiteral("/org/freedesktop/ScreenSaver"),
                         QStringLiteral("org.freedesktop.ScreenSaver"), bus);
    QDBusInterface gnome(QStringLiteral("org.gnome.SessionManager"), QStringLiteral("/org/gnome/SessionManager"),
                         QStringLiteral("org.gnome.SessionManager"), bus);
    const QString why = QStringLiteral("Showing an extended display");
    if (on) {
        if (const QDBusReply<uint> cookie = saver.call(QStringLiteral("Inhibit"), QStringLiteral("uConnect"), why); cookie.isValid()) {
            awakeAssertion_ = cookie.value(); awakeByGnome_ = false;
        } else if (const QDBusReply<uint> gnomeCookie = gnome.call(QStringLiteral("Inhibit"), QStringLiteral("uConnect"), uint(0), why,
                                                                   uint(8));  // 8: idle
                   gnomeCookie.isValid()) {
            awakeAssertion_ = gnomeCookie.value(); awakeByGnome_ = true;
        }
    } else if (awakeAssertion_) {
        if (awakeByGnome_) gnome.call(QStringLiteral("Uninhibit"), awakeAssertion_);
        else saver.call(QStringLiteral("UnInhibit"), awakeAssertion_);
        awakeAssertion_ = 0;
    }
#endif
}

}  // namespace ucd
