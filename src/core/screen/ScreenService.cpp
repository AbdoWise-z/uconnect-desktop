#include "ScreenService.hpp"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QPointer>
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
    convertPool_.setObjectName("screen-convert");
    convertPool_.setMaxThreadCount(converterThreads());
}

int ScreenService::converterThreads() const {
    if (settings_.encoderThreads > 0) return std::clamp(settings_.encoderThreads, 1, 16);
    return std::clamp(QThread::idealThreadCount() / 2, 1, 4);
}

ScreenService::~ScreenService() {
    timer_.stop();
    convertPool_.waitForDone();
    QMetaObject::invokeMethod(encoderWorker_, [this] { encoder_.reset(); desktop_.reset(); }, Qt::BlockingQueuedConnection);
    QMetaObject::invokeMethod(decoderWorker_, [this] { decoders_.clear(); }, Qt::BlockingQueuedConnection);
    encoderThread_.quit(); decoderThread_.quit(); encoderThread_.wait(); decoderThread_.wait();
}

void ScreenService::configure(const ScreenSettings& settings) {
    const bool offered = settings_.offered;
    const bool controlOffered = settings_.controlOffered;
    const bool gpuChanged = settings_.gpuCapture != settings.gpuCapture;
    const QVariantList monitorsBefore = settings_.monitors;
    settings_ = settings;
    settings_.offered = offered;
    settings_.controlOffered = controlOffered;
    settings_.maxFps = std::clamp(settings.maxFps, 1, MaxFps);
    settings_.maxWidth = std::clamp(settings.maxWidth, 320, 3840);
    settings_.maxBitrate = std::clamp(settings.maxBitrate, 150000, CongestionController::MaxBitrate);
    settings_.maxViewers = std::clamp(settings.maxViewers, 1, 8);
    settings_.encoderThreads = std::clamp(settings.encoderThreads, 0, 16);
    settings_.queueFrames = std::clamp(settings.queueFrames, 1, 30);
    convertPool_.setMaxThreadCount(converterThreads());
    if (gpuChanged) {
        resetNative(); updateQtCapture();
        for (auto& [peer, host] : hosts_) host.forceKey = true;
    }
    settings_.monitor = std::clamp(settings.monitor, 0, std::max(0, int(settings.monitors.size()) - 1));
    followMonitors(monitorsBefore);
    while (int(hosts_.size()) > settings_.maxViewers) stopHost(hosts_.rbegin()->first, "viewer limit reduced");
    // An extended display is not an offered service: DisplayService starts it.
    if (!settings_.offered && !display_) stopHosting("screen service disabled");
}
void ScreenService::followMonitors(const QVariantList& before) {
    // A camera's list is its devices, and an extended display has one monitor.
    if (camera_ || display_ || before == settings_.monitors) return;
    const auto id = [](const QVariant& monitor) {
        const auto map = monitor.toMap();
        return map.value("nativeId", map.value("name")).toString();
    };
    std::vector<Peer> gone;
    int moved = -1;
    for (auto& [peer, host] : hosts_) {
        const QString was = id(before.value(host.monitor));
        int index = -1;
        for (int i = 0; i < settings_.monitors.size() && !was.isEmpty(); ++i)
            if (id(settings_.monitors[i]) == was) { index = i; break; }
        // Never another monitor in its place: that is not what was shared.
        if (index < 0) { gone.push_back(peer); continue; }
        if (index == host.monitor) continue;
        host.monitor = index;
        if (!host.live) continue;
        auto changed = message("scr.monitorChanged", host.token); changed["index"] = index; send_(peer, changed);
        moved = index;
    }
    for (const auto& peer : gone) stopHost(peer, "shared screen disconnected");
    // The desktop's layout may have moved the monitor too: input follows it.
    for (const auto& [peer, host] : hosts_) if (host.live) {
        const auto bounds = settings_.monitors.value(host.monitor).toMap();
        input_.setBounds(QRect(bounds.value("inputX", bounds.value("x")).toInt(), bounds.value("inputY", bounds.value("y")).toInt(),
            bounds.value("inputWidth", bounds.value("width")).toInt(), bounds.value("inputHeight", bounds.value("height")).toInt()));
        break;
    }
    if (moved >= 0) emit captureChanged(true, moved);
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

void ScreenService::request(const Peer& peer, bool control, int maxWidth, int fps, int maxBitrate, int bufferMs) {
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
    viewer.maxBitrate = maxBitrate > 0 ? std::clamp(maxBitrate, CongestionController::MinBitrate, CongestionController::MaxBitrate) : 0;
    viewer.bufferMs = std::clamp(bufferMs, 0, 2000);
    viewer.expires = now_() + 35000; viewer.reportAt = now_();
    viewers_.emplace(peer, std::move(viewer)); timer_.start();
    showViewer(peer, "requesting", {}, {{"pictured", false}, {"stalled", false}, {"hostStatus", QString()}, {"host", QVariantMap()}});
    QJsonObject ask{{"t", "scr.request"}, {"request", double(viewers_.at(peer).request)},
        {"wantControl", viewers_.at(peer).wantControl}, {"maxWidth", maxWidth}, {"maxFps", fps}, {"codecs", QJsonArray{"h264"}}};
    if (viewers_.at(peer).maxBitrate) ask["maxBitrate"] = viewers_.at(peer).maxBitrate;
    if (!send_(peer, ask)) stopViewer(peer, "connection unavailable");
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
    if (remember && camera_) {
        emit rememberPermission(authorize_(*peer).key, perm::CameraDirect);
    } else if (remember) {
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
    if (!host.maxBitrate) host.maxBitrate = settings_.maxBitrate;
    host.congestion.reset(host.path, host.fps, host.width, host.maxBitrate, display_ ? DisplayStartBitrate : 0);
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
    pipeline_.clear(); encoderInput_ = {};
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
        // The viewer may ask for less than this device allows, never more.
        const int asked = msg.value("maxBitrate").toInt(0);
        host.maxBitrate = asked > 0 ? std::min(settings_.maxBitrate, std::max(asked, CongestionController::MinBitrate)) : settings_.maxBitrate;
        hosts_.emplace(peer, std::move(host)); timer_.start();
        const auto& perms = authorize_(peer).permissions;
        const bool directOk = camera_
            ? perms.contains(perm::CameraDirect)
            : perms.contains(perm::ScreenDirect) && (!wantControl || perms.contains(perm::ControlDirect));
        if (directOk) startHost(peer);
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
                ? (camera_ ? QStringLiteral("The other device has to allow this one to view its camera, in its special or network rules.")
                           : QStringLiteral("The other device has to allow this one to view its screen, in its special or network rules."))
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
    } else if (forViewer && type == "scr.stats") {
        // How the sending side keeps up, for the viewer's statistics.
        QVariantMap host;
        for (const char* key : {"fps", "encodeMs", "convertMs", "queue", "queuePeak", "dropped", "throttled", "sentKbps",
                                "targetFps", "targetKbps", "width", "threads", "senderDrops", "lagMs"}) {
            const auto value = msg.value(QLatin1String(key));
            if (value.isDouble() && std::isfinite(value.toDouble())) host[QLatin1String(key)] = value.toDouble();
        }
        host["pipeline"] = msg.value("pipeline").toString().left(128);
        if (const auto transport = msg.value("transport").toString(); transport == "udp" || transport == "tcp") {
            host["transport"] = transport;
            if (transport == "tcp") viewer->second.receiver.setReliable(true);
        }
        viewer->second.hostStats = host;
        showViewer(peer, "live", {}, {{"host", host}});
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
                host->second.lastReport = now_();
                // UDP losing much of the stream for seconds: over TCP from now
                // on, late rather than lost (not for an extended display, which
                // must stay direct), starting the bitrate over.
                auto& h = host->second;
                // Nothing at all arrived of what was sent: all of it is lost
                // (the viewer cannot count frames it never saw a piece of).
                // (Once something has arrived: before that the stream is still
                // starting, not lost.)
                if (report->received > 0) h.delivered = true;
                const quint64 lost = h.delivered && report->received + report->lost == 0 ? h.sentSinceReport : report->lost;
                h.sentSinceReport = 0;
                if (!display_ && overTcp_ && (h.path == "direct" || h.path == "relayed") &&
                    h.lossy.report(report->received, lost) && overTcp_(peer)) {
                    emit activity(QStringLiteral("%1 loses too much over UDP: its stream goes over TCP now").arg(authorize_(peer).name));
                    pathChanged(peer, QStringLiteral("tcp")); h.forceKey = true;
                    return true;
                }
                // How far behind the viewer is: the newest frame it has against
                // the newest sent, on this clock. Well behind (TCP queuing what
                // the path cannot carry): drop what waits, start again from a
                // keyframe, and ease the bitrate.
                if (report->latestMs >= 0) h.lagMs = std::max(0, int(qint32(h.lastSentCaptureMs - quint32(report->latestMs))));
                if (h.lagMs > BehindMs && now_() - h.behindAt > 1500) {
                    h.behindAt = now_(); h.sender.dropQueued(); h.forceKey = true;
                }
                h.congestion.update(*report, h.lagMs);
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
    // Played out at the host's pace, every picture is needed; shown at once,
    // only the last of a batch (the others just advance the decoder). A
    // picture costs a copy out of the decoder (from the GPU, for hardware
    // decoding), so a decoder that has fallen behind (a batch of several:
    // e.g. a phone's 1220x2712 screen at 60 fps) makes only the last one and
    // catches up instead of building a backlog that is then thrown away.
    const bool everyFrame = display_ || (viewer.bufferMs > 0 && complete.size() <= 2);
    QMetaObject::invokeMethod(decoderWorker_, [this, peer, token, generation, complete = std::move(complete), started, everyFrame] {
        auto& decoder = decoders_[peer]; if (!decoder) decoder = std::make_unique<Decoder>();
        QVector<Viewer::Shown> images; bool any = false;
        for (qsizetype i = 0; i < complete.size(); ++i) {
            bool decoded = false;
            auto picture = decoder->decodeFrame(complete[i], everyFrame || i == complete.size() - 1, &decoded);
            any |= decoded;
            if (picture.isValid()) {
                if (!everyFrame) images.clear();
                images.push_back({std::move(picture), complete[i].captureMs});
            }
        }
        // Per frame: a batch taking longer is not a slower decoder.
        const double elapsed = double(monotonicMs() - started) / std::max<qsizetype>(1, complete.size());
        QMetaObject::invokeMethod(this, [this, peer, token, generation, images, any, elapsed] {
            const auto it = viewers_.find(peer);
            if (generation != decoderGeneration_ || it == viewers_.end() || it->second.token != token) return;
            auto& v = it->second;
            v.decoding = false; v.decodeMs = elapsed; stats_.decodeMs += elapsed;
            if (!any) {
                v.toDecode.clear();
                v.receiver.reset(token, v.fps);
                send_(peer, message("scr.keyframe", token));
            } else if (!images.isEmpty()) {
                v.decoded += int(images.size());
                if (!v.pictured) {
                    v.pictured = true;
                    showViewer(peer, "live", {}, {{"pictured", true}});
                }
                if (display_) {
                    // At most 3 waiting: older ones are too late to be worth showing.
                    for (const auto& image : images) v.present.push_back(image);
                    while (v.present.size() > 3) v.present.pop_front();
                    presentNext(peer, v, now_());
                } else if (v.bufferMs <= 0) {
                    emit frameReady(hex(peer), images.back().picture); ++stats_.shown; ++v.shown;
                } else {
                    const qint64 now = now_();
                    for (const auto& image : images) {
                        // The playout clock follows the quickest transit of the last
                        // few seconds: frames slowed by the network wait less.
                        const qint32 transit = qint32(quint32(now) - image.captureMs);
                        v.transits.push_back({now, transit});
                        while (v.transits.size() > 1 && now - v.transits.front().first > 5000) v.transits.pop_front();
                        qint32 quickest = transit;
                        for (const auto& [at, value] : v.transits) quickest = std::min(quickest, value);
                        v.baseline = quickest; v.haveBaseline = true;
                        if (qint32(image.captureMs + quint32(v.baseline + v.bufferMs) - quint32(now)) < 0) ++v.late;
                        v.present.push_back(image);
                    }
                    // A second or more behind: catch up rather than fall further behind.
                    while (v.present.size() > 240) { v.present.pop_front(); ++v.skipped; }
                    playout(peer, v, now);
                }
            }
            decodeNext(peer);
        });
    });
}

// Extended displays: shows the next waiting picture once its time has come,
// one per frame interval, a little faster while more are waiting so latency
// does not build.
void ScreenService::presentNext(const Peer& peer, Viewer& viewer, qint64 now) {
    if (viewer.present.empty()) return;
    const double interval = 1000. / std::max(1, viewer.fps);
    if (now < viewer.presentAt) return;
    emit frameReady(hex(peer), viewer.present.front().picture); ++stats_.shown; ++viewer.shown;
    viewer.present.pop_front();
    const double next = viewer.present.size() > 1 ? interval * .75 : interval;
    // After a pause, the cadence starts again from now.
    viewer.presentAt = now - viewer.presentAt > qint64(interval) ? now + qint64(next) : viewer.presentAt + qint64(next);
}

// Screens and cameras with a playout buffer: each picture is shown at its
// capture time plus the buffer, on the clock of the quickest recent transit,
// so the stream keeps the host's pace whatever the network does to it. When
// several are due (the viewer fell behind), the newest is shown.
void ScreenService::playout(const Peer& peer, Viewer& viewer, qint64 now) {
    std::optional<QVideoFrame> show;
    while (!viewer.present.empty()) {
        const auto& front = viewer.present.front();
        const qint32 wait = qint32(front.captureMs + quint32(viewer.baseline + viewer.bufferMs) - quint32(now));
        if (wait > 0) break;
        if (show) ++viewer.skipped;
        show = front.picture;
        viewer.present.pop_front();
    }
    if (show) { emit frameReady(hex(peer), *show); ++stats_.shown; ++viewer.shown; }
}

void ScreenService::pathChanged(const Peer& peer, const QString& path) {
    if (auto it = hosts_.find(peer); it != hosts_.end()) {
        it->second.path = path;
        it->second.congestion.reset(path, it->second.fps, it->second.width,
                                    it->second.maxBitrate ? it->second.maxBitrate : settings_.maxBitrate, display_ ? DisplayStartBitrate : 0);
        // An extended display needs the devices near each other: through the
        // relay or over TCP it would lag too far behind the mouse.
        if (display_ && it->second.live && path != "direct" && path != "opening") {
            failMedia(peer, QStringLiteral("An extended display needs both devices on the same network (the connection is %1).").arg(path));
            return;
        }
    }
    if (auto it = viewers_.find(peer); it != viewers_.end()) {
        it->second.path = path; showViewer(peer, it->second.state);
        if (path == "tcp") it->second.receiver.setReliable(true);
    }
}

void ScreenService::submitCapture(const QVideoFrame& frame) {
    ++stats_.offered;
    // Frames cross to this thread to join the queue. A backlog on the way
    // means this thread is behind; more would only be dropped there.
    if (postedCaptures_.fetch_add(1) >= 4) { --postedCaptures_; ++stats_.busy; return; }
    const auto captured = quint32(monotonicMs());
    QMetaObject::invokeMethod(this, [this, frame, captured] { --postedCaptures_; queueCapture(frame, {}, captured); });
}
void ScreenService::submitImage(const QImage& image) {
    ++stats_.offered;
    queueCapture({}, image, quint32(now_()));
}

void ScreenService::queueCapture(const QVideoFrame& video, const QImage& image, quint32 captureMs) {
    bool watched = false; int fps = settings_.maxFps, width = settings_.maxWidth;
    for (const auto& [peer, host] : hosts_) if (host.live) {
        watched = true; fps = std::min(fps, host.congestion.fps()); width = std::min(width, host.congestion.width());
    }
    // The native path, while it is tried or used, captures by itself: Qt's
    // frames are not needed then. (Synthetic images always count.)
    if (!watched || (video.isValid() && !camera_ && !nativeDisabled_)) return;
    // Frames are due every 1000/fps ms, kept fractional: whole milliseconds
    // would drift at high rates (165 fps is 6.06 ms). Late frames do not
    // bunch up to catch up.
    const double now = double(now_());
    if (now < nextCapture_ - .5) { ++stats_.throttled; return; }
    if (int(pipeline_.size()) >= std::clamp(settings_.queueFrames, 1, 30)) { ++stats_.dropped; return; }
    nextCapture_ = std::max(nextCapture_ + 1000. / fps, now);
    Job job; job.seq = nextJob_++; job.captureMs = captureMs; job.video = video; job.image = image;
    const auto seq = job.seq; const auto generation = generation_;
    pipeline_.push_back(std::move(job));
    stats_.queueSum += int(pipeline_.size()); ++stats_.queueSamples;
    stats_.queuePeak = std::max(stats_.queuePeak, int(pipeline_.size()));
    // Converted for the encoder ahead of time, at the size it will encode:
    // only once it is open (before that, it converts the frame itself).
    const QSize source = video.isValid() ? video.size() : image.size();
    if (!encoderInput_.valid() || source.width() < 2 || source.height() < 2) {
        pipeline_.back().converted = true;
        pumpEncoder();
        return;
    }
    const int targetWidth = std::max(2, std::min(source.width(), width) & ~1);
    const EncoderInput input{QSize(targetWidth, std::max(2, int(qint64(source.height()) * targetWidth / source.width()) & ~1)),
                             encoderInput_.format};
    QPointer<ScreenService> self(this);
    convertPool_.start([self, video, image, input, generation, seq] {
        QElapsedTimer clock; clock.start();
        auto prepared = Encoder::prepare(video, image, input);
        const double ms = double(clock.nsecsElapsed()) / 1e6;
        if (!self) return;
        QMetaObject::invokeMethod(self.data(), [self, generation, seq, prepared = std::move(prepared), ms]() mutable {
            if (!self || generation != self->generation_) return;
            for (auto& job : self->pipeline_) {
                if (job.seq != seq) continue;
                job.converted = true; job.convertMs = ms;
                if (prepared) { job.prepared = std::move(prepared); job.video = {}; job.image = {}; }
                break;
            }
            self->pumpEncoder();
        });
    });
}

ScreenService::Targets ScreenService::takeTargets() {
    Targets targets;
    int width = settings_.maxWidth, fps = settings_.maxFps, bitrate = settings_.maxBitrate;
    for (auto& [peer, host] : hosts_) if (host.live) {
        targets.hosts[peer] = host.token; targets.key |= host.forceKey; host.forceKey = false;
        width = std::min(width, host.congestion.width()); fps = std::min(fps, host.congestion.fps());
        bitrate = std::min(bitrate, host.congestion.bitrate());
    }
    targets.options = CodecOptions{width, fps, bitrate, {}, settings_.ciscoLibrary, converterThreads()};
    return targets;
}

// Encodes the oldest frame once it is converted: strictly one after another,
// in capture order.
void ScreenService::pumpEncoder() {
    if (encoding_ || pipeline_.empty() || !pipeline_.front().converted) return;
    // One encode at a time with the native path too: they share the frame numbers.
    if (capturePending_.exchange(true)) return;
    Job job = std::move(pipeline_.front());
    pipeline_.pop_front();
    auto targets = takeTargets();
    if (targets.hosts.empty()) { pipeline_.clear(); capturePending_ = false; return; }
    encoding_ = true; ++stats_.encodes;
    if (job.prepared) { ++stats_.converts; stats_.convertMs += job.convertMs; }
    const auto number = nextFrame_; const auto generation = generation_;
    QMetaObject::invokeMethod(encoderWorker_, [this, targets, generation, number, job = std::move(job)] {
        const auto start = monotonicMs();
        const auto result = job.prepared ? encoder_->encode(*job.prepared, targets.options, 1, number, job.captureMs, targets.key)
            : !job.image.isNull() ? encoder_->encode(job.image, targets.options, 1, number, job.captureMs, targets.key)
                                  : encoder_->encode(job.video, targets.options, 1, number, job.captureMs, targets.key);
        const auto error = encoder_->error(); const auto name = encoder_->name(); const auto input = encoder_->input();
        const double encodeMs = double(monotonicMs() - start);
        QMetaObject::invokeMethod(this, [this, targets, generation, result, error, name, input, encodeMs] {
            encoding_ = false; capturePending_ = false;
            if (generation == generation_) encoderInput_ = input;
            encoded(targets.hosts, generation, result, error, name, targets.options.fps, encodeMs, true);
            pumpEncoder();
        });
    });
}

// The native path (desktop duplication): captures, converts and encodes on
// the GPU, one frame at a time, polled from tick().
void ScreenService::encodeCapture(quint32 captureMs) {
    bool watched = false; int fps = settings_.maxFps;
    for (const auto& [peer, host] : hosts_) if (host.live) { watched = true; fps = std::min(fps, host.congestion.fps()); }
    const double now = double(now_());
    if (!watched || now < nextCapture_ - .5) { if (watched) ++stats_.throttled; capturePending_ = false; pumpEncoder(); return; }
    nextCapture_ = std::max(nextCapture_ + 1000. / fps, now); ++stats_.encodes;
    const auto targets = takeTargets();
    const auto number = nextFrame_; const auto generation = generation_;
    QString nativeMonitor;
    for (const auto& [peer, host] : hosts_) if (host.live) {
        const auto info = settings_.monitors.value(host.monitor).toMap();
        nativeMonitor = info.value("nativeId", info.value("name")).toString(); break;
    }
    const bool wasNative = nativeActive_;
    QMetaObject::invokeMethod(encoderWorker_, [this, targets, generation, number, captureMs, nativeMonitor, wasNative] {
        const auto encodeStart = monotonicMs();
        if (!desktop_) desktop_ = desktopEncoder();
        DesktopResult gpu;
        if (desktop_) gpu = desktop_->encode(nativeMonitor, targets.options, number, captureMs, targets.key);
        if (!gpu.active && wasNative) encoder_->reset();  // a retry failing leaves the Qt path alone
        const double encodeMs = double(monotonicMs() - encodeStart);
        QMetaObject::invokeMethod(this, [this, targets, generation, gpu, encodeMs] {
            capturePending_ = false;
            if (generation == generation_) {
                if (gpu.active) {
                    nativeFailures_ = 0; nativeRetryAt_ = 0; nativeWorked_ = true; lastNativeError_.clear();
                } else {
                    if (!nativeFailures_ && !gpu.detail.isEmpty()) emit activity(gpu.detail);
                    if (!gpu.detail.isEmpty()) lastNativeError_ = gpu.detail;
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
            // The native path returns no frame when the screen has not changed:
            // nothing was lost.
            encoded(targets.hosts, generation, gpu.frame, {}, gpu.active ? gpu.pipeline : QString(), targets.options.fps, encodeMs, false);
            pumpEncoder();  // queued frames waited for it
        });
    });
}

// An encoded frame (or none) for the hosts it was meant for. `lostIfEmpty`:
// no frame means one was lost, so the next must be a keyframe.
void ScreenService::encoded(const std::map<Peer, quint16>& targets, quint64 generation, const std::optional<EncodedFrame>& result,
                            const QString& error, const QString& name, int fps, double encodeMs, bool lostIfEmpty) {
    stats_.encodeMs += encodeMs; if (result) ++stats_.frames;
    if (generation != generation_) return;
    if (!result && error.isEmpty() && lostIfEmpty)
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
            it->second.lastSentCaptureMs = frame.captureMs;
            if (!it->second.sender.enqueue(frame, now_(), 1000 / fps, spread, paceBps)) { it->second.forceKey = true; ++stats_.refused; }
            else { ++it->second.framesSent; ++stats_.queued; stats_.bytesSent += frame.bytes.size(); }
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
}

// Once a second: each viewer of this device hears how the sending side
// keeps up (scr.stats); the log gets a line with UCD_SCREEN_STATS.
void ScreenService::publishStats(qint64 now) {
    const qint64 span = std::max<qint64>(1, now - stats_.at);
    const auto perSecond = [&](double n) { return n * 1000. / span; };
    const double encodeMs = stats_.encodes ? stats_.encodeMs / stats_.encodes : 0.;
    const double convertMs = stats_.converts ? stats_.convertMs / stats_.converts : 0.;
    const double queue = stats_.queueSamples ? double(stats_.queueSum) / stats_.queueSamples : 0.;
    for (const auto& [peer, host] : hosts_) if (host.live) {
        auto msg = message("scr.stats", host.token);
        msg["fps"] = perSecond(stats_.frames); msg["encodeMs"] = encodeMs; msg["convertMs"] = convertMs;
        msg["queue"] = queue; msg["queuePeak"] = stats_.queuePeak;
        msg["dropped"] = perSecond(stats_.dropped + stats_.busy); msg["throttled"] = perSecond(stats_.throttled);
        msg["sentKbps"] = double(stats_.bytesSent) * 8. / span;
        msg["targetFps"] = host.congestion.fps(); msg["targetKbps"] = host.congestion.bitrate() / 1000;
        msg["width"] = host.congestion.width(); msg["threads"] = converterThreads();
        msg["senderDrops"] = double(host.sender.dropped()); msg["pipeline"] = lastPipeline_;
        msg["transport"] = host.path == "tcp" ? "tcp" : "udp"; msg["lagMs"] = host.lagMs;
        send_(peer, msg);
    }
    if (statsOn_ && (!hosts_.empty() || !viewers_.empty())) {
        const auto rate = [&](double n) { return QString::number(perSecond(n), 'f', 1); };
        QString line = QStringLiteral("[stats%1]").arg(camera_ ? " camera" : display_ ? " display" : "");
        for (const auto& [peer, host] : hosts_) if (host.live) {
            line += QStringLiteral(" host: offered %1/s (busy %2/s), throttled %3/s, dropped %4/s, queue %5 (peak %6),"
                                   " convert %7 ms on %8 threads, encodes %9/s at %10 ms, frames %11/s, refused %12/s, sender drops %13"
                                   " | target %14 fps %15 kbit/s %16 px | loss %17% trend %18 ms | %19")
                .arg(rate(stats_.offered), rate(stats_.busy), rate(stats_.throttled), rate(stats_.dropped))
                .arg(queue, 0, 'f', 1).arg(stats_.queuePeak).arg(convertMs, 0, 'f', 1).arg(converterThreads())
                .arg(rate(stats_.encodes)).arg(encodeMs, 0, 'f', 1).arg(rate(stats_.frames), rate(stats_.refused))
                .arg(host.sender.dropped()).arg(host.congestion.fps()).arg(host.congestion.bitrate() / 1000).arg(host.congestion.width())
                .arg(host.reportLoss * 100, 0, 'f', 1).arg(host.reportTrendMs, 0, 'f', 1).arg(lastPipeline_);
            break;
        }
        if (!viewers_.empty())
            line += QStringLiteral(" viewer: complete %1/s, decodes %2/s at %3 ms/frame, shown %4/s")
                .arg(rate(stats_.completed), rate(stats_.decodes))
                .arg(stats_.decodes ? stats_.decodeMs / stats_.decodes : 0., 0, 'f', 1).arg(rate(stats_.shown));
        qInfo().noquote() << line;
    }
    stats_.offered = 0; stats_.busy = 0;
    stats_.throttled = stats_.dropped = stats_.encodes = stats_.frames = stats_.queued = stats_.refused = 0;
    stats_.converts = stats_.queueSamples = stats_.queueSum = stats_.queuePeak = 0;
    stats_.completed = stats_.decodes = stats_.shown = 0;
    stats_.encodeMs = stats_.convertMs = stats_.decodeMs = 0; stats_.bytesSent = 0; stats_.at = now;
}

void ScreenService::tick() {
    const auto now = now_(); QVector<Peer> expired, expiredViewers;
    if (now - stats_.at >= 1000) publishStats(now);
    // The native path (desktop duplication) captures by itself. Try it as
    // soon as someone watches instead of waiting for Qt's capture to deliver
    // a first frame, which on some systems never comes.
    const bool watched = std::any_of(hosts_.begin(), hosts_.end(), [](const auto& entry) { return entry.second.live; });
    if (nativeDisabled_ && nativeRetryAt_ && now >= nativeRetryAt_ && settings_.gpuCapture && !camera_) {
        nativeDisabled_ = false; nativeRetryAt_ = 0; nativeAttemptAt_ = now + 500; updateQtCapture();
    }
    if (watched && !camera_ && settings_.gpuCapture && !nativeDisabled_ && now >= nativeAttemptAt_ && !capturePending_.exchange(true))
        encodeCapture(quint32(now));
    const bool controlled = std::any_of(hosts_.begin(), hosts_.end(), [](const auto& entry) { return entry.second.control; });
    if (!controlled && input_.pressedCount() && now - lastReleaseRetry_ >= 100) {
        lastReleaseRetry_ = now; input_.release();
    }
    QVector<std::pair<Peer, QString>> displayFailures;
    for (auto& [peer, host] : hosts_) {
        if (display_ && host.live && host.path == "opening" && now - host.startedAt > 5000)
            displayFailures.push_back({peer, QStringLiteral("No direct connection to the other device: an extended display needs both devices on the same network.")});
        else if (display_ && host.live && !host.framesSent && now - host.startedAt > 15000)
            displayFailures.push_back({peer, lastNativeError_.isEmpty()
                ? QStringLiteral("The extended display could not be captured.")
                : QStringLiteral("The extended display could not be captured. %1").arg(lastNativeError_)});
        if (!host.live && now >= host.expires) expired.push_back(peer);
        else if (host.live) {
            if (host.controlRequest && now >= host.controlExpires) setControl(peer, false, host.controlRequest, "control request timed out");
            if (host.control && !controlAllowed(peer)) setControl(peer, false, 0, "control permission withdrawn");
            if (now - host.lastReport > 5000 || !allowed(peer)) expired.push_back(peer);
            else if (!host.sender.pump(now, [&](const QByteArray& packet) {
                         const bool sent = datagram_(peer, packet); if (sent) ++host.sentSinceReport; return sent;
                     })) host.forceKey = true;
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
        if (!viewer.present.empty()) { if (display_) presentNext(peer, viewer, now); else playout(peer, viewer, now); }
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
            const double span = double(std::max<qint64>(1, now - viewer.reportAt));
            // How long the next picture waits to be shown: the buffer in use.
            const double waiting = viewer.present.empty() ? 0.
                : double(qint32(viewer.present.front().captureMs + quint32(viewer.baseline + viewer.bufferMs) - quint32(now)));
            showViewer(peer, "live", {}, {{"fps", viewer.decoded * 1000. / span},
                {"shownFps", viewer.shown * 1000. / span}, {"late", viewer.late}, {"skipped", viewer.skipped},
                {"bufferMs", viewer.bufferMs}, {"waitingMs", waiting}, {"waitingFrames", int(viewer.present.size())},
                {"bitrate", double(viewer.bytes) * 8000. / span}, {"decodeMs", viewer.decodeMs},
                {"loss", double(report.lost) / std::max(1., double(report.received + report.lost))},
                {"jitterMs", report.jitterMs}});
            viewer.bytes = 0; viewer.decoded = 0; viewer.shown = viewer.late = viewer.skipped = 0; viewer.reportAt = now;
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
