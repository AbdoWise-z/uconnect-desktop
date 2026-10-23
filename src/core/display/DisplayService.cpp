#include "DisplayService.hpp"

#include <QDebug>

#include <QJsonArray>
#include <algorithm>
#include <cmath>

#include "core/Permissions.hpp"
#include "core/screen/MediaPacket.hpp"

namespace ucd::display {
namespace {
QString hex(const uconnect::DevId& peer) { return QString::fromStdString(uconnect::to_hex(peer)); }

quint32 requestOf(const QJsonObject& message) {
    const double value = message.value("request").toDouble();
    return value >= 1 && value <= 4294967295. && std::floor(value) == value ? quint32(value) : 0;
}

QString describe(const QString& code, const QString& detail) {
    if (!detail.isEmpty()) return detail.left(300);
    if (code == "forbidden") return QStringLiteral("The other device has to allow this one to use its screen, in its special or network rules.");
    if (code == "busy") return QStringLiteral("The other device is already showing a display.");
    if (code == "denied") return QStringLiteral("The other device declined.");
    if (code == "codec") return QStringLiteral("The other device cannot decode this video.");
    if (code == "driver") return QStringLiteral("The virtual display could not be created.");
    return code;
}

// H.264 level 5.2: 2,073,600 macroblocks a second (4K at 60 Hz).
constexpr qint64 MaxMacroblockRate = 2073600;
}  // namespace

Mode resolveMode(const Request& request, const LocalScreen& screen, int maxWidth, int maxFps, bool hiDpiCapable) {
    const bool match = request.width <= 0 || request.height <= 0;
    int width = match ? screen.width : request.width;
    int height = match ? screen.height : request.height;
    int fps = match ? screen.fps : request.fps;
    // What the encoder here and the decoder there handle, keeping the shape.
    const int widthLimit = std::clamp(maxWidth, 640, 3840);
    if (width > widthLimit) { height = int(qint64(height) * widthLimit / width); width = widthLimit; }
    if (height > 2400) { width = int(qint64(width) * 2400 / height); height = 2400; }
    width = std::max(640, width & ~1); height = std::max(480, height & ~1);
    fps = std::clamp(fps, 24, std::clamp(maxFps, 24, screen::MaxFps));
    const qint64 macroblocks = qint64((width + 15) / 16) * ((height + 15) / 16);
    if (macroblocks * fps > MaxMacroblockRate) fps = std::max(24, int(MaxMacroblockRate / macroblocks));
    return {width, height, fps * 1000, hiDpiCapable && screen.dpi >= 180};
}

DisplayService::DisplayService(Send send, Authorize authorize, screen::ScreenService* media, Factory factory,
        StatusCheck availability, QObject* parent, Now now)
    : QObject(parent), send_(std::move(send)), authorize_(std::move(authorize)), media_(media),
      factory_(std::move(factory)), availability_(std::move(availability)),
      now_(now ? std::move(now) : screen::ScreenService::monotonicMs) {
    timer_.setParent(this); timer_.setInterval(500);
    connect(&timer_, &QTimer::timeout, this, &DisplayService::tick);
    // The media session ended (stop, disconnect, silence, the path, a
    // capture failure...): the monitor goes with it.
    connect(media_, &screen::ScreenService::hostEnded, this, [this](const QString& peerId, const QString& reason) {
        if (const auto peer = uconnect::from_hex<uconnect::kDevIdLen>(peerId.toStdString()))
            end(*peer, "ended", reason, false);
    });
    connect(media_, &screen::ScreenService::viewerChanged, this, [this](const QString& peerId, const QVariantMap& update) {
        const auto state = update.value("state").toString();
        if (state != "ended" && state != "denied" && state != "busy") return;
        if (const auto peer = uconnect::from_hex<uconnect::kDevIdLen>(peerId.toStdString())) shown_.erase(*peer);
    });
}

DisplayService::~DisplayService() {
    timer_.stop();
    // Workers report back to this object: let them finish first. Monitors
    // they made are destroyed with the queued reports, which are dropped.
    for (auto* worker : std::as_const(workers_)) worker->wait();
    qDeleteAll(workers_);
    for (auto& [peer, hosted] : hosted_) if (hosted.monitor) hosted.monitor->destroy();
}

int DisplayService::monitorCount() const {
    return int(std::count_if(hosted_.begin(), hosted_.end(), [](const auto& entry) { return entry.second.monitor != nullptr; }));
}

// --- the host ------------------------------------------------------------------

void DisplayService::offer(const Peer& peer, const Request& request) {
    const auto access = authorize_(peer);
    if (!access.trusted) {
        emit hostStateChanged(hex(peer), {{"state", "ended"}, {"detail", tr("Give the other device special rules before using it as a display.")}});
        return;
    }
    if (const auto status = availability_(); status.availability != Availability::Ready) {
        emit hostStateChanged(hex(peer), {{"state", "ended"}, {"detail", status.detail}});
        return;
    }
    // One extended display at a time.
    if (!hosted_.empty() && !hosted_.contains(peer)) {
        emit hostStateChanged(hex(peer), {{"state", "ended"}, {"detail", tr("Another extended display is already running.")}});
        return;
    }
    if (hosted_.contains(peer)) end(peer, "ended", "replaced", true);
    Hosted hosted;
    hosted.request = nextRequest_++; if (!nextRequest_) ++nextRequest_;
    hosted.asked = request;
    hosted.expires = now_() + 35000;
    const auto requestId = hosted.request;
    hosted_.emplace(peer, std::move(hosted));
    timer_.start();
    QJsonValue mode = QStringLiteral("match");
    if (request.width > 0 && request.height > 0)
        mode = QJsonObject{{"width", request.width}, {"height", request.height}, {"fps", request.fps}};
    showHost(peer);
    if (!send_(peer, {{"t", "dsp.request"}, {"request", double(requestId)}, {"mode", mode}, {"codecs", QJsonArray{"h264"}}}))
        end(peer, "ended", tr("The other device is not connected."), false);
}

void DisplayService::stop(const Peer& peer, const QString& reason) {
    if (hosted_.contains(peer)) end(peer, "ended", reason, true);
    else if (const auto it = shown_.find(peer); it != shown_.end()) {
        const auto request = it->second.request;
        shown_.erase(it);
        emit consentClosed(hex(peer), request);
        media_->stopViewer(peer, reason);
    }
}

void DisplayService::end(const Peer& peer, const QString& state, const QString& detail, bool tellPeer) {
    const auto it = hosted_.find(peer);
    if (it == hosted_.end()) return;
    // The monitor first: nothing below may leave it behind.
    auto monitor = std::move(it->second.monitor);
    const quint32 request = it->second.request;
    const bool live = it->second.live;
    hosted_.erase(it);
    if (monitor) monitor->destroy();
    if (live) media_->stopHost(peer, detail);  // tells the peer; its hostEnded finds nothing left here
    else if (tellPeer) send_(peer, {{"t", "dsp.stop"}, {"request", double(request)}});
    emit hostStateChanged(hex(peer), {{"state", state}, {"detail", detail}});
    if (monitor) emit activity(tr("Extended display on %1 ended: %2").arg(authorize_(peer).name, detail));
}

void DisplayService::accepted(const Peer& peer, quint32 request, const LocalScreen& screen, int maxWidth, int maxFps) {
    auto it = hosted_.find(peer);
    if (it == hosted_.end() || it->second.request != request || it->second.live || it->second.state == "creating") return;
    const auto access = authorize_(peer);
    if (!access.trusted) {
        send_(peer, {{"t", "dsp.err"}, {"request", double(request)}, {"code", "forbidden"}});
        end(peer, "ended", tr("The other device's rules here no longer allow it."), false);
        return;
    }
#ifdef Q_OS_MACOS
    const bool hiDpi = true;
#else
    const bool hiDpi = false;
#endif
    auto& hosted = it->second;
    hosted.mode = resolveMode(hosted.asked, screen, maxWidth, maxFps, hiDpi);
    hosted.state = "creating";
    hosted.expires = now_() + 20000;
    hosted.generation = nextGeneration_++;
    showHost(peer);

    std::shared_ptr<VirtualDisplay> monitor(factory_ ? factory_() : nullptr);
    if (!monitor) {
        send_(peer, {{"t", "dsp.err"}, {"request", double(request)}, {"code", "driver"}});
        end(peer, "ended", tr("Extended displays are not supported here."), false);
        return;
    }
    // Plugging a monitor in waits for the OS: off this thread.
    const auto identity = monitorIdentity(access.key);
    const auto name = access.name;
    const auto mode = hosted.mode;
    const auto generation = hosted.generation;
    QThread* worker = QThread::create([this, peer, generation, monitor, identity, name, mode] {
        QString error;
        if (!monitor->create(identity, name, mode, &error) && error.isEmpty()) error = tr("The virtual display could not be created.");
        QMetaObject::invokeMethod(this, [this, peer, generation, monitor, error] { created(peer, generation, monitor, error); });
    });
    workers_.push_back(worker);
    connect(worker, &QThread::finished, this, [this, worker] { workers_.removeAll(worker); worker->deleteLater(); });
    worker->start();
}

void DisplayService::created(const Peer& peer, quint64 generation, std::shared_ptr<VirtualDisplay> monitor, const QString& error) {
    const auto it = hosted_.find(peer);
    if (it == hosted_.end() || it->second.generation != generation) {
        monitor->destroy();  // the session ended while the monitor was being made
        return;
    }
    const quint32 request = it->second.request;
    if (!error.isEmpty()) {
        monitor->destroy();
        send_(peer, {{"t", "dsp.err"}, {"request", double(request)}, {"code", "driver"}, {"detail", error.left(300)}});
        end(peer, "ended", error, false);
        return;
    }
    it->second.monitor = monitor;
    const auto mode = it->second.mode;
    const QSize pixels = monitor->pixels().isEmpty() ? QSize(mode.width, mode.height) : monitor->pixels();
    screen::ScreenSettings settings;
    settings.ciscoLibrary = codec_.ciscoLibrary;
    settings.offered = true;
    settings.gpuCapture = nativeCapture_;
    settings.cursorEmbedded = false;
    settings.maxFps = mode.refreshMilliHz / 1000;
    settings.maxWidth = pixels.width();
    settings.maxBitrate = it->second.asked.maxBitrate;
    settings.maxViewers = 1;
    settings.monitors = {monitorInfo(*monitor, authorize_(peer).name)};
    settings.monitor = 0;
    it->second.media = settings;
    const auto area = settings.monitors.first().toMap();
    qInfo().noquote() << QStringLiteral("[display] the virtual monitor starts at %1,%2 %3x%4")
        .arg(area.value("inputX").toInt()).arg(area.value("inputY").toInt())
        .arg(area.value("inputWidth").toInt()).arg(area.value("inputHeight").toInt());
    media_->configure(settings);
    const bool started = media_->startDisplayHost(peer, request, pixels.width(), settings.maxFps);
    // A failed start ends the media session, and with it (hostEnded) this one.
    const auto now = hosted_.find(peer);
    if (!started || now == hosted_.end()) {
        if (now != hosted_.end()) end(peer, "ended", tr("The session could not start."), true);
        return;
    }
    now->second.live = true;
    now->second.state = "live";
    showHost(peer, {{"captureId", monitor->captureId()}});
    emit activity(tr("Extended display on %1: %2x%3 at %4 Hz").arg(authorize_(peer).name)
        .arg(pixels.width()).arg(pixels.height()).arg(settings.maxFps));
}

void DisplayService::showHost(const Peer& peer, const QVariantMap& extra) {
    const auto it = hosted_.find(peer);
    if (it == hosted_.end()) return;
    QVariantMap state = extra;
    state["state"] = it->second.state;
    state["detail"] = QString();
    // The mode is known once the other device answered with its screen.
    if (it->second.state == "creating" || it->second.live) {
        state["width"] = it->second.mode.width; state["height"] = it->second.mode.height;
        state["fps"] = it->second.mode.refreshMilliHz / 1000;
    }
    emit hostStateChanged(hex(peer), state);
}

// --- the display device --------------------------------------------------------

void DisplayService::setOffered(bool offered) {
    offered_ = offered;
    if (!offered)
        while (!shown_.empty()) stop(shown_.begin()->first, "display service disabled");
}

void DisplayService::setLocalScreens(const QVariantList& screens) { screens_ = screens; }

LocalScreen DisplayService::localScreen(int index) const {
    const auto info = screens_.value(std::clamp(index, 0, std::max(0, int(screens_.size()) - 1))).toMap();
    LocalScreen screen;
    screen.width = info.value("width", 1920).toInt(); screen.height = info.value("height", 1080).toInt();
    screen.fps = info.value("fps", 60).toInt(); screen.dpi = info.value("dpi", 96).toDouble();
    return screen;
}

void DisplayService::refuse(const Peer& peer, quint32 request, const QString& code, const QString& detail) {
    QJsonObject message{{"t", "dsp.err"}, {"request", double(request)}, {"code", code}};
    if (!detail.isEmpty()) message["detail"] = detail;
    send_(peer, message);
    emit activity(tr("Display request from %1: %2").arg(authorize_(peer).name, code));
}

void DisplayService::consent(const QString& peerId, quint32 request, bool allow, bool remember, int screen) {
    const auto peer = uconnect::from_hex<uconnect::kDevIdLen>(peerId.toStdString());
    if (!peer) return;
    const auto it = shown_.find(*peer);
    if (it == shown_.end() || it->second.request != request || it->second.accepted) return;
    const auto access = authorize_(*peer);
    if (!allow || now_() >= it->second.expires || !offered_ || !access.trusted || !access.permissions.contains(perm::DisplayShow)) {
        shown_.erase(it);
        refuse(*peer, request, "denied");
        emit consentClosed(peerId, request);
        return;
    }
    if (remember) emit rememberPermission(access.key, perm::DisplayDirect);
    accept(*peer, request, screen);
}

void DisplayService::accept(const Peer& peer, quint32 request, int screen) {
    const auto it = shown_.find(peer);
    if (it == shown_.end() || it->second.request != request || it->second.accepted) return;
    it->second.accepted = true;
    const auto local = localScreen(screen);
    media_->expectDisplay(peer, request, 3840, screen::MaxFps);
    send_(peer, {{"t", "dsp.accept"}, {"request", double(request)},
        {"screen", QJsonObject{{"width", local.width}, {"height", local.height}, {"fps", local.fps}, {"dpi", local.dpi}}},
        {"maxWidth", 3840}, {"maxFps", screen::MaxFps}});
    emit consentClosed(hex(peer), request);
    emit showRequested(hex(peer), std::clamp(screen, 0, std::max(0, int(screens_.size()) - 1)));
    emit activity(tr("Showing an extended display for %1").arg(authorize_(peer).name));
}

// --- both ----------------------------------------------------------------------

bool DisplayService::handle(const Peer& peer, const QJsonObject& message) {
    const auto type = message.value("t").toString();
    if (!type.startsWith("dsp.")) return false;
    const quint32 request = requestOf(message);

    if (type == "dsp.request") {
        if (!request) return true;
        const auto access = authorize_(peer);
        if (!offered_ || !access.verified || !access.trusted || !access.permissions.contains(perm::DisplayShow)) {
            refuse(peer, request, "forbidden"); return true;
        }
        if (!message.value("codecs").toArray().contains("h264")) { refuse(peer, request, "codec"); return true; }
        if (!shown_.empty()) { refuse(peer, request, "busy"); return true; }
        shown_[peer] = {request, now_() + 30000, false};
        timer_.start();
        if (access.permissions.contains(perm::DisplayDirect)) {
            accept(peer, request, 0);
        } else {
            send_(peer, {{"t", "dsp.wait"}, {"request", double(request)}});
            const auto mode = message.value("mode");
            emit consentRequested(hex(peer), request, {{"name", access.name}, {"fingerprint", access.fingerprint},
                {"screens", screens_}, {"mode", mode.isObject() ? mode.toObject().toVariantMap() : QVariant("match")}});
        }
        return true;
    }
    if (type == "dsp.wait") {
        if (const auto it = hosted_.find(peer); it != hosted_.end() && it->second.request == request && !it->second.live) {
            it->second.state = "waiting"; showHost(peer);
        }
        return true;
    }
    if (type == "dsp.err") {
        const auto code = message.value("code").toString().left(32);
        const auto detail = describe(code, message.value("detail").toString());
        if (const auto it = hosted_.find(peer); it != hosted_.end() && it->second.request == request) {
            end(peer, "ended", detail, false);
        } else if (const auto shown = shown_.find(peer); shown != shown_.end() && shown->second.request == request) {
            shown_.erase(shown);
            media_->stopViewer(peer, detail);
        }
        return true;
    }
    if (type == "dsp.accept") {
        const auto screen = message.value("screen").toObject();
        LocalScreen local;
        local.width = std::clamp(screen.value("width").toInt(1920), 640, 16384);
        local.height = std::clamp(screen.value("height").toInt(1080), 480, 16384);
        local.fps = std::clamp(screen.value("fps").toInt(60), 24, 480);
        local.dpi = std::clamp(screen.value("dpi").toDouble(96), 50., 1000.);
        accepted(peer, request, local, message.value("maxWidth").toInt(3840), message.value("maxFps").toInt(60));
        return true;
    }
    if (type == "dsp.stop") {
        // Before the media session starts, a stop names the request.
        if (const auto it = hosted_.find(peer); it != hosted_.end() && request && it->second.request == request && !it->second.live) {
            end(peer, "ended", tr("The other device cancelled."), false);
            return true;
        }
        if (const auto it = shown_.find(peer); it != shown_.end() && request && it->second.request == request && !it->second.accepted) {
            shown_.erase(it);
            emit consentClosed(hex(peer), request);
            return true;
        }
    }
    return false;  // the media session's messages
}

void DisplayService::peerGone(const Peer& peer) {
    end(peer, "ended", tr("The other device disconnected."), false);
    if (const auto it = shown_.find(peer); it != shown_.end()) {
        emit consentClosed(hex(peer), it->second.request);
        shown_.erase(it);
    }
}

void DisplayService::policyChanged() {
    QVector<Peer> untrusted, withdrawn;
    for (const auto& [peer, hosted] : hosted_) if (!authorize_(peer).trusted) untrusted.push_back(peer);
    for (const auto& [peer, shown] : shown_) {
        const auto access = authorize_(peer);
        if (!access.trusted || !access.permissions.contains(perm::DisplayShow)) withdrawn.push_back(peer);
    }
    for (const auto& peer : untrusted) end(peer, "ended", tr("The other device's rules here no longer allow it."), true);
    for (const auto& peer : withdrawn) stop(peer, "permission withdrawn");
}

void DisplayService::reset() {
    while (!hosted_.empty()) end(hosted_.begin()->first, "ended", "stopped", true);
    while (!shown_.empty()) stop(shown_.begin()->first, "stopped");
}

void DisplayService::tick() {
    const auto now = now_();
    QVector<Peer> silent, unanswered;
    for (auto& [peer, hosted] : hosted_) {
        if (hosted.monitor) hosted.monitor->keepAlive();
        // The pointer is placed on the monitor by its desktop area, which the
        // OS may settle after the monitor appears, and people move it.
        if (hosted.live && hosted.monitor && !hosted.media.monitors.isEmpty()) {
            const auto info = monitorInfo(*hosted.monitor, authorize_(peer).name);
            if (info != hosted.media.monitors.first().toMap()) {
                qInfo().noquote() << QStringLiteral("[display] the virtual monitor is now at %1,%2 %3x%4")
                    .arg(info.value("inputX").toInt()).arg(info.value("inputY").toInt())
                    .arg(info.value("inputWidth").toInt()).arg(info.value("inputHeight").toInt());
                hosted.media.monitors = {info};
                media_->configure(hosted.media);
            }
        }
        if (!hosted.live && now >= hosted.expires) silent.push_back(peer);
    }
    for (const auto& [peer, shown] : shown_) if (!shown.accepted && now >= shown.expires) unanswered.push_back(peer);
    for (const auto& peer : silent) end(peer, "ended", tr("The other device did not answer."), true);
    for (const auto& peer : unanswered) {
        const auto request = shown_.at(peer).request;
        shown_.erase(peer);
        refuse(peer, request, "denied");
        emit consentClosed(hex(peer), request);
    }
    if (hosted_.empty() && shown_.empty()) timer_.stop();
}

}  // namespace ucd::display
