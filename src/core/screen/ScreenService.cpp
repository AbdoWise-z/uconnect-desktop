#include "ScreenService.hpp"

#include <QJsonArray>
#include <QRandomGenerator>
#include <QtEndian>
#include <chrono>
#include <cmath>
#include <QBuffer>
#include <QImageReader>
#include "core/Identity.hpp"

namespace ucd::screen {
namespace {
QString hex(const uconnect::DevId& peer) { return QString::fromStdString(uconnect::to_hex(peer)); }
QJsonObject message(const char* type, quint16 token) { return {{"t", type}, {"session", token}}; }
}

qint64 ScreenService::monotonicMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

ScreenService::ScreenService(Send send, Authorize authorize, Open open, Close close, Datagram datagram,
        QObject* parent, Now now, std::unique_ptr<InputBackend> input, Kind kind)
    : QObject(parent), send_(std::move(send)), authorize_(std::move(authorize)), open_(std::move(open)),
      close_(std::move(close)), datagram_(std::move(datagram)), now_(now ? std::move(now) : monotonicMs),
      kind_(kind), camera_(kind == Kind::Camera), display_(kind == Kind::Display),
      encoderWorker_(new QObject), decoderWorker_(new QObject), encoder_(std::make_unique<Encoder>()),
      input_(input ? std::move(input) : nativeInputBackend()) {
    // Camera and display sessions are screen sessions under their own names
    // on the wire: cam.* / dsp.* messages, their own datagram kinds.
    if (camera_ || display_) {
        const QString prefix = camera_ ? QStringLiteral("cam.") : QStringLiteral("dsp.");
        auto wireSend = std::move(send_);
        send_ = [wireSend, prefix](const Peer& peer, QJsonObject msg) {
            auto type = msg.value("t").toString(); if (type.startsWith("scr.")) type.replace(0, 4, prefix);
            msg["t"] = type; return wireSend(peer, msg);
        };
        const bool display = display_;
        auto wireDatagram = std::move(datagram_);
        datagram_ = [wireDatagram, display](const Peer& peer, QByteArray packet) {
            if (packet.isEmpty() || packet[0] != 1 && !(display && packet[0] == 2)) return false;
            packet[0] = char(display ? packet[0] + 3 : 3); return wireDatagram(peer, packet);
        };
    }
    timer_.setParent(this); timer_.setInterval(2); timer_.setTimerType(Qt::PreciseTimer);
    connect(&timer_, &QTimer::timeout, this, &ScreenService::tick);
    encoderWorker_->moveToThread(&encoderThread_); decoderWorker_->moveToThread(&decoderThread_);
    connect(&encoderThread_, &QThread::finished, encoderWorker_, &QObject::deleteLater);
    connect(&decoderThread_, &QThread::finished, decoderWorker_, &QObject::deleteLater);
    encoderThread_.setObjectName("screen-encoder"); decoderThread_.setObjectName("screen-decoder");
    encoderThread_.start(); decoderThread_.start();
}

ScreenService::~ScreenService() {
    timer_.stop();
    QMetaObject::invokeMethod(encoderWorker_, [this] { encoder_.reset(); desktop_.reset(); }, Qt::BlockingQueuedConnection);
    QMetaObject::invokeMethod(decoderWorker_, [this] { decoders_.clear(); }, Qt::BlockingQueuedConnection);
    encoderThread_.quit(); decoderThread_.quit(); encoderThread_.wait(); decoderThread_.wait();
}

void ScreenService::configure(const ScreenSettings& settings) {
    const bool offered = settings_.offered;
    const bool controlOffered = settings_.controlOffered;
    const bool gpuChanged = settings_.gpuCapture != settings.gpuCapture;
    settings_ = settings;
    settings_.offered = offered;
    settings_.controlOffered = controlOffered;
    settings_.maxFps = std::clamp(settings.maxFps, 1, MaxFps);
    settings_.maxWidth = std::clamp(settings.maxWidth, 320, 3840);
    // Extended displays run between nearby devices and need sharp text.
    settings_.maxBitrate = std::clamp(settings.maxBitrate, 150000, display_ ? CongestionController::MaxBitrate : 20000000);
    settings_.maxViewers = std::clamp(settings.maxViewers, 1, 8);
    if (gpuChanged) {
        resetNative(); updateQtCapture();
        for (auto& [peer, host] : hosts_) host.forceKey = true;
    }
    settings_.monitor = std::clamp(settings.monitor, 0, std::max(0, int(settings.monitors.size()) - 1));
    while (int(hosts_.size()) > settings_.maxViewers) stopHost(hosts_.rbegin()->first, "viewer limit reduced");
    // An extended display is not an offered service: DisplayService starts it.
    if (!settings_.offered && !display_) stopHosting("screen service disabled");
}
void ScreenService::setOffered(bool offered) {
    settings_.offered = offered;
    if (!offered) stopHosting("screen service disabled");
}
void ScreenService::setControlOffered(bool offered) {
    settings_.controlOffered = offered;
    if (!offered) stopControl();
}
bool ScreenService::controlAllowed(const Peer& peer) const {
    const auto access = authorize_(peer);
    return !camera_ && !display_ && settings_.controlOffered && access.trusted && access.permissions.contains(perm::ControlInput);
}

bool ScreenService::allowed(const Peer& peer) const {
    const auto access = authorize_(peer);
    // An extended display is sent only to a device this one trusts; the
    // receiving side checks its own permission (viewerAllowed).
    if (display_) return access.trusted;
    return settings_.offered && access.trusted && access.permissions.contains(camera_ ? perm::CameraView : perm::ScreenView);
}

bool ScreenService::viewerAllowed(const Peer& peer) const {
    const auto access = authorize_(peer);
    if (display_) return access.verified && access.trusted && access.permissions.contains(perm::DisplayShow);
    return access.verified;
}

void ScreenService::failMedia(const Peer& peer, const QString& detail) {
    const auto it = hosts_.find(peer);
    if (it == hosts_.end()) return;
    if (it->second.live) { auto failure = message("scr.error", it->second.token); failure["detail"] = detail; send_(peer, failure); }
    stopHost(peer, detail);
}

bool ScreenService::startDisplayHost(const Peer& peer, quint32 request, int width, int fps) {
    if (!display_ || !request || hosts_.contains(peer) || settings_.monitors.isEmpty() || !allowed(peer)) return false;
    Host host; host.request = request; host.expires = now_() + 30000;
    host.width = std::clamp(width, 320, 3840); host.fps = std::clamp(fps, 1, MaxFps);
    hosts_.emplace(peer, std::move(host)); timer_.start();
    startHost(peer);
    return hosts_.contains(peer);
}

void ScreenService::expectDisplay(const Peer& peer, quint32 request, int maxWidth, int fps) {
    if (!display_ || !request) return;
    if (viewers_.contains(peer)) stopViewer(peer);
    Viewer viewer;
    viewer.request = request;
    viewer.width = std::clamp(maxWidth, 320, 3840); viewer.fps = std::clamp(fps, 1, MaxFps);
    viewer.expires = now_() + 15000; viewer.reportAt = now_();
    viewers_.emplace(peer, std::move(viewer)); timer_.start();
    showViewer(peer, "requesting", {}, {{"pictured", false}, {"stalled", false}, {"hostStatus", QString()}});
}

void ScreenService::fail(const Peer& peer, quint32 request, const QString& code) {
    send_(peer, {{"t", "scr.err"}, {"request", double(request)}, {"code", code}});
    emit activity(QStringLiteral("Screen request from %1: %2").arg(authorize_(peer).name, code));
}

void ScreenService::request(const Peer& peer, bool control, int maxWidth, int fps) {
    // Watching needs to know who is sending; only the sharing device has to
    // trust the other one.
    if (!authorize_(peer).verified) {
        showViewer(peer, "denied", "This device has not proven its identity yet. Wait a moment and try again.");
        return;
    }
    if (viewers_.contains(peer)) stopViewer(peer);
    Viewer viewer;
    viewer.wantControl = control && !camera_;
    viewer.request = nextRequest_++; if (!nextRequest_) ++nextRequest_;
    viewer.width = std::clamp(maxWidth, 320, 3840); viewer.fps = std::clamp(fps, 1, MaxFps);
    viewer.expires = now_() + 35000; viewer.reportAt = now_();
    viewers_.emplace(peer, std::move(viewer)); timer_.start();
    showViewer(peer, "requesting", {}, {{"pictured", false}, {"stalled", false}, {"hostStatus", QString()}});
    if (!send_(peer, {{"t", "scr.request"}, {"request", double(viewers_.at(peer).request)},
        {"wantControl", viewers_.at(peer).wantControl}, {"maxWidth", maxWidth}, {"maxFps", fps}, {"codecs", QJsonArray{"h264"}}}))
        stopViewer(peer, "connection unavailable");
}

void ScreenService::consent(const QString& peerId, quint32 requestId, bool allow, bool remember) {
    const auto peer = uconnect::from_hex<uconnect::kDevIdLen>(peerId.toStdString());
    if (!peer) return;
    const auto it = hosts_.find(*peer);
    if (it == hosts_.end()) return;
    if (it->second.live) {
        if (it->second.controlRequest != requestId || !requestId) return;
        const bool approved = allow && now_() < it->second.controlExpires && controlAllowed(*peer);
        if (approved && remember) emit rememberPermission(authorize_(*peer).key, perm::ControlDirect);
        setControl(*peer, approved, requestId, approved ? QString() : QString("denied"));
        return;
    }
    if (it->second.request != requestId) return;
    if (!allow || now_() >= it->second.expires || !allowed(*peer)) {
        fail(*peer, requestId, "denied"); emit consentClosed(peerId, requestId); hosts_.erase(it); return;
    }
    if (remember && !camera_) {
        emit rememberPermission(authorize_(*peer).key, perm::ScreenDirect);
        if (it->second.wantControl) emit rememberPermission(authorize_(*peer).key, perm::ControlDirect);
    }
    startHost(*peer);
}

void ScreenService::startHost(const Peer& peer) {
    auto& host = hosts_.at(peer);
    if (!allowed(peer)) { fail(peer, host.request, "forbidden"); hosts_.erase(peer); emit hostEnded(hex(peer), "forbidden"); return; }
    host.token = quint16(QRandomGenerator::system()->bounded(1u, 65536u));
    host.live = true; host.lastReport = now_(); host.startedAt = now_();
    host.monitor = settings_.monitor;
    for (const auto& [other, existing] : hosts_) if (other != peer && existing.live) { host.monitor = existing.monitor; break; }
    host.congestion.setTolerant(display_);
    host.congestion.reset(host.path, host.fps, host.width, settings_.maxBitrate, display_ ? DisplayStartBitrate : 0);
    if (!open_(peer)) { fail(peer, host.request, "transport"); hosts_.erase(peer); emit hostEnded(hex(peer), "transport"); return; }
    emit consentClosed(hex(peer), host.request);
    host.control = host.wantControl && controlAllowed(peer) && input_.available().isEmpty();
    const auto bounds = settings_.monitors.value(host.monitor).toMap();
    input_.setBounds(QRect(bounds.value("inputX", bounds.value("x")).toInt(), bounds.value("inputY", bounds.value("y")).toInt(),
        bounds.value("inputWidth", bounds.value("width")).toInt(), bounds.value("inputHeight", bounds.value("height")).toInt()));
    send_(peer, {{"t", "scr.started"}, {"request", double(host.request)}, {"session", host.token},
        {"codec", "h264"}, {"width", host.width}, {"height", 0}, {"fps", host.fps},
        {"monitors", QJsonArray::fromVariantList(settings_.monitors)}, {"monitor", host.monitor}, {"control", host.control},
        {"pipeline", lastPipeline_}, {"cursorEmbedded", nativeActive_ ? false : settings_.cursorEmbedded},
        {"controlDetail", host.controlRefused}
    });
    if (std::count_if(hosts_.begin(), hosts_.end(), [](const auto& entry) { return entry.second.live; }) == 1)
        emit captureChanged(true, host.monitor);
    updateQtCapture();
    showHosting();
    emit activity(QStringLiteral("%1 started with %2").arg(display_ ? "Extended display" : "Screen sharing", authorize_(peer).name));
}

void ScreenService::showViewer(const Peer& peer, const QString& state, const QString& detail, const QVariantMap& extra) {
    QVariantMap update = extra; update["state"] = state; update["detail"] = detail;
    const auto it = viewers_.find(peer);
    // The viewer's side of a session in the activity log: each state once.
    if (it == viewers_.end() || it->second.state != state)
        emit activity(QStringLiteral("Watching %1 (%2): %3%4").arg(authorize_(peer).name, camera_ ? "camera" : display_ ? "extended display" : "screen", state,
                                                                    detail.isEmpty() ? QString() : QStringLiteral(" - ") + detail));
    if (it != viewers_.end()) { it->second.state = state; update["path"] = it->second.path; }
    emit viewerChanged(hex(peer), update);
}

void ScreenService::showHosting() {
    QVariantList viewers;
    for (const auto& [peer, host] : hosts_) if (host.live)
        viewers.push_back(QVariantMap{{"devId", hex(peer)}, {"name", authorize_(peer).name}, {"control", host.control}});
    emit hostingChanged(viewers);
}

// Two devices may watch each other: each role ends on its own, and only a
// disconnect (stop) ends both.
void ScreenService::stop(const Peer& peer, const QString& reason) { endRoles(peer, reason, true, true); }
void ScreenService::stopHost(const Peer& peer, const QString& reason) { endRoles(peer, reason, true, false); }
void ScreenService::stopViewer(const Peer& peer, const QString& reason) { endRoles(peer, reason, false, true); }

void ScreenService::endRoles(const Peer& peer, const QString& reason, bool asHost, bool asViewer) {
    int channels = 0;
    const auto host = asHost ? hosts_.find(peer) : hosts_.end();
    if (host != hosts_.end()) {
        if (host->second.control) input_.release();
        if (host->second.controlRequest) emit consentClosed(hex(peer), host->second.controlRequest);
        if (host->second.live) ++channels;
        if (host->second.live) send_(peer, message("scr.stop", host->second.token));
        else fail(peer, host->second.request, "ended");
        emit consentClosed(hex(peer), host->second.request);
        hosts_.erase(host); showHosting();
        if (std::none_of(hosts_.begin(), hosts_.end(), [](const auto& entry) { return entry.second.live; })) {
            resetNative(); lastPipeline_.clear(); updateQtCapture(); emit captureChanged(false, settings_.monitor);
        }
        emit activity(QStringLiteral("%1 stopped: %2").arg(display_ ? "Extended display" : "Screen sharing", reason));
        emit hostEnded(hex(peer), reason);
    }
    const auto viewer = asViewer ? viewers_.find(peer) : viewers_.end();
    if (viewer != viewers_.end()) {
        if (viewer->second.channel) ++channels;
        auto msg = message("scr.stop", viewer->second.token); msg["request"] = double(viewer->second.request);
        send_(peer, msg); viewers_.erase(viewer);
        QMetaObject::invokeMethod(decoderWorker_, [this, peer] { decoders_.erase(peer); });
        showViewer(peer, "ended", reason);
    }
    while (channels-- > 0) close_(peer);
    if (hosts_.empty() && viewers_.empty() && !input_.pressedCount()) timer_.stop();
}
void ScreenService::captureFailed(const QString& error) {
    if (qtWanted_) stopHosting(error);  // otherwise it was stopped for the native path, or nothing is shared
}

void ScreenService::resetNative() {
    // An extended display is captured natively only: Qt's capture is not
    // set up for the virtual monitor.
    ++generation_; nativeActive_ = nativeWorked_ = false; nativeDisabled_ = camera_ || (!display_ && !settings_.gpuCapture);
    nativeRetryAt_ = nativeAttemptAt_ = 0; nativeFailures_ = 0;
    QMetaObject::invokeMethod(encoderWorker_, [this] { encoder_->reset(); desktop_.reset(); });
}

void ScreenService::updateQtCapture() {
    const bool watched = std::any_of(hosts_.begin(), hosts_.end(), [](const auto& entry) { return entry.second.live; });
    const bool wanted = watched && nativeDisabled_ && !display_;
    if (wanted != qtWanted_) { qtWanted_ = wanted; emit qtCaptureWanted(wanted); }
}

void ScreenService::stopHosting(const QString& reason) {
    while (!hosts_.empty()) stopHost(hosts_.begin()->first, reason);
}
void ScreenService::peerGone(const Peer& peer) { stop(peer, "device disconnected"); }
void ScreenService::reset() {
    ++generation_; ++decoderGeneration_;
    while (!hosts_.empty()) stopHost(hosts_.begin()->first);
    while (!viewers_.empty()) stopViewer(viewers_.begin()->first);
}
void ScreenService::policyChanged() {
    QVector<Peer> revokedHosts, revokedViewers;
    for (const auto& [peer, host] : hosts_) if (!allowed(peer)) revokedHosts.push_back(peer);
    for (const auto& [peer, viewer] : viewers_) if (!viewerAllowed(peer)) revokedViewers.push_back(peer);
    for (const auto& peer : revokedHosts) stopHost(peer, "permission withdrawn");
    for (const auto& peer : revokedViewers) stopViewer(peer, "permission withdrawn");
    QVector<Peer> controlRevoked;
    for (const auto& [peer, host] : hosts_) if (host.control && !controlAllowed(peer)) controlRevoked.push_back(peer);
    for (const auto& peer : controlRevoked) setControl(peer, false, 0, "control permission withdrawn");
}

bool ScreenService::handle(const Peer& peer, const QJsonObject& wire) {
    QJsonObject msg = wire;
    if (camera_ || display_) {
        auto type = msg.value("t").toString(); if (!type.startsWith(camera_ ? "cam." : "dsp.")) return false;
        type.replace(0, 4, "scr."); msg["t"] = type;
    }
    const auto type = msg.value("t").toString();
    if (!type.startsWith("scr.") && !type.startsWith("in.")) return false;
    // An extended display's handshake is DisplayService's, and it carries no
    // input or source switching.
    if (display_ && (type == "scr.request" || type == "scr.wait" || type == "scr.err" || type == "scr.accept" || type.startsWith("in.")))
        return false;
    if (display_ && (type == "scr.monitor" || type == "scr.control")) return true;
    const double requestValue = msg.value("request").toDouble();
    const quint32 requestId = requestValue >= 1 && requestValue <= 4294967295. && std::floor(requestValue) == requestValue
        ? quint32(requestValue) : 0;
    if (type == "scr.request") {
        if (!requestId || !allowed(peer)) { fail(peer, requestId, "forbidden"); return true; }
        if (!msg.value("codecs").toArray().contains("h264")) { fail(peer, requestId, "codec"); return true; }
        bool wantControl = msg.value("wantControl").toBool();
        if (wantControl && camera_) { fail(peer, requestId, "control forbidden"); return true; }
        // Control that cannot be granted (no permission, or no way to inject
        // input here, e.g. Wayland) leaves a view: the viewer is told why.
        QString controlRefused;
        if (wantControl && !controlAllowed(peer)) controlRefused = QStringLiteral("This device is not allowed to control the screen.");
        else if (wantControl) controlRefused = input_.available();
        if (!controlRefused.isEmpty()) wantControl = false;
        if (hosts_.contains(peer) || int(hosts_.size()) >= settings_.maxViewers) { fail(peer, requestId, "busy"); return true; }
        if (wantControl && std::any_of(hosts_.begin(), hosts_.end(), [](const auto& entry) { return entry.second.control || (!entry.second.live && entry.second.wantControl) || entry.second.controlRequest; })) { fail(peer, requestId, "control busy"); return true; }
        Host host; host.request = requestId; host.expires = now_() + 30000;
        host.wantControl = wantControl; host.controlRefused = controlRefused;
        host.width = std::min(settings_.maxWidth, std::clamp(msg.value("maxWidth").toInt(1920), 320, 3840));
        host.fps = std::min(settings_.maxFps, std::clamp(msg.value("maxFps").toInt(30), 1, MaxFps));
        hosts_.emplace(peer, std::move(host)); timer_.start();
        if (!camera_ && authorize_(peer).permissions.contains(perm::ScreenDirect) &&
            (!wantControl || authorize_(peer).permissions.contains(perm::ControlDirect))) startHost(peer);
        else {
            send_(peer, {{"t", "scr.wait"}, {"request", double(requestId)}});
            const auto access = authorize_(peer);
            emit consentRequested(hex(peer), requestId, {{"name", access.name}, {"fingerprint", access.fingerprint},
                {"control", wantControl}, {"camera", camera_}});
        }
        return true;
    }
    auto viewer = viewers_.find(peer);
    if (type == "scr.wait" || type == "scr.err" || type == "scr.started") {
        if (viewer == viewers_.end() || requestId != viewer->second.request || viewer->second.token) return true;
        if (type == "scr.wait") showViewer(peer, "waiting", "Waiting for the host to allow screen sharing.");
        else if (type == "scr.err") {
            const auto code = msg.value("code").toString();
            viewers_.erase(viewer);
            const QString detail = code == "forbidden"
                ? (camera_ ? QStringLiteral("The other device has to trust this one and allow it to view its camera.")
                           : QStringLiteral("The other device has to trust this one and allow it to view its screen."))
                : code;
            showViewer(peer, code == "busy" ? "busy" : "denied", detail);
        } else {
            const int token = msg.value("session").toInt();
            if (token < 1 || token > 65535 || msg.value("codec") != "h264" || !viewerAllowed(peer)) return true;
            viewer->second.token = quint16(token); viewer->second.expires = now_() + 10000;
            viewer->second.liveAt = now_();
            viewer->second.fps = std::clamp(msg.value("fps").toInt(30), 1, MaxFps);
            viewer->second.receiver.setPatient(display_);
            viewer->second.receiver.reset(quint16(token), viewer->second.fps);
            viewer->second.control = viewer->second.wantControl && !camera_ && msg.value("control").toBool();
            if (!open_(peer)) { stopViewer(peer, "transport unavailable"); return true; }
            viewer->second.channel = true;
            showViewer(peer, "live", msg.value("controlDetail").toString().left(300), {{"monitors", msg.value("monitors").toArray().toVariantList()},
                {"monitor", msg.value("monitor").toInt()}, {"control", viewer->second.control},
                {"pipeline", msg.value("pipeline").toString()},
                {"cursorEmbedded", msg.value("cursorEmbedded").toBool(true)}});
        }
        return true;
    }
    const int token = msg.value("session").toInt();
    auto host = hosts_.find(peer);
    const bool forHost = host != hosts_.end() && host->second.live && token == host->second.token;
    const bool forViewer = viewer != viewers_.end() && viewer->second.token && token == viewer->second.token;
    if (type.startsWith("in.")) {
        if (forHost && host->second.control && allowed(peer) && controlAllowed(peer)) {
            if (!input_.available().isEmpty() || !input_.apply(msg)) setControl(peer, false, 0, "Input could not be delivered; check host accessibility and keyboard layout.");
        }
        return true;
    }
    if (type == "scr.stop") {
        // A viewer's stop names its request; a host's does not. That tells
        // the two sessions of devices watching each other apart.
        const bool fromViewer = host != hosts_.end() && requestId && requestId == host->second.request;
        const bool fromHost = forViewer && !requestId;
        if (fromViewer && (forHost || !host->second.live)) stopHost(peer, "stopped by peer");
        if (fromHost) stopViewer(peer, "stopped by peer");
    } else if (forViewer && type == "scr.error") {
        stopViewer(peer, msg.value("detail").toString("media error"));
    } else if (forViewer && type == "scr.controlled") {
        if (requestId && requestId != viewer->second.controlRequest) return true;
        if (msg.value("on").toBool() && (!requestId || !viewer->second.wantControl || camera_)) return true;
        viewer->second.control = msg.value("on").toBool();
        if (!viewer->second.control) viewer->second.wantControl = false;
        viewer->second.controlRequest = 0;
        showViewer(peer, "live", msg.value("detail").toString(), {{"control", viewer->second.control}, {"controlPending", false}});
    } else if (forViewer && type == "scr.status") {
        showViewer(peer, "live", {}, {{"hostStatus", msg.value("detail").toString().left(300)}});
    } else if (forViewer && type == "scr.pipeline") {
        QVariantMap info{{"pipeline", msg.value("name").toString().left(128)}};
        if (msg.value("cursorEmbedded").isBool()) info["cursorEmbedded"] = msg.value("cursorEmbedded").toBool();
        showViewer(peer, "live", {}, info);
    } else if (forViewer && !camera_ && type == "scr.cursor") {
        const QByteArray base64 = msg.value("png").toString().toLatin1();
        if (base64.size() > 65536) return true;
        QByteArray png = QByteArray::fromBase64(base64, QByteArray::AbortOnBase64DecodingErrors);
        QBuffer buffer(&png); buffer.open(QIODevice::ReadOnly); QImageReader reader(&buffer, "PNG");
        const auto size = reader.size();
        const int hx = msg.value("hotspotX").toInt(-1), hy = msg.value("hotspotY").toInt(-1);
        if (size.width() < 1 || size.height() < 1 || size.width() > 128 || size.height() > 128 ||
            hx < 0 || hy < 0 || hx >= size.width() || hy >= size.height() || reader.read().isNull()) return true;
        showViewer(peer, "live", {}, {{"cursorImage", "data:image/png;base64," + QString::fromLatin1(base64)},
            {"cursorHotspotX", hx}, {"cursorHotspotY", hy}});
    } else if (forHost) {
        if (!allowed(peer)) { stopHost(peer, "permission withdrawn"); return true; }
        if (type == "scr.control") {
            if (msg.value("on").toBool()) requestHostControl(peer, requestId);
            else setControl(peer, false, requestId);
        } else if (type == "scr.report") {
            if (const auto report = Feedback::parse(msg)) {
                if (now_() - host->second.lastReport < 100) return true;
                host->second.lastReport = now_(); host->second.congestion.update(*report);
                host->second.reportLoss = double(report->lost) / std::max(1., double(report->received + report->lost));
                host->second.reportTrendMs = report->owdTrendMs;
            }
        } else if (type == "scr.keyframe") host->second.forceKey = true;
        else if (type == "scr.nack") {
            const double frameValue = msg.value("frame").toDouble(-1);
            if (frameValue < 0 || frameValue > 4294967295. || std::floor(frameValue) != frameValue) return true;
            const auto frame = quint32(frameValue);
            // An extended display may ask again (up to three times) for a frame
            // whose resent packets were lost too; otherwise once per frame.
            if (host->second.nackedFrame == frame && ++host->second.nackRepeats >= (display_ ? 3 : 1)) return true;
            if (host->second.nackedFrame != frame) host->second.nackRepeats = 0;
            host->second.nackedFrame = frame;
            const auto array = msg.value("indexes").toArray();
            if (array.size() > MaxFragments) return true;
            QVector<int> indexes; for (const auto& index : array) indexes.push_back(index.toInt(-1));
            for (const auto& packet : host->second.sender.retransmit(frame, indexes, now_()))
                if (!datagram_(peer, packet)) break;
        } else if (type == "scr.monitor") {
            const int index = msg.value("index").toInt(-1);
            if (index >= 0 && index < settings_.monitors.size()) {
                const bool anotherController = std::any_of(hosts_.begin(), hosts_.end(), [&](const auto& entry) { return entry.first != peer && entry.second.control; });
                if (anotherController) return true;
                input_.release(); resetNative(); updateQtCapture();
                const auto bounds = settings_.monitors[index].toMap();
                input_.setBounds(QRect(bounds.value("inputX", bounds.value("x")).toInt(), bounds.value("inputY", bounds.value("y")).toInt(),
                    bounds.value("inputWidth", bounds.value("width")).toInt(), bounds.value("inputHeight", bounds.value("height")).toInt()));
                for (auto& [other, existing] : hosts_) if (existing.live) {
                    existing.monitor = index; existing.forceKey = true; existing.sender.reset();
                    auto changed = message("scr.monitorChanged", existing.token); changed["index"] = index; send_(other, changed);
                }
                emit captureChanged(true, index);
            }
        }
    }
    if (forViewer && type == "scr.monitorChanged") showViewer(peer, "live", {}, {{"monitor", msg.value("index").toInt()}});
    return true;
}

void ScreenService::receive(const Peer& peer, const QByteArray& wire) {
    QByteArray packet = wire;
    if (packet.isEmpty()) return;
    // Back to the screen kinds: 1 video, 2 cursor.
    if (camera_) { if (packet[0] != 3) return; packet[0] = 1; }
    else if (display_) { if (packet[0] != 4 && packet[0] != 5) return; packet[0] = char(packet[0] - 3); }
    else if (packet[0] != 1 && packet[0] != 2) return;
    const auto it = viewers_.find(peer);
    if (it == viewers_.end() || !it->second.token || !viewerAllowed(peer)) return;
    auto& viewer = it->second;
    if (!packet.isEmpty() && packet[0] == 2) {
        const auto cursor = parseCursor(packet);
        if (!cursor || cursor->session != viewer.token || (viewer.haveCursor && !cursorNewer(cursor->sequence, viewer.cursorSequence))) return;
        viewer.cursorSequence = cursor->sequence; viewer.haveCursor = true;
        showViewer(peer, "live", {}, {{"cursorX", cursor->point.x()}, {"cursorY", cursor->point.y()}, {"cursorVisible", cursor->visible}});
        return;
    }
    auto complete = viewer.receiver.receive(packet, now_()); viewer.bytes += packet.size();
    if (complete.isEmpty()) return;
    // Frames that complete while the decoder is busy wait for it: each one is
    // a reference for the next, so none may be skipped. Only a decoder far
    // behind (a second of video) starts over from a keyframe.
    viewer.toDecode += complete; stats_.completed += complete.size();
    if (viewer.toDecode.size() > std::max(8, viewer.fps)) {
        viewer.toDecode.clear();
        viewer.receiver.reset(viewer.token, viewer.fps);
        send_(peer, message("scr.keyframe", viewer.token));
        return;
    }
    if (!viewer.decoding) decodeNext(peer);
}

void ScreenService::decodeNext(const Peer& peer) {
    const auto it = viewers_.find(peer);
    if (it == viewers_.end() || it->second.toDecode.isEmpty()) return;
    auto& viewer = it->second;
    viewer.decoding = true; ++stats_.decodes;
    auto complete = std::move(viewer.toDecode);
    viewer.toDecode.clear();
    const auto token = viewer.token; const auto generation = decoderGeneration_; const auto started = monotonicMs();
    const bool everyFrame = display_;
    QMetaObject::invokeMethod(decoderWorker_, [this, peer, token, generation, complete = std::move(complete), started, everyFrame] {
        auto& decoder = decoders_[peer]; if (!decoder) decoder = std::make_unique<Decoder>();
        // Screen sharing shows only the last frame of a batch; the others just
        // advance the decoder. An extended display paces every frame out.
        QVector<QVideoFrame> images; bool any = false;
        for (qsizetype i = 0; i < complete.size(); ++i) {
            bool decoded = false;
            auto picture = decoder->decodeFrame(complete[i], everyFrame || i == complete.size() - 1, &decoded);
            any |= decoded;
            if (picture.isValid()) { if (!everyFrame) images.clear(); images.push_back(std::move(picture)); }
        }
        // Per frame: a batch taking longer is not a slower decoder.
        const double elapsed = double(monotonicMs() - started) / std::max<qsizetype>(1, complete.size());
        QMetaObject::invokeMethod(this, [this, peer, token, generation, images, any, elapsed] {
            const auto it = viewers_.find(peer);
            if (generation != decoderGeneration_ || it == viewers_.end() || it->second.token != token) return;
            it->second.decoding = false; it->second.decodeMs = elapsed; stats_.decodeMs += elapsed;
            if (!any) {
                it->second.toDecode.clear();
                it->second.receiver.reset(token, it->second.fps);
                send_(peer, message("scr.keyframe", token));
            } else if (!images.isEmpty()) {
                it->second.decoded += int(images.size());
                if (!it->second.pictured) {
                    it->second.pictured = true;
                    showViewer(peer, "live", {}, {{"pictured", true}});
                }
                if (display_) {
                    // At most 3 waiting: older ones are too late to be worth showing.
                    for (const auto& image : images) it->second.present.push_back(image);
                    while (it->second.present.size() > 3) it->second.present.pop_front();
                    presentNext(peer, it->second, now_());
                } else {
                    emit frameReady(hex(peer), images.back()); ++stats_.shown;
                }
            }
            decodeNext(peer);
        });
    });
}

// Shows the next waiting picture once its time has come: one per frame
// interval, a little faster while more are waiting so latency does not build.
void ScreenService::presentNext(const Peer& peer, Viewer& viewer, qint64 now) {
    if (viewer.present.empty()) return;
    const double interval = 1000. / std::max(1, viewer.fps);
    if (now < viewer.presentAt) return;
    emit frameReady(hex(peer), viewer.present.front()); ++stats_.shown;
    viewer.present.pop_front();
    const double next = viewer.present.size() > 1 ? interval * .75 : interval;
    // After a pause, the cadence starts again from now.
    viewer.presentAt = now - viewer.presentAt > qint64(interval) ? now + qint64(next) : viewer.presentAt + qint64(next);
}

void ScreenService::pathChanged(const Peer& peer, const QString& path) {
    if (auto it = hosts_.find(peer); it != hosts_.end()) {
        it->second.path = path;
        it->second.congestion.reset(path, it->second.fps, it->second.width, settings_.maxBitrate, display_ ? DisplayStartBitrate : 0);
        // An extended display needs the devices near each other: through the
        // relay or over TCP it would lag too far behind the mouse.
        if (display_ && it->second.live && path != "direct" && path != "opening") {
            failMedia(peer, QStringLiteral("An extended display needs both devices on the same network (the connection is %1).").arg(path));
            return;
        }
    }
    if (auto it = viewers_.find(peer); it != viewers_.end()) {
        it->second.path = path; showViewer(peer, it->second.state);
    }
}

void ScreenService::submitCapture(const QVideoFrame& frame) {
    ++stats_.offered;
    if (capturePending_.exchange(true)) { ++stats_.busy; return; }
    const auto captured = quint32(monotonicMs());
    QMetaObject::invokeMethod(this, [this, frame, captured] { encodeCapture(frame, {}, captured); });
}
void ScreenService::submitImage(const QImage& image) {
    if (capturePending_.exchange(true)) return;
    encodeCapture({}, image, quint32(now_()));
}
void ScreenService::encodeCapture(const QVideoFrame& video, const QImage& image, quint32 captureMs) {
    int width = settings_.maxWidth, fps = settings_.maxFps, bitrate = settings_.maxBitrate;
    bool key = false; std::map<Peer, quint16> targets;
    for (auto& [peer, host] : hosts_) if (host.live) {
        targets[peer] = host.token; key |= host.forceKey;
        width = std::min(width, host.congestion.width()); fps = std::min(fps, host.congestion.fps());
        bitrate = std::min(bitrate, host.congestion.bitrate());
    }
    // Frames are due every 1000/fps ms, kept fractional: whole milliseconds
    // would drift at high rates (165 fps is 6.06 ms). Late frames do not
    // bunch up to catch up.
    const double now = double(now_());
    if (targets.empty() || now < nextCapture_ - .5) { if (!targets.empty()) ++stats_.throttled; capturePending_ = false; return; }
    nextCapture_ = std::max(nextCapture_ + 1000. / fps, now); ++stats_.encodes;
    for (auto& [peer, host] : hosts_) if (host.live) host.forceKey = false;
    const auto number = nextFrame_; const auto generation = generation_;
    QString nativeMonitor;
    for (const auto& [peer, host] : hosts_) if (host.live) {
        const auto info = settings_.monitors.value(host.monitor).toMap();
        nativeMonitor = info.value("nativeId", info.value("name")).toString(); break;
    }
    const bool native = !camera_ && settings_.gpuCapture && !nativeDisabled_ && image.isNull();
    CodecOptions options{width, fps, bitrate, {}, settings_.ciscoLibrary};
    const bool wasNative = nativeActive_;
    QMetaObject::invokeMethod(encoderWorker_, [this, targets, generation, number, key, video, image, captureMs, options, native, nativeMonitor, wasNative] {
        const auto encodeStart = monotonicMs();
        DesktopResult gpu;
        if (native) {
            if (!desktop_) desktop_ = desktopEncoder();
            if (desktop_) gpu = desktop_->encode(nativeMonitor, options, number, captureMs, key);
            if (!gpu.active && wasNative) encoder_->reset();  // a retry failing leaves the Qt path alone
        }
        // A native attempt started without a captured frame has nothing else
        // to encode when it fails: Qt's capture takes over from its next frame.
        const bool sourceless = !gpu.active && image.isNull() && !video.isValid();
        const auto result = gpu.active || sourceless ? gpu.frame
            : image.isNull() ? encoder_->encode(video, options, 1, number, captureMs, key)
                             : encoder_->encode(image, options, 1, number, captureMs, key);
        const auto error = gpu.active || sourceless ? QString() : encoder_->error();
        const auto name = gpu.active ? gpu.pipeline : sourceless ? QString() : encoder_->name();
        const auto nativeError = gpu.detail;
        const double encodeMs = monotonicMs() - encodeStart;
        QMetaObject::invokeMethod(this, [this, targets, generation, result, error, name, fps = options.fps, native, gpu, nativeError, sourceless, encodeMs] {
            capturePending_ = false;
            stats_.encodeMs += encodeMs; if (result) ++stats_.frames;
            if (generation != generation_) return;
            if (native) {
                if (gpu.active) {
                    nativeFailures_ = 0; nativeRetryAt_ = 0; nativeWorked_ = true;
                } else {
                    if (!nativeFailures_ && !nativeError.isEmpty()) emit activity(nativeError);
                    // Only a path that worked is tried again (it was lost to a display
                    // change, a UAC prompt, the lock screen): each try pauses Qt's capture.
                    // An extended display has no other way to be captured, and a
                    // monitor that just arrived may take a moment to duplicate.
                    nativeRetryAt_ = display_ ? now_() + 500
                        : nativeWorked_ ? now_() + std::min<qint64>(60000, qint64(2000) << std::min(nativeFailures_, 5)) : 0;
                    ++nativeFailures_;
                }
                nativeDisabled_ = !gpu.active; nativeActive_ = gpu.active;
                updateQtCapture();
            }
            // No frame from the software encoder means one was lost. The native
            // path also returns none when the screen has not changed yet, which
            // needs no keyframe.
            if (!result && error.isEmpty() && !gpu.active && !sourceless)
                for (auto& [peer, host] : hosts_) if (host.live) host.forceKey = true;
            if (result) ++nextFrame_;
            for (const auto& [peer, token] : targets) {
                const auto it = hosts_.find(peer);
                if (it == hosts_.end() || it->second.token != token) continue;
                if (result) {
                    auto frame = *result; frame.session = token;
                    // Extended displays run on a direct path with room to spare: a
                    // shorter spread costs less latency.
                    const double spread = display_ && it->second.path == "direct" ? 0.25 : 0.5;
                    const int paceBps = display_ ? 3 * it->second.congestion.bitrate() : 0;
                    if (!it->second.sender.enqueue(frame, now_(), 1000 / fps, spread, paceBps)) { it->second.forceKey = true; ++stats_.refused; }
                    else { ++it->second.framesSent; ++stats_.queued; }
                } else if (!error.isEmpty()) {
                    auto failure = message("scr.error", token); failure["detail"] = error;
                    send_(peer, failure); stopHost(peer, error);
                }
            }
            if (!name.isEmpty() && name != lastPipeline_) {
                lastPipeline_ = name; emit activity(QStringLiteral("Media pipeline: %1").arg(name));
                for (const auto& [peer, host] : hosts_) if (host.live) {
                    auto info = message("scr.pipeline", host.token); info["name"] = name;
                    info["cursorEmbedded"] = nativeActive_ ? false : settings_.cursorEmbedded;
                    send_(peer, info);
                }
            }
        });
    });
}

void ScreenService::logStats(qint64 now) {
    const qint64 span = std::max<qint64>(1, now - stats_.at);
    const auto rate = [&](double n) { return QString::number(n * 1000. / span, 'f', 1); };
    QString line = QStringLiteral("[stats%1]").arg(camera_ ? " camera" : display_ ? " display" : "");
    for (const auto& [peer, host] : hosts_) if (host.live) {
        line += QStringLiteral(" host: offered %1/s (busy %2/s), throttled %3/s, encodes %4/s at %5 ms, frames %6/s, queued %7/s, refused %8/s,"
                               " sender drops %9 | target %10 fps %11 kbit/s %12 px | loss %13% trend %14 ms | %15")
            .arg(rate(stats_.offered), rate(stats_.busy), rate(stats_.throttled), rate(stats_.encodes))
            .arg(stats_.encodes ? stats_.encodeMs / stats_.encodes : 0., 0, 'f', 1)
            .arg(rate(stats_.frames), rate(stats_.queued), rate(stats_.refused))
            .arg(host.sender.dropped()).arg(host.congestion.fps()).arg(host.congestion.bitrate() / 1000).arg(host.congestion.width())
            .arg(host.reportLoss * 100, 0, 'f', 1).arg(host.reportTrendMs, 0, 'f', 1).arg(lastPipeline_);
        break;
    }
    if (!viewers_.empty())
        line += QStringLiteral(" viewer: complete %1/s, decodes %2/s at %3 ms/frame, shown %4/s")
            .arg(rate(stats_.completed), rate(stats_.decodes))
            .arg(stats_.decodes ? stats_.decodeMs / stats_.decodes : 0., 0, 'f', 1).arg(rate(stats_.shown));
    if (!hosts_.empty() || !viewers_.empty()) qInfo().noquote() << line;
    stats_.offered = 0; stats_.busy = 0;
    stats_.throttled = stats_.encodes = stats_.frames = stats_.queued = stats_.refused = 0;
    stats_.completed = stats_.decodes = stats_.shown = 0; stats_.encodeMs = stats_.decodeMs = 0; stats_.at = now;
}

void ScreenService::tick() {
    const auto now = now_(); QVector<Peer> expired, expiredViewers;
    if (statsOn_ && now - stats_.at >= 1000) logStats(now);
    // The native path (desktop duplication) captures by itself. Try it as
    // soon as someone watches instead of waiting for Qt's capture to deliver
    // a first frame, which on some systems never comes.
    const bool watched = std::any_of(hosts_.begin(), hosts_.end(), [](const auto& entry) { return entry.second.live; });
    if (nativeDisabled_ && nativeRetryAt_ && now >= nativeRetryAt_ && settings_.gpuCapture && !camera_) {
        nativeDisabled_ = false; nativeRetryAt_ = 0; nativeAttemptAt_ = now + 500; updateQtCapture();
    }
    if (watched && !camera_ && settings_.gpuCapture && !nativeDisabled_ && now >= nativeAttemptAt_ && !capturePending_.exchange(true))
        encodeCapture({}, {}, quint32(now));
    const bool controlled = std::any_of(hosts_.begin(), hosts_.end(), [](const auto& entry) { return entry.second.control; });
    if (!controlled && input_.pressedCount() && now - lastReleaseRetry_ >= 100) {
        lastReleaseRetry_ = now; input_.release();
    }
    QVector<std::pair<Peer, QString>> displayFailures;
    for (auto& [peer, host] : hosts_) {
        if (display_ && host.live && host.path == "opening" && now - host.startedAt > 5000)
            displayFailures.push_back({peer, QStringLiteral("No direct connection to the other device: an extended display needs both devices on the same network.")});
        else if (display_ && host.live && !host.framesSent && now - host.startedAt > 15000)
            displayFailures.push_back({peer, QStringLiteral("The extended display could not be captured.")});
        if (!host.live && now >= host.expires) expired.push_back(peer);
        else if (host.live) {
            if (host.controlRequest && now >= host.controlExpires) setControl(peer, false, host.controlRequest, "control request timed out");
            if (host.control && !controlAllowed(peer)) setControl(peer, false, 0, "control permission withdrawn");
            if (now - host.lastReport > 5000 || !allowed(peer)) expired.push_back(peer);
            else if (!host.sender.pump(now, [&](const QByteArray& packet) { return datagram_(peer, packet); })) host.forceKey = true;
            if (!host.framesSent && !host.stalledTold && now - host.startedAt > 5000) {
                host.stalledTold = true;
                const QString source = camera_ ? QStringLiteral("camera") : QStringLiteral("screen");
                const QString detail = lastPipeline_.isEmpty()
                    ? QStringLiteral("The %1 has not produced a picture yet: no capture method has delivered a frame.").arg(source)
                    : QStringLiteral("The %1 has not produced a picture yet (%2).").arg(source, lastPipeline_);
                emit activity(detail);
                auto status = message("scr.status", host.token); status["detail"] = detail; send_(peer, status);
            }
        }
    }
    for (auto& [peer, viewer] : viewers_) {
        if (!viewer.present.empty()) presentNext(peer, viewer, now);
        if (!viewer.token) { if (now >= viewer.expires) expiredViewers.push_back(peer); continue; }
        if (!viewer.pictured && !viewer.stalledTold && now - viewer.liveAt > 8000) {
            viewer.stalledTold = true;
            showViewer(peer, "live", {}, {{"stalled", true}});
        }
        const auto recovery = viewer.receiver.expire(now);
        if (recovery.keyframe) send_(peer, message("scr.keyframe", viewer.token));
        for (const auto& nack : recovery.nacks) {
            auto msg = message("scr.nack", viewer.token); msg["frame"] = double(nack.frame);
            QJsonArray indexes; for (int index : nack.indexes) indexes.push_back(index);
            msg["indexes"] = indexes; send_(peer, msg);
        }
        if (now - viewer.reportAt >= 300) {
            const auto report = viewer.receiver.report(viewer.decodeMs); auto msg = report.json();
            msg["t"] = "scr.report"; msg["session"] = viewer.token; send_(peer, msg);
            showViewer(peer, "live", {}, {{"fps", viewer.decoded * 1000. / (now - viewer.reportAt)},
                {"bitrate", double(viewer.bytes) * 8000. / (now - viewer.reportAt)}, {"decodeMs", viewer.decodeMs},
                {"loss", double(report.lost) / std::max(1., double(report.received + report.lost))},
                {"jitterMs", report.jitterMs}});
            viewer.bytes = 0; viewer.decoded = 0; viewer.reportAt = now;
        }
    }
    for (const auto& [peer, detail] : displayFailures) { failMedia(peer, detail); expired.removeAll(peer); }
    for (const auto& peer : expired) stopHost(peer, "request or feedback timed out");
    for (const auto& peer : expiredViewers) stopViewer(peer, "request timed out");
    if (hosts_.empty() && viewers_.empty() && !input_.pressedCount()) timer_.stop();
}
void ScreenService::monitor(const Peer& peer, int index) {
    if (const auto it = viewers_.find(peer); it != viewers_.end() && it->second.token) {
        auto msg = message("scr.monitor", it->second.token); msg["index"] = index; send_(peer, msg);
    }
}

void ScreenService::setControl(const Peer& peer, bool on, quint32 request, const QString& detail) {
    const auto it = hosts_.find(peer); if (it == hosts_.end() || !it->second.live) return;
    auto& host = it->second;
    if (host.controlRequest) emit consentClosed(hex(peer), host.controlRequest);
    host.controlRequest = 0;
    const bool accepted = on && controlAllowed(peer) && input_.available().isEmpty();
    if (!accepted && host.control) input_.release();
    host.control = accepted;
    auto msg = message("scr.controlled", host.token); msg["request"] = double(request); msg["on"] = accepted;
    msg["detail"] = on && !input_.available().isEmpty() ? input_.available() : detail;
    send_(peer, msg); showHosting();
    emit activity(QStringLiteral("Remote control %1 for %2").arg(accepted ? "started" : "stopped", authorize_(peer).name));
}
void ScreenService::requestHostControl(const Peer& peer, quint32 request) {
    auto& host = hosts_.at(peer);
    if (!request || host.controlRequest) return;
    for (const auto& [other, existing] : hosts_) if (other != peer && (existing.control || existing.controlRequest || (!existing.live && existing.wantControl))) {
        setControl(peer, false, request, "Another viewer already has control."); return;
    }
    if (!controlAllowed(peer) || !input_.available().isEmpty()) {
        setControl(peer, false, request, input_.available().isEmpty() ? QString("control forbidden") : input_.available()); return;
    }
    if (authorize_(peer).permissions.contains(perm::ControlDirect)) { setControl(peer, true, request); return; }
    host.controlRequest = request; host.controlExpires = now_() + 30000;
    const auto access = authorize_(peer);
    emit consentRequested(hex(peer), request, {{"name", access.name}, {"fingerprint", access.fingerprint}, {"control", true}, {"controlOnly", true}});
}
void ScreenService::requestControl(const Peer& peer, bool on) {
    if (camera_ || display_) return;
    const auto it = viewers_.find(peer); if (it == viewers_.end() || !it->second.token) return;
    auto& viewer = it->second;
    if (viewer.controlRequest && on) return;
    if (!on) send_(peer, message("in.release", viewer.token));
    viewer.wantControl = on;
    viewer.controlRequest = nextRequest_++; if (!nextRequest_) ++nextRequest_;
    showViewer(peer, "live", {}, {{"controlPending", on}});
    auto msg = message("scr.control", viewer.token); msg["on"] = on; msg["request"] = double(viewer.controlRequest); send_(peer, msg);
}
void ScreenService::stopControl() {
    for (const auto& [peer, host] : hosts_) if (host.control || host.controlRequest) setControl(peer, false);
}
void ScreenService::sendInput(const Peer& peer, QJsonObject msg) {
    if (camera_ || display_) return;
    const auto it = viewers_.find(peer);
    if (it == viewers_.end() || !it->second.token || !it->second.control || !authorize_(peer).verified) return;
    if (!msg.value("t").toString().startsWith("in.")) return;
    msg["session"] = it->second.token;
    if (!send_(peer, msg)) stopViewer(peer, "input connection unavailable");
}

void ScreenService::submitCursor(const QPoint& position, bool visible, const QByteArray& shape, const QPoint& hotspot) {
    if (!shape.isEmpty() && shape.size() <= 49152 && shape != cursorShape_) {
        cursorShape_ = shape; cursorHotspot_ = hotspot; ++cursorVersion_;
    }
    if (camera_) return;
    ++cursorSequence_;
    for (auto& [peer, host] : hosts_) {
        if (!host.live) continue;
        if (host.cursorVersion != cursorVersion_ && !cursorShape_.isEmpty()) {
            auto msg = message("scr.cursor", host.token); msg["png"] = QString::fromLatin1(cursorShape_.toBase64());
            msg["hotspotX"] = cursorHotspot_.x(); msg["hotspotY"] = cursorHotspot_.y();
            if (send_(peer, msg)) host.cursorVersion = cursorVersion_;
        }
        const auto monitor = settings_.monitors.value(host.monitor).toMap();
        const QRect bounds(monitor.value("inputX", monitor.value("x")).toInt(), monitor.value("inputY", monitor.value("y")).toInt(),
            monitor.value("inputWidth", monitor.value("width")).toInt(), monitor.value("inputHeight", monitor.value("height")).toInt());
        if (bounds.isEmpty()) continue;
        const QPoint local = position - bounds.topLeft();
        const QPoint pixels(qint64(local.x()) * monitor.value("width").toInt() / bounds.width(),
            qint64(local.y()) * monitor.value("height").toInt() / bounds.height());
        datagram_(peer, cursorPacket({host.token, cursorSequence_, pixels, visible && bounds.contains(position)}));
    }
}

}  // namespace ucd::screen
