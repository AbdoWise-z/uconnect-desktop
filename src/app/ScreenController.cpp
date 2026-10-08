#include "ScreenController.hpp"
#include <QGuiApplication>
#include <QScreen>
#include <QSettings>
#include <QStandardPaths>
#include <QDir>
#include <QBuffer>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace ucd {
ScreenController::ScreenController(Session* session, QObject* parent)
    : QObject(parent), session_(session), shortcut_([this] { stopHosting(); }) {
    const QVariantMap defaults{{"monitor", 0}, {"maxFps", 30}, {"maxWidth", 1920}, {"maxBitrate", 8000000},
        {"maxViewers", 1}, {"stopShortcut", "Ctrl+Alt+Shift+S"}, {"softwareCodec", false}, {"codecLibrary", ""}};
    QSettings store;
    for (auto it = defaults.begin(); it != defaults.end(); ++it) settings_[it.key()] = store.value("screen/" + it.key(), it.value());
#ifdef UCD_HAS_SCREEN
    auto* service = session_->screen();
    moveTimer_.setInterval(9); moveTimer_.setTimerType(Qt::PreciseTimer);
    connect(&moveTimer_, &QTimer::timeout, this, [this] {
        const auto moves = moves_; moves_.clear();
        for (auto it = moves.begin(); it != moves.end(); ++it) input(it.key(), it.value());
    });
    moveTimer_.start();
    cursorTimer_.setInterval(33);
    connect(&cursorTimer_, &QTimer::timeout, this, [this, service] {
        const auto cursor = screen::nativeCursor(cursorShape_); cursorShape_ = cursor.shape;
        QByteArray png;
        if (!cursor.image.isNull()) { QBuffer buffer(&png); buffer.open(QIODevice::WriteOnly); cursor.image.save(&buffer, "PNG"); }
        QMetaObject::invokeMethod(service, [service, cursor, png] { service->submitCursor(cursor.position, cursor.visible, png, cursor.hotspot); });
    });
    captureSession_.setScreenCapture(&capture_); captureSession_.setVideoSink(&captureSink_);
    connect(&captureSink_, &QVideoSink::videoFrameChanged, service,
        [service](const QVideoFrame& frame) { service->submitCapture(frame); }, Qt::DirectConnection);
    connect(&capture_, &QScreenCapture::errorOccurred, this, [this](auto, const QString& error) {
        emit notify(error + tr(" Check Screen Recording permission on macOS.")); stopHosting();
    });
    connect(service, &screen::ScreenService::captureChanged, this, [this](bool active, int index) {
        capture_.stop();
        const auto screens = QGuiApplication::screens();
        if (active && !screens.isEmpty()) {
            capture_.setScreen(screens[std::clamp(index, 0, int(screens.size()) - 1)]); capture_.start();
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
    connect(service, &screen::ScreenService::imageReady, this, [this](const QString& peer, const QImage& image) {
        if (const auto sink = sinks_.value(peer)) {
            QVideoFrame frame(QVideoFrameFormat(image.size(), QVideoFrameFormat::Format_RGBA8888));
            if (frame.map(QVideoFrame::WriteOnly)) {
                for (int y = 0; y < image.height(); ++y) memcpy(frame.bits(0) + y * frame.bytesPerLine(0), image.constScanLine(y), image.width() * 4);
                frame.unmap(); sink->setVideoFrame(frame);
            }
        }
    });
    connect(service, &screen::ScreenService::hostingChanged, this, [this](const QVariantList& hosting) {
        hosting_ = hosting;
        if (hosting.isEmpty()) { shortcut_.disable(); shortcutError_.clear(); cursorTimer_.stop(); }
        else { shortcutError_ = shortcut_.enable(settings_.value("stopShortcut").toString()); cursorTimer_.start(); }
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
    connect(qGuiApp, &QGuiApplication::screenAdded, this, [this] { pushSettings(); });
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, [this] { stopHosting(); pushSettings(); });
#endif
    pushSettings();
}
ScreenController::~ScreenController() {
#ifdef UCD_HAS_SCREEN
    for (auto* filter : std::as_const(captures_)) delete filter;
    captures_.clear();
    capture_.stop();
#endif
}
bool ScreenController::available() const {
#ifdef UCD_HAS_SCREEN
    return screen::lgplCodecBuild();
#else
    return false;
#endif
}
void ScreenController::configure(const QVariantMap& values) {
    for (auto it = values.begin(); it != values.end(); ++it) if (settings_.contains(it.key())) settings_[it.key()] = it.value();
    settings_["maxFps"] = std::clamp(settings_["maxFps"].toInt(), 1, 60);
    settings_["maxWidth"] = std::clamp(settings_["maxWidth"].toInt(), 320, 3840);
    settings_["maxBitrate"] = std::clamp(settings_["maxBitrate"].toInt(), 150000, 20000000);
    settings_["maxViewers"] = 1;
    QSettings store;
    for (auto it = settings_.begin(); it != settings_.end(); ++it) store.setValue("screen/" + it.key(), it.value());
    pushSettings(); emit settingsChanged();
    if (!hosting_.isEmpty()) { shortcutError_ = shortcut_.enable(settings_.value("stopShortcut").toString()); emit hostingChanged(); }
}
void ScreenController::pushSettings() {
#ifdef UCD_HAS_SCREEN
    screen::ScreenSettings settings;
    settings.monitor = settings_.value("monitor").toInt(); settings.maxFps = settings_.value("maxFps").toInt();
    settings.maxWidth = settings_.value("maxWidth").toInt(); settings.maxBitrate = settings_.value("maxBitrate").toInt();
    if (settings_.value("softwareCodec").toBool()) settings.ciscoLibrary = settings_.value("codecLibrary").toString();
    for (const auto* monitor : QGuiApplication::screens()) {
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
        settings.monitors.push_back(QVariantMap{{"name", monitor->name()}, {"width", pixels.width()}, {"height", pixels.height()},
            {"inputX", input.x()}, {"inputY", input.y()}, {"inputWidth", input.width()}, {"inputHeight", input.height()}});
    }
    auto* service = session_->screen();
    QMetaObject::invokeMethod(service, [service, settings] { service->configure(settings); });
#endif
}
void ScreenController::request(const QString& id, bool control) {
#ifdef UCD_HAS_SCREEN
    const auto peer = uconnect::from_hex<uconnect::kDevIdLen>(id.toStdString()); if (!peer) return;
    auto* service = session_->screen();
    QMetaObject::invokeMethod(service, [service, peer, control] { service->request(*peer, control); });
#else
    Q_UNUSED(id); Q_UNUSED(control);
#endif
}
void ScreenController::stop(const QString& id) {
#ifdef UCD_HAS_SCREEN
    const auto peer = uconnect::from_hex<uconnect::kDevIdLen>(id.toStdString()); if (!peer) return;
    auto* service = session_->screen(); QMetaObject::invokeMethod(service, [service, peer] { service->stop(*peer); });
#else
    Q_UNUSED(id);
#endif
}
void ScreenController::stopHosting() {
#ifdef UCD_HAS_SCREEN
    auto* service = session_->screen(); QMetaObject::invokeMethod(service, [service] { service->stopHosting(); });
#endif
}
void ScreenController::consent(const QString& peer, quint32 request, bool allow, bool remember) {
#ifdef UCD_HAS_SCREEN
    auto* service = session_->screen();
    QMetaObject::invokeMethod(service, [service, peer, request, allow, remember] { service->consent(peer, request, allow, remember); });
#else
    Q_UNUSED(peer); Q_UNUSED(request); Q_UNUSED(allow); Q_UNUSED(remember);
#endif
}
void ScreenController::monitor(const QString& id, int index) {
#ifdef UCD_HAS_SCREEN
    const auto peer = uconnect::from_hex<uconnect::kDevIdLen>(id.toStdString()); if (!peer) return;
    auto* service = session_->screen(); QMetaObject::invokeMethod(service, [service, peer, index] { service->monitor(*peer, index); });
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
    auto* service = session_->screen(); QMetaObject::invokeMethod(service, [service, peer, on] { service->requestControl(*peer, on); });
#else
    Q_UNUSED(id); Q_UNUSED(on);
#endif
}
void ScreenController::stopControl() {
#ifdef UCD_HAS_SCREEN
    auto* service = session_->screen(); QMetaObject::invokeMethod(service, [service] { service->stopControl(); });
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
void ScreenController::input(const QString& id, const QJsonObject& message) {
    const auto peer = uconnect::from_hex<uconnect::kDevIdLen>(id.toStdString()); if (!peer) return;
    auto* service = session_->screen(); QMetaObject::invokeMethod(service, [service, peer, message] { service->sendInput(*peer, message); });
}
#endif
}
