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
        QObject* parent, Now now, std::unique_ptr<InputBackend> input)
    : QObject(parent), send_(std::move(send)), authorize_(std::move(authorize)), open_(std::move(open)),
      close_(std::move(close)), datagram_(std::move(datagram)), now_(now ? std::move(now) : monotonicMs),
      encoderWorker_(new QObject), decoderWorker_(new QObject), encoder_(std::make_unique<Encoder>()),
      input_(input ? std::move(input) : nativeInputBackend()) {
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
    QMetaObject::invokeMethod(encoderWorker_, [this] { encoder_.reset(); }, Qt::BlockingQueuedConnection);
    QMetaObject::invokeMethod(decoderWorker_, [this] { decoders_.clear(); }, Qt::BlockingQueuedConnection);
    encoderThread_.quit(); decoderThread_.quit(); encoderThread_.wait(); decoderThread_.wait();
}

void ScreenService::configure(const ScreenSettings& settings) {
    const bool offered = settings_.offered;
    const bool controlOffered = settings_.controlOffered;
    settings_ = settings;
    settings_.offered = offered;
    settings_.controlOffered = controlOffered;
    settings_.maxFps = std::clamp(settings.maxFps, 1, 60);
    settings_.maxWidth = std::clamp(settings.maxWidth, 320, 3840);
    settings_.maxBitrate = std::clamp(settings.maxBitrate, 150000, 20000000);
    settings_.maxViewers = 1;
    settings_.monitor = std::clamp(settings.monitor, 0, std::max(0, int(settings.monitors.size()) - 1));
    if (!settings_.offered) stopHosting("screen service disabled");
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
    return settings_.controlOffered && access.trusted && access.permissions.contains(perm::ControlInput);
}

bool ScreenService::allowed(const Peer& peer) const {
    const auto access = authorize_(peer);
    return settings_.offered && access.trusted && access.permissions.contains(perm::ScreenView);
}

void ScreenService::fail(const Peer& peer, quint32 request, const QString& code) {
    send_(peer, {{"t", "scr.err"}, {"request", double(request)}, {"code", code}});
    emit activity(QStringLiteral("Screen request from %1: %2").arg(authorize_(peer).name, code));
}

void ScreenService::request(const Peer& peer, bool control, int maxWidth, int fps) {
    if (!authorize_(peer).trusted) { showViewer(peer, "denied", "Trust this device before viewing its screen."); return; }
    if (viewers_.contains(peer)) stop(peer);
    Viewer viewer;
    viewer.request = nextRequest_++; if (!nextRequest_) ++nextRequest_;
    viewer.width = std::clamp(maxWidth, 320, 3840); viewer.fps = std::clamp(fps, 1, 60);
    viewer.expires = now_() + 35000; viewer.reportAt = now_();
    viewers_.emplace(peer, std::move(viewer)); timer_.start();
    showViewer(peer, "requesting");
    if (!send_(peer, {{"t", "scr.request"}, {"request", double(viewers_.at(peer).request)},
        {"wantControl", control}, {"maxWidth", maxWidth}, {"maxFps", fps}, {"codecs", QJsonArray{"h264"}}}))
        stop(peer, "connection unavailable");
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
    if (remember) {
        emit rememberPermission(authorize_(*peer).key, perm::ScreenDirect);
        if (it->second.wantControl) emit rememberPermission(authorize_(*peer).key, perm::ControlDirect);
    }
    startHost(*peer);
}

void ScreenService::startHost(const Peer& peer) {
    auto& host = hosts_.at(peer);
    if (!allowed(peer)) { fail(peer, host.request, "forbidden"); hosts_.erase(peer); return; }
    host.token = quint16(QRandomGenerator::system()->bounded(1u, 65536u));
    host.live = true; host.lastReport = now_(); host.monitor = settings_.monitor;
    host.congestion.reset(host.path, host.fps, host.width, settings_.maxBitrate);
    if (!open_(peer)) { fail(peer, host.request, "transport"); hosts_.erase(peer); return; }
    emit consentClosed(hex(peer), host.request);
    host.control = host.wantControl && controlAllowed(peer) && input_.available().isEmpty();
    const auto bounds = settings_.monitors.value(host.monitor).toMap();
    input_.setBounds(QRect(bounds.value("inputX", bounds.value("x")).toInt(), bounds.value("inputY", bounds.value("y")).toInt(),
        bounds.value("inputWidth", bounds.value("width")).toInt(), bounds.value("inputHeight", bounds.value("height")).toInt()));
    send_(peer, {{"t", "scr.started"}, {"request", double(host.request)}, {"session", host.token},
        {"codec", "h264"}, {"width", host.width}, {"height", 0}, {"fps", host.fps},
        {"monitors", QJsonArray::fromVariantList(settings_.monitors)}, {"monitor", host.monitor}, {"control", host.control},
#ifdef Q_OS_WIN
        {"cursorEmbedded", false}
#else
        {"cursorEmbedded", true}
#endif
    });
    emit captureChanged(true, host.monitor); showHosting();
    emit activity(QStringLiteral("Screen sharing started with %1").arg(authorize_(peer).name));
}

