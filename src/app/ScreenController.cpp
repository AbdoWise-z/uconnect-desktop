#include "ScreenController.hpp"
#include <QGuiApplication>
#include <QScreen>
#include <QSettings>
#include <QStandardPaths>
#include <QDir>
#include <QBuffer>
#include <QPermissions>
#include "core/screen/MediaTransport.hpp"  // MaxFps
#ifdef Q_OS_WIN
#include <windows.h>
#elif defined(Q_OS_MACOS)
#include <ApplicationServices/ApplicationServices.h>
#endif

namespace ucd {
ScreenController::ScreenController(Session* session, QObject* parent, bool camera)
    : QObject(parent), session_(session), camera_(camera) {
    const QVariantMap defaults{{"monitor", 0}, {"maxFps", 30}, {"maxWidth", 1920}, {"maxBitrate", 8000000},
        {"maxViewers", 1}, {"gpuCapture", true}, {"releaseUsage", 228}, {"stopShortcut", "Ctrl+Alt+Shift+S"}, {"softwareCodec", false}, {"codecLibrary", ""}};
    QSettings store;
    // The camera keeps its own settings (device, resolution, fps) separate from
    // screen sharing, so each is configured on its own.
    const QString ns = camera_ ? "camera/" : "screen/";
    for (auto it = defaults.begin(); it != defaults.end(); ++it) settings_[it.key()] = store.value(ns + it.key(), it.value());
#ifdef UCD_HAS_SCREEN
    auto* service = this->service();
    moveTimer_.setInterval(9); moveTimer_.setTimerType(Qt::PreciseTimer);
    connect(&moveTimer_, &QTimer::timeout, this, [this] {
        const auto moves = moves_; moves_.clear();
        for (auto it = moves.begin(); it != moves.end(); ++it) input(it.key(), it.value());
    });
    if (!camera_) moveTimer_.start();
    cursorTimer_.setInterval(33);
    connect(&cursorTimer_, &QTimer::timeout, this, [this, service] {
        const auto cursor = screen::nativeCursor(cursorShape_); cursorShape_ = cursor.shape;
        QByteArray png;
        if (!cursor.image.isNull()) { QBuffer buffer(&png); buffer.open(QIODevice::WriteOnly); cursor.image.save(&buffer, "PNG"); }
        QMetaObject::invokeMethod(service, [service, cursor, png] { service->submitCursor(cursor.position, cursor.visible, png, cursor.hotspot); });
    });
    if (!camera_) captureSession_.setScreenCapture(&capture_); captureSession_.setVideoSink(&captureSink_);
    connect(&captureSink_, &QVideoSink::videoFrameChanged, service,
        [service](const QVideoFrame& frame) { service->submitCapture(frame); }, Qt::DirectConnection);
    connect(&capture_, &QScreenCapture::errorOccurred, this, [service](auto, QString error) {
        qWarning().noquote() << "Qt screen capture failed:" << error;
#ifdef Q_OS_MACOS
        error += tr(" Check Screen Recording permission in System Settings.");
#endif
        QMetaObject::invokeMethod(service, [service, error] { service->captureFailed(error); });
    });
    connect(service, &screen::ScreenService::qtCaptureWanted, this, [this](bool wanted) {
        if (camera_) return;
        qtCaptureWanted_ = wanted;
        if (!wanted) capture_.stop();
        else if (!capture_.isActive()) capture_.start();  // the screen was chosen by captureChanged
    });
    connect(service, &screen::ScreenService::captureChanged, this, [this](bool active, int index) {
        ++sourceGeneration_;
        if (camera_) {
            captureSession_.setCamera(nullptr); cameraCapture_.reset();
            if (active) startCamera(index, sourceGeneration_);
            return;
        }
        capture_.stop();
        const auto screens = QGuiApplication::screens();
        if (active && !screens.isEmpty()) {
            // On Wayland Qt's FFmpeg backend invokes the ScreenCast portal.
            // Its user-selected source replaces the QScreen selection.
            if (!QGuiApplication::platformName().startsWith("wayland")) capture_.setScreen(screens[std::clamp(index, 0, int(screens.size()) - 1)]);
            if (qtCaptureWanted_) capture_.start();
        }
    });
    connect(service, &screen::ScreenService::viewerChanged, this, [this](const QString& peer, const QVariantMap& update) {
        auto& state = states_[peer]; for (auto it = update.begin(); it != update.end(); ++it) state[it.key()] = it.value();
        if (state.value("state") != "live" || !state.value("control").toBool()) {
            if (auto* capture = captures_.value(peer)) capture->setCaptured(false);
            moves_.remove(peer); buttons_.remove(peer);
        }
        if (!update.value("detail").toString().isEmpty()) emit notify(update.value("detail").toString());
        emit stateChanged(peer);
    });
    connect(service, &screen::ScreenService::frameReady, this, [this](const QString& peer, const QVideoFrame& frame) {
        if (const auto sink = sinks_.value(peer)) sink->setVideoFrame(frame);
    });
    connect(service, &screen::ScreenService::hostingChanged, this, [this](const QVariantList& hosting) {
        hosting_ = hosting;
        if (hosting.isEmpty()) cursorTimer_.stop();
        else if (!camera_) cursorTimer_.start();
        emit hostingChanged();
    });
    connect(service, &screen::ScreenService::consentRequested, this,
        [this](const QString& peer, quint32 request, QVariantMap info) {
            info["peer"] = peer; info["request"] = request; consents_.push_back(info); emit consentChanged();
        });
    connect(service, &screen::ScreenService::consentClosed, this, [this](const QString& peer, quint32 request) {
        for (int i = consents_.size() - 1; i >= 0; --i) {
            const auto info = consents_[i].toMap();
            if (info.value("peer") == peer && info.value("request").toUInt() == request) consents_.removeAt(i);
        }
        emit consentChanged();
    });
    connect(service, &screen::ScreenService::rememberPermission, this, &ScreenController::rememberPermission);
    connect(&devices_, &QMediaDevices::videoInputsChanged, this, [this] { if (camera_) { stopHosting(); pushSettings(); } });
    connect(qGuiApp, &QGuiApplication::screenAdded, this, [this] { pushSettings(); });
    // A share follows its monitor and ends only with it (ScreenService::
    // followMonitors), so ending an extended display leaves other shares be.
    // The desktop portal does not say which monitor it shares.
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, [this](QScreen* removed) {
        if (!camera_ && QGuiApplication::platformName().startsWith("wayland")) stopHosting();
        pushSettings(removed);
    });
#endif
    pushSettings();
}
ScreenController::~ScreenController() {
#ifdef UCD_HAS_SCREEN
    for (auto* filter : std::as_const(captures_)) delete filter;
    captures_.clear();
    capture_.stop(); captureSession_.setCamera(nullptr); cameraCapture_.reset();
#endif
}
bool ScreenController::available() const {
#ifdef UCD_HAS_SCREEN
    return screen::lgplCodecBuild();
#else
    return false;
#endif
}
void ScreenController::setShortcutError(const QString& error) {
    if (shortcutError_ == error) return; shortcutError_ = error; emit hostingChanged();
}
void ScreenController::configure(const QVariantMap& values) {
    for (auto it = values.begin(); it != values.end(); ++it) if (settings_.contains(it.key())) settings_[it.key()] = it.value();
    settings_["maxFps"] = std::clamp(settings_["maxFps"].toInt(), 1, screen::MaxFps);
    settings_["maxWidth"] = std::clamp(settings_["maxWidth"].toInt(), 320, 3840);
    settings_["maxBitrate"] = std::clamp(settings_["maxBitrate"].toInt(), 150000, 20000000);
    settings_["maxViewers"] = std::clamp(settings_["maxViewers"].toInt(), 1, 8);
    if (!QList<int>{228, 41, 69}.contains(settings_["releaseUsage"].toInt())) settings_["releaseUsage"] = 228;
    QSettings store;
    const QString ns = camera_ ? "camera/" : "screen/";
    for (auto it = settings_.begin(); it != settings_.end(); ++it) store.setValue(ns + it.key(), it.value());
    pushSettings(); emit settingsChanged();
}
void ScreenController::pushSettings(const QScreen* removed) {
#ifdef UCD_HAS_SCREEN
    screen::ScreenSettings settings;
    settings.monitor = settings_.value("monitor").toInt(); settings.maxFps = settings_.value("maxFps").toInt();
    settings.maxWidth = settings_.value("maxWidth").toInt(); settings.maxBitrate = settings_.value("maxBitrate").toInt();
    settings.maxViewers = settings_.value("maxViewers").toInt();
    settings.gpuCapture = settings_.value("gpuCapture").toBool();
#ifndef Q_OS_MACOS
    settings.cursorEmbedded = camera_ || QGuiApplication::platformName().startsWith("wayland");
#endif
    if (settings_.value("softwareCodec").toBool()) settings.ciscoLibrary = settings_.value("codecLibrary").toString();
    if (camera_) {
        settings.monitor = 0;
        for (const auto& device : QMediaDevices::videoInputs())
            settings.monitors.push_back(QVariantMap{{"name", device.description()}, {"width", 0}, {"height", 0}});
    } else for (const auto* monitor : QGuiApplication::screens()) {
        if (monitor == removed) continue;  // still listed while it is being removed
        QRect input = monitor->geometry();
        QSize pixels = monitor->size() * monitor->devicePixelRatio();
#ifdef Q_OS_WIN
        DEVMODEW mode{}; mode.dmSize = sizeof(mode);
        if (EnumDisplaySettingsExW(reinterpret_cast<const wchar_t*>(monitor->name().utf16()), ENUM_CURRENT_SETTINGS, &mode, 0)) {
            input = QRect(mode.dmPosition.x, mode.dmPosition.y, mode.dmPelsWidth, mode.dmPelsHeight); pixels = input.size();
        }
#elif !defined(Q_OS_MACOS)
        input = QRect(monitor->geometry().topLeft() * monitor->devicePixelRatio(), pixels);
#endif
        QString nativeId = monitor->name();
#ifdef Q_OS_MACOS
        CGDirectDisplayID displays[32]{}; uint32_t count = 0;
        if (CGGetActiveDisplayList(32, displays, &count) == kCGErrorSuccess) for (uint32_t i = 0; i < count; ++i) {
            const auto bounds = CGDisplayBounds(displays[i]);
            if (int(bounds.origin.x) == input.x() && int(bounds.origin.y) == input.y()) { nativeId = QString::number(displays[i]); break; }
        }
#endif
        settings.monitors.push_back(QVariantMap{{"nativeId", nativeId}, {"name", monitor->name()}, {"width", pixels.width()}, {"height", pixels.height()},
            {"inputX", input.x()}, {"inputY", input.y()}, {"inputWidth", input.width()}, {"inputHeight", input.height()}});
    }
    if (!camera_ && QGuiApplication::platformName().startsWith("wayland")) {
        settings.monitor = 0; settings.gpuCapture = false;
        settings.monitors = {QVariantMap{{"name", tr("Desktop portal")}, {"width", 0}, {"height", 0}}};
    }
    auto* service = this->service();
    QMetaObject::invokeMethod(service, [service, settings] { service->configure(settings); });
#endif
}
void ScreenController::request(const QString& id, bool control) {
#ifdef UCD_HAS_SCREEN
    const auto peer = uconnect::from_hex<uconnect::kDevIdLen>(id.toStdString()); if (!peer) return;
    auto* service = this->service();
    // Ask for what this device is set to show; the host caps it by its own settings.
    const int width = settings_.value("maxWidth").toInt(), fps = settings_.value("maxFps").toInt();
    QMetaObject::invokeMethod(service, [service, peer, control, width, fps] { service->request(*peer, control, width, fps); });
#else
    Q_UNUSED(id); Q_UNUSED(control);
#endif
}
void ScreenController::stop(const QString& id) {
#ifdef UCD_HAS_SCREEN
    const auto peer = uconnect::from_hex<uconnect::kDevIdLen>(id.toStdString()); if (!peer) return;
    auto* service = this->service(); QMetaObject::invokeMethod(service, [service, peer] { service->stopViewer(*peer); });
#else
    Q_UNUSED(id);
#endif
}
void ScreenController::stopHosting() {
#ifdef UCD_HAS_SCREEN
    auto* service = this->service(); QMetaObject::invokeMethod(service, [service] { service->stopHosting(); });
#endif
}
void ScreenController::consent(const QString& peer, quint32 request, bool allow, bool remember) {
#ifdef UCD_HAS_SCREEN
    auto* service = this->service();
    QMetaObject::invokeMethod(service, [service, peer, request, allow, remember] { service->consent(peer, request, allow, remember); });
#else
    Q_UNUSED(peer); Q_UNUSED(request); Q_UNUSED(allow); Q_UNUSED(remember);
#endif
}
void ScreenController::monitor(const QString& id, int index) {
#ifdef UCD_HAS_SCREEN
    const auto peer = uconnect::from_hex<uconnect::kDevIdLen>(id.toStdString()); if (!peer) return;
    auto* service = this->service(); QMetaObject::invokeMethod(service, [service, peer, index] { service->monitor(*peer, index); });
#else
    Q_UNUSED(id); Q_UNUSED(index);
#endif
}
void ScreenController::attachSink(const QString& peer, QObject* sink) {
#ifdef UCD_HAS_SCREEN
    sinks_[peer] = qobject_cast<QVideoSink*>(sink);
#else
    Q_UNUSED(peer); Q_UNUSED(sink);
#endif
}
void ScreenController::requestControl(const QString& id, bool on) {
#ifdef UCD_HAS_SCREEN
    const auto peer = uconnect::from_hex<uconnect::kDevIdLen>(id.toStdString()); if (!peer) return;
    if (!on) releaseInput(id);
    auto* service = this->service(); QMetaObject::invokeMethod(service, [service, peer, on] { service->requestControl(*peer, on); });
#else
    Q_UNUSED(id); Q_UNUSED(on);
#endif
}
void ScreenController::stopControl() {
#ifdef UCD_HAS_SCREEN
    auto* service = this->service(); QMetaObject::invokeMethod(service, [service] { service->stopControl(); });
#endif
}
void ScreenController::captureKeyboard(const QString& peer, QObject* window, bool capture) {
#ifdef UCD_HAS_SCREEN
    if (!states_.value(peer).value("control").toBool()) return;
    auto*& filter = captures_[peer];
    if (!filter) {
        filter = new InputCapture(this);
        connect(filter, &InputCapture::input, this, [this, peer](const QJsonObject& msg) {
            if (msg.value("t") == "in.release") { moves_.remove(peer); buttons_.remove(peer); }
            input(peer, msg);
        });
        connect(filter, &InputCapture::captureChanged, this, [this, peer](bool captured) {
            states_[peer]["keyboardCaptured"] = captured; emit stateChanged(peer);
        });
    }
    if (capture && !filter->captured()) filter->attach(window);
    filter->setReleaseUsage(quint16(settings_.value("releaseUsage", 228).toInt()));
    filter->setCaptured(capture);
#else
    Q_UNUSED(peer); Q_UNUSED(window); Q_UNUSED(capture);
#endif
}
void ScreenController::mouse(const QString& peer, double x, double y, int buttons) {
#ifdef UCD_HAS_SCREEN
    if (!states_.value(peer).value("control").toBool()) return;
    QJsonObject msg{{"t", "in.mouse"}, {"x", std::clamp(x, 0., 1.)}, {"y", std::clamp(y, 0., 1.)}, {"buttons", buttons & 7}};
    if (buttons != buttons_.value(peer)) { moves_.remove(peer); input(peer, msg); buttons_[peer] = buttons; }
    else moves_[peer] = msg;
#else
    Q_UNUSED(peer); Q_UNUSED(x); Q_UNUSED(y); Q_UNUSED(buttons);
#endif
}
void ScreenController::wheel(const QString& peer, int dx, int dy) {
#ifdef UCD_HAS_SCREEN
    input(peer, {{"t", "in.wheel"}, {"dx", std::clamp(dx, -2400, 2400)}, {"dy", std::clamp(dy, -2400, 2400)}});
#else
    Q_UNUSED(peer); Q_UNUSED(dx); Q_UNUSED(dy);
#endif
}
void ScreenController::releaseInput(const QString& peer) {
#ifdef UCD_HAS_SCREEN
    moves_.remove(peer); buttons_.remove(peer);
    if (auto* capture = captures_.value(peer)) capture->setCaptured(false);
    input(peer, {{"t", "in.release"}});
#else
    Q_UNUSED(peer);
#endif
}
#ifdef UCD_HAS_SCREEN
void ScreenController::startCamera(int index, quint64 generation) {
#if QT_CONFIG(permissions)
    QCameraPermission permission;
    const auto status = qApp->checkPermission(permission);
    if (status == Qt::PermissionStatus::Denied) { emit notify(tr("Allow uConnect to use your camera in system privacy settings.")); stopHosting(); return; }
    if (status == Qt::PermissionStatus::Undetermined) {
        qApp->requestPermission(permission, this, [this, index, generation](const QPermission& result) {
            if (generation != sourceGeneration_ || hosting_.isEmpty()) return;
            if (result.status() == Qt::PermissionStatus::Granted) startCamera(index, generation);
            else { emit notify(tr("Camera access was denied.")); stopHosting(); }
        });
        return;
    }
#endif
    const auto cameras = QMediaDevices::videoInputs();
    if (cameras.isEmpty()) { emit notify(tr("No camera is available.")); stopHosting(); return; }
    // Construct the native camera only after consent and OS authorization.
    captureSession_.setCamera(nullptr);
    cameraCapture_ = std::make_unique<QCamera>(cameras[std::clamp(index, 0, int(cameras.size()) - 1)]);
    connect(cameraCapture_.get(), &QCamera::errorOccurred, this, [this](auto, const QString& error) { emit notify(error); stopHosting(); });
    captureSession_.setCamera(cameraCapture_.get()); cameraCapture_->start();
}
void ScreenController::input(const QString& id, const QJsonObject& message) {
    const auto peer = uconnect::from_hex<uconnect::kDevIdLen>(id.toStdString()); if (!peer) return;
    auto* service = this->service(); QMetaObject::invokeMethod(service, [service, peer, message] { service->sendInput(*peer, message); });
}
#endif
}
