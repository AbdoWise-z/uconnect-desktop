#include "ScreenController.hpp"
#include <QGuiApplication>
#include <QScreen>
#include <QSettings>
#include <QStandardPaths>
#include <QDir>

namespace ucd {
ScreenController::ScreenController(Session* session, QObject* parent)
    : QObject(parent), session_(session), shortcut_([this] { stopHosting(); }) {
    const QVariantMap defaults{{"monitor", 0}, {"maxFps", 30}, {"maxWidth", 1920}, {"maxBitrate", 8000000},
        {"maxViewers", 1}, {"stopShortcut", "Ctrl+Alt+Shift+S"}, {"softwareCodec", false}, {"codecLibrary", ""}};
    QSettings store;
    for (auto it = defaults.begin(); it != defaults.end(); ++it) settings_[it.key()] = store.value("screen/" + it.key(), it.value());
#ifdef UCD_HAS_SCREEN
    auto* service = session_->screen();
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
        if (hosting.isEmpty()) { shortcut_.disable(); shortcutError_.clear(); }
        else shortcutError_ = shortcut_.enable(settings_.value("stopShortcut").toString());
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
    for (const auto* monitor : QGuiApplication::screens()) settings.monitors.push_back(QVariantMap{
        {"name", monitor->name()}, {"width", monitor->size().width()}, {"height", monitor->size().height()}});
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
}