void ScreenService::showViewer(const Peer& peer, const QString& state, const QString& detail, const QVariantMap& extra) {
    QVariantMap update = extra; update["state"] = state; update["detail"] = detail;
    if (const auto it = viewers_.find(peer); it != viewers_.end()) {
        it->second.state = state; update["path"] = it->second.path;
    }
    emit viewerChanged(hex(peer), update);
}

void ScreenService::showHosting() {
    QVariantList viewers;
    for (const auto& [peer, host] : hosts_) if (host.live)
        viewers.push_back(QVariantMap{{"devId", hex(peer)}, {"name", authorize_(peer).name}, {"control", host.control}});
    emit hostingChanged(viewers);
}

void ScreenService::stop(const Peer& peer, const QString& reason) {
    const auto host = hosts_.find(peer);
    if (host != hosts_.end()) {
        if (host->second.control) input_.release();
        if (host->second.controlRequest) emit consentClosed(hex(peer), host->second.controlRequest);
        if (host->second.live) send_(peer, message("scr.stop", host->second.token));
        else fail(peer, host->second.request, "ended");
        emit consentClosed(hex(peer), host->second.request);
        hosts_.erase(host); showHosting();
        emit captureChanged(false, settings_.monitor);
        QMetaObject::invokeMethod(encoderWorker_, [this] { encoder_->reset(); });
        emit activity(QStringLiteral("Screen sharing stopped: %1").arg(reason));
    }
    const auto viewer = viewers_.find(peer);
    if (viewer != viewers_.end()) {
        auto msg = message("scr.stop", viewer->second.token); msg["request"] = double(viewer->second.request);
        send_(peer, msg); viewers_.erase(viewer);
        QMetaObject::invokeMethod(decoderWorker_, [this, peer] { decoders_.erase(peer); });
        showViewer(peer, "ended", reason);
    }
    close_(peer);
    if (hosts_.empty() && viewers_.empty() && !input_.pressedCount()) timer_.stop();
}
void ScreenService::stopHosting(const QString& reason) {
    while (!hosts_.empty()) stop(hosts_.begin()->first, reason);
}
void ScreenService::peerGone(const Peer& peer) { stop(peer, "device disconnected"); }
void ScreenService::reset() {
    ++generation_;
    while (!hosts_.empty()) stop(hosts_.begin()->first);
    while (!viewers_.empty()) stop(viewers_.begin()->first);
}
void ScreenService::policyChanged() {
    QVector<Peer> revoked;
    for (const auto& [peer, host] : hosts_) if (!allowed(peer)) revoked.push_back(peer);
    for (const auto& [peer, viewer] : viewers_) if (!authorize_(peer).trusted) revoked.push_back(peer);
    for (const auto& peer : revoked) stop(peer, "permission withdrawn");
    QVector<Peer> controlRevoked;
    for (const auto& [peer, host] : hosts_) if (host.control && !controlAllowed(peer)) controlRevoked.push_back(peer);
    for (const auto& peer : controlRevoked) setControl(peer, false, 0, "control permission withdrawn");
}

