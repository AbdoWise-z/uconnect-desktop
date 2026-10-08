#include "ScreenService.hpp"

#include <QJsonArray>
#include <QRandomGenerator>
#include <QtEndian>
#include <chrono>
#include <cmath>
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
        QObject* parent, Now now)
    : QObject(parent), send_(std::move(send)), authorize_(std::move(authorize)), open_(std::move(open)),
      close_(std::move(close)), datagram_(std::move(datagram)), now_(now ? std::move(now) : monotonicMs),
      encoderWorker_(new QObject), decoderWorker_(new QObject), encoder_(std::make_unique<Encoder>()) {
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
    settings_ = settings;
    settings_.offered = offered;
    settings_.maxFps = std::clamp(settings.maxFps, 1, 60);
    settings_.maxWidth = std::clamp(settings.maxWidth, 320, 3840);
    settings_.maxBitrate = std::clamp(settings.maxBitrate, 150000, 20000000);
    settings_.maxViewers = 1;
    if (!settings_.offered) stopHosting("screen service disabled");
}
void ScreenService::setOffered(bool offered) {
    settings_.offered = offered;
    if (!offered) stopHosting("screen service disabled");
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
    if (it == hosts_.end() || it->second.live || it->second.request != requestId) return;
    if (!allow || now_() >= it->second.expires || !allowed(*peer)) {
        fail(*peer, requestId, "denied"); emit consentClosed(peerId, requestId); hosts_.erase(it); return;
    }
    if (remember) emit rememberPermission(authorize_(*peer).key, perm::ScreenDirect);
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
    // Control is deliberately unavailable until the input phase lands.
    send_(peer, {{"t", "scr.started"}, {"request", double(host.request)}, {"session", host.token},
        {"codec", "h264"}, {"width", host.width}, {"height", 0}, {"fps", host.fps},
        {"monitors", QJsonArray::fromVariantList(settings_.monitors)}, {"control", false}});
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
        viewers.push_back(QVariantMap{{"devId", hex(peer)}, {"name", authorize_(peer).name}, {"control", false}});
    emit hostingChanged(viewers);
}

void ScreenService::stop(const Peer& peer, const QString& reason) {
    const auto host = hosts_.find(peer);
    if (host != hosts_.end()) {
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
    if (hosts_.empty() && viewers_.empty()) timer_.stop();
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
}

bool ScreenService::handle(const Peer& peer, const QJsonObject& msg) {
    const auto type = msg.value("t").toString();
    if (!type.startsWith("scr.")) return false;
    const double requestValue = msg.value("request").toDouble();
    const quint32 requestId = requestValue >= 1 && requestValue <= 4294967295. && std::floor(requestValue) == requestValue
        ? quint32(requestValue) : 0;
    if (type == "scr.request") {
        if (!requestId || !allowed(peer)) { fail(peer, requestId, "forbidden"); return true; }
        if (!msg.value("codecs").toArray().contains("h264")) { fail(peer, requestId, "codec"); return true; }
        if (!hosts_.empty()) { fail(peer, requestId, "busy"); return true; }
        Host host; host.request = requestId; host.expires = now_() + 30000;
        host.width = std::min(settings_.maxWidth, std::clamp(msg.value("maxWidth").toInt(1920), 320, 3840));
        host.fps = std::min(settings_.maxFps, std::clamp(msg.value("maxFps").toInt(30), 1, 60));
        hosts_.emplace(peer, std::move(host)); timer_.start();
        if (authorize_(peer).permissions.contains(perm::ScreenDirect)) startHost(peer);
        else {
            send_(peer, {{"t", "scr.wait"}, {"request", double(requestId)}});
            const auto access = authorize_(peer);
            emit consentRequested(hex(peer), requestId, {{"name", access.name}, {"fingerprint", access.fingerprint},
                {"control", false}});
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
            if (!open_(peer)) { stop(peer, "transport unavailable"); return true; }
            showViewer(peer, "live", {}, {{"monitors", msg.value("monitors").toArray().toVariantList()}, {"control", false}});
        }
        return true;
    }
    const int token = msg.value("session").toInt();
    auto host = hosts_.find(peer);
    const bool forHost = host != hosts_.end() && host->second.live && token == host->second.token;
    const bool forViewer = viewer != viewers_.end() && viewer->second.token && token == viewer->second.token;
    if (type == "scr.stop") {
        if (forHost || forViewer || (host != hosts_.end() && !host->second.live && requestId == host->second.request))
            stop(peer, "stopped by peer");
    } else if (forViewer && type == "scr.error") {
        stop(peer, msg.value("detail").toString("media error"));
    } else if (forHost) {
        if (!allowed(peer)) { stop(peer, "permission withdrawn"); return true; }
        if (type == "scr.report") {
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
                emit captureChanged(true, index);
            }
        }
    }
    return true;
}

void ScreenService::receive(const Peer& peer, const QByteArray& packet) {
    const auto it = viewers_.find(peer);
    if (it == viewers_.end() || !it->second.token || !authorize_(peer).trusted) return;
    auto& viewer = it->second;
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
    for (auto& [peer, host] : hosts_) {
        if (!host.live && now >= host.expires) expired.push_back(peer);
        else if (host.live) {
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
}
void ScreenService::monitor(const Peer& peer, int index) {
    if (const auto it = viewers_.find(peer); it != viewers_.end() && it->second.token) {
        auto msg = message("scr.monitor", it->second.token); msg["index"] = index; send_(peer, msg);
    }
}

}  // namespace ucd::screen