bool ScreenService::handle(const Peer& peer, const QJsonObject& msg) {
    const auto type = msg.value("t").toString();
    if (!type.startsWith("scr.") && !type.startsWith("in.")) return false;
    const double requestValue = msg.value("request").toDouble();
    const quint32 requestId = requestValue >= 1 && requestValue <= 4294967295. && std::floor(requestValue) == requestValue
        ? quint32(requestValue) : 0;
    if (type == "scr.request") {
        if (!requestId || !allowed(peer)) { fail(peer, requestId, "forbidden"); return true; }
        if (!msg.value("codecs").toArray().contains("h264")) { fail(peer, requestId, "codec"); return true; }
        const bool wantControl = msg.value("wantControl").toBool();
        if (wantControl && (!controlAllowed(peer) || !input_.available().isEmpty())) {
            fail(peer, requestId, !controlAllowed(peer) ? QString("control forbidden") : input_.available()); return true;
        }
        if (!hosts_.empty()) { fail(peer, requestId, "busy"); return true; }
        Host host; host.request = requestId; host.expires = now_() + 30000;
        host.wantControl = wantControl;
        host.width = std::min(settings_.maxWidth, std::clamp(msg.value("maxWidth").toInt(1920), 320, 3840));
        host.fps = std::min(settings_.maxFps, std::clamp(msg.value("maxFps").toInt(30), 1, 60));
        hosts_.emplace(peer, std::move(host)); timer_.start();
        if (authorize_(peer).permissions.contains(perm::ScreenDirect) &&
            (!wantControl || authorize_(peer).permissions.contains(perm::ControlDirect))) startHost(peer);
        else {
            send_(peer, {{"t", "scr.wait"}, {"request", double(requestId)}});
            const auto access = authorize_(peer);
            emit consentRequested(hex(peer), requestId, {{"name", access.name}, {"fingerprint", access.fingerprint},
                {"control", wantControl}});
        }
        return true;
    }
    auto viewer = viewers_.find(peer);
    if (type == "scr.wait" || type == "scr.err" || type == "scr.started") {
        if (viewer == viewers_.end() || requestId != viewer->second.request || viewer->second.token) return true;
        if (type == "scr.wait") showViewer(peer, "waiting", "Waiting for the host to allow screen sharing.");
        else if (type == "scr.err") {
            const auto code = msg.value("code").toString();
            viewers_.erase(viewer); showViewer(peer, code == "busy" ? "busy" : "denied", code);
        } else {
            const int token = msg.value("session").toInt();
            if (token < 1 || token > 65535 || msg.value("codec") != "h264" || !authorize_(peer).trusted) return true;
            viewer->second.token = quint16(token); viewer->second.expires = now_() + 10000;
            viewer->second.fps = std::clamp(msg.value("fps").toInt(30), 1, 60);
            viewer->second.receiver.reset(quint16(token), viewer->second.fps);
            viewer->second.control = msg.value("control").toBool();
            if (!open_(peer)) { stop(peer, "transport unavailable"); return true; }
            showViewer(peer, "live", {}, {{"monitors", msg.value("monitors").toArray().toVariantList()},
                {"monitor", msg.value("monitor").toInt()}, {"control", viewer->second.control},
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
        if (forHost || forViewer || (host != hosts_.end() && !host->second.live && requestId == host->second.request))
            stop(peer, "stopped by peer");
    } else if (forViewer && type == "scr.error") {
        stop(peer, msg.value("detail").toString("media error"));
    } else if (forViewer && type == "scr.controlled") {
        if (requestId && requestId != viewer->second.controlRequest) return true;
        viewer->second.control = msg.value("on").toBool();
        viewer->second.controlRequest = 0;
        showViewer(peer, "live", msg.value("detail").toString(), {{"control", viewer->second.control}, {"controlPending", false}});
    } else if (forViewer && type == "scr.cursor") {
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
        if (!allowed(peer)) { stop(peer, "permission withdrawn"); return true; }
        if (type == "scr.control") {
            if (msg.value("on").toBool()) requestHostControl(peer, requestId);
            else setControl(peer, false, requestId);
        } else if (type == "scr.report") {
            if (const auto report = Feedback::parse(msg)) {
                if (now_() - host->second.lastReport < 100) return true;
                host->second.lastReport = now_(); host->second.congestion.update(*report);
            }
        } else if (type == "scr.keyframe") host->second.forceKey = true;
        else if (type == "scr.nack") {
            const double frameValue = msg.value("frame").toDouble(-1);
            if (frameValue < 0 || frameValue > 4294967295. || std::floor(frameValue) != frameValue) return true;
            const auto frame = quint32(frameValue);
            if (host->second.nackedFrame == frame) return true;
            host->second.nackedFrame = frame;
            const auto array = msg.value("indexes").toArray();
            if (array.size() > MaxFragments) return true;
            QVector<int> indexes; for (const auto& index : array) indexes.push_back(index.toInt(-1));
            for (const auto& packet : host->second.sender.retransmit(frame, indexes, now_()))
                if (!datagram_(peer, packet)) break;
        } else if (type == "scr.monitor") {
            const int index = msg.value("index").toInt(-1);
            if (index >= 0 && index < settings_.monitors.size()) {
                host->second.monitor = index; host->second.forceKey = true;
                input_.release();
                const auto bounds = settings_.monitors[index].toMap();
                input_.setBounds(QRect(bounds.value("inputX", bounds.value("x")).toInt(), bounds.value("inputY", bounds.value("y")).toInt(),
                    bounds.value("inputWidth", bounds.value("width")).toInt(), bounds.value("inputHeight", bounds.value("height")).toInt()));
                auto changed = message("scr.monitorChanged", host->second.token); changed["index"] = index; send_(peer, changed);
                emit captureChanged(true, index);
            }
        }
    }
    if (forViewer && type == "scr.monitorChanged") showViewer(peer, "live", {}, {{"monitor", msg.value("index").toInt()}});
    return true;
}

void ScreenService::receive(const Peer& peer, const QByteArray& packet) {
    const auto it = viewers_.find(peer);
    if (it == viewers_.end() || !it->second.token || !authorize_(peer).trusted) return;
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
    if (viewer.decoding) {
        // References cannot be discarded silently. Reset transport gating and
        // request a new keyframe rather than queueing unbounded decode work.
        viewer.receiver.reset(viewer.token, viewer.fps);
        send_(peer, message("scr.keyframe", viewer.token)); return;
    }
    viewer.decoding = true;
    const auto token = viewer.token; const auto generation = generation_; const auto started = monotonicMs();
    QMetaObject::invokeMethod(decoderWorker_, [this, peer, token, generation, complete = std::move(complete), started] {
        auto& decoder = decoders_[peer]; if (!decoder) decoder = std::make_unique<Decoder>();
        QImage image;
        for (const auto& frame : complete) {
            const auto decoded = decoder->decode(frame); if (!decoded.isNull()) image = decoded;
        }
        const double elapsed = monotonicMs() - started;
        QMetaObject::invokeMethod(this, [this, peer, token, generation, image, elapsed] {
            const auto it = viewers_.find(peer);
            if (generation != generation_ || it == viewers_.end() || it->second.token != token) return;
            it->second.decoding = false; it->second.decodeMs = elapsed;
            if (image.isNull()) {
                it->second.receiver.reset(token, it->second.fps);
                send_(peer, message("scr.keyframe", token));
            } else { ++it->second.decoded; emit imageReady(hex(peer), image); }
        });
    });
}

void ScreenService::pathChanged(const Peer& peer, const QString& path) {
    if (auto it = hosts_.find(peer); it != hosts_.end()) {
        it->second.path = path; it->second.congestion.reset(path, it->second.fps, it->second.width, settings_.maxBitrate);
    }
    if (auto it = viewers_.find(peer); it != viewers_.end()) {
        it->second.path = path; showViewer(peer, it->second.state);
    }
}

void ScreenService::submitCapture(const QVideoFrame& frame) {
    if (capturePending_.exchange(true)) return;
    const auto captured = quint32(monotonicMs());
    QMetaObject::invokeMethod(this, [this, frame, captured] { encodeCapture(frame, {}, captured); });
}
void ScreenService::submitImage(const QImage& image) {
    if (capturePending_.exchange(true)) return;
    encodeCapture({}, image, quint32(now_()));
}
void ScreenService::encodeCapture(const QVideoFrame& video, const QImage& image, quint32 captureMs) {
    if (hosts_.empty() || !hosts_.begin()->second.live) { capturePending_ = false; return; }
    auto& host = hosts_.begin()->second;
    if (host.sender.busy() || now_() - lastCapture_ < 1000 / host.congestion.fps()) { capturePending_ = false; return; }
    lastCapture_ = now_();
    const auto peer = hosts_.begin()->first; const auto token = host.token; const auto number = nextFrame_;
    const bool key = host.forceKey; host.forceKey = false;
    CodecOptions options{host.congestion.width(), host.congestion.fps(), host.congestion.bitrate(), {}, settings_.ciscoLibrary};
    QMetaObject::invokeMethod(encoderWorker_, [this, peer, token, number, key, video, image, captureMs, options] {
        const auto result = encoder_->encode(image.isNull() ? video.toImage() : image, options, token, number, captureMs, key);
        const auto error = encoder_->error(); const auto name = encoder_->name();
        QMetaObject::invokeMethod(this, [this, peer, token, result, error, name] {
            capturePending_ = false;
            const auto it = hosts_.find(peer);
            if (it == hosts_.end() || it->second.token != token) return;
            if (result) { it->second.sender.enqueue(*result, now_(), 1000 / it->second.congestion.fps()); ++nextFrame_; }
            else if (!error.isEmpty()) {
                auto failure = message("scr.error", token); failure["detail"] = error;
                send_(peer, failure); stop(peer, error);
            }
            Q_UNUSED(name);
        });
    });
}

void ScreenService::tick() {
    const auto now = now_(); QVector<Peer> expired;
    const bool controlled = std::any_of(hosts_.begin(), hosts_.end(), [](const auto& entry) { return entry.second.control; });
    if (!controlled && input_.pressedCount() && now - lastReleaseRetry_ >= 100) {
        lastReleaseRetry_ = now; input_.release();
    }
    for (auto& [peer, host] : hosts_) {
        if (!host.live && now >= host.expires) expired.push_back(peer);
        else if (host.live) {
            if (host.controlRequest && now >= host.controlExpires) setControl(peer, false, host.controlRequest, "control request timed out");
            if (host.control && !controlAllowed(peer)) setControl(peer, false, 0, "control permission withdrawn");
            if (now - host.lastReport > 5000 || !allowed(peer)) expired.push_back(peer);
            else if (!host.sender.pump(now, [&](const QByteArray& packet) { return datagram_(peer, packet); })) host.forceKey = true;
        }
    }
    for (auto& [peer, viewer] : viewers_) {
        if (!viewer.token) { if (now >= viewer.expires) expired.push_back(peer); continue; }
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
    for (const auto& peer : expired) stop(peer, "request or feedback timed out");
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
    if (!accepted) input_.release();
    host.control = accepted;
    auto msg = message("scr.controlled", host.token); msg["request"] = double(request); msg["on"] = accepted;
    msg["detail"] = on && !input_.available().isEmpty() ? input_.available() : detail;
    send_(peer, msg); showHosting();
    emit activity(QStringLiteral("Remote control %1 for %2").arg(accepted ? "started" : "stopped", authorize_(peer).name));
}
void ScreenService::requestHostControl(const Peer& peer, quint32 request) {
    auto& host = hosts_.at(peer);
    if (!request || host.controlRequest) return;
    if (!controlAllowed(peer) || !input_.available().isEmpty()) {
        setControl(peer, false, request, input_.available().isEmpty() ? QString("control forbidden") : input_.available()); return;
    }
    if (authorize_(peer).permissions.contains(perm::ControlDirect)) { setControl(peer, true, request); return; }
    host.controlRequest = request; host.controlExpires = now_() + 30000;
    const auto access = authorize_(peer);
    emit consentRequested(hex(peer), request, {{"name", access.name}, {"fingerprint", access.fingerprint}, {"control", true}, {"controlOnly", true}});
}
void ScreenService::requestControl(const Peer& peer, bool on) {
    const auto it = viewers_.find(peer); if (it == viewers_.end() || !it->second.token) return;
    auto& viewer = it->second;
    if (viewer.controlRequest && on) return;
    if (!on) send_(peer, message("in.release", viewer.token));
    viewer.controlRequest = nextRequest_++; if (!nextRequest_) ++nextRequest_;
    showViewer(peer, "live", {}, {{"controlPending", on}});
    auto msg = message("scr.control", viewer.token); msg["on"] = on; msg["request"] = double(viewer.controlRequest); send_(peer, msg);
}
void ScreenService::stopControl() {
    for (const auto& [peer, host] : hosts_) if (host.control || host.controlRequest) setControl(peer, false);
}
void ScreenService::sendInput(const Peer& peer, QJsonObject msg) {
    const auto it = viewers_.find(peer);
    if (it == viewers_.end() || !it->second.token || !it->second.control || !authorize_(peer).trusted) return;
    if (!msg.value("t").toString().startsWith("in.")) return;
    msg["session"] = it->second.token;
    if (!send_(peer, msg)) stop(peer, "input connection unavailable");
}

void ScreenService::submitCursor(const QPoint& position, bool visible, const QByteArray& shape, const QPoint& hotspot) {
    if (!shape.isEmpty() && shape.size() <= 49152 && shape != cursorShape_) {
        cursorShape_ = shape; cursorHotspot_ = hotspot; ++cursorVersion_;
    }
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
