#include "Session.hpp"

#include <algorithm>
#include <exception>

#include <QHostAddress>
#include <QJsonArray>
#include <QMetaObject>
#include <QTimer>

using namespace std::chrono_literals;

namespace ucd {
namespace {

constexpr auto kPollInterval     = 3s;
constexpr auto kSnapshotInterval = 500ms;
constexpr auto kLookupTimeout    = 2s;
constexpr auto kRetryAfter       = 30s;
constexpr uint8_t kLookupMax     = 30;

QString hex(const uconnect::DevId& id) {
    return QString::fromStdString(uconnect::to_hex(id));
}

std::optional<uconnect::DevId> parseDevId(const QString& s) {
    return uconnect::from_hex<uconnect::kDevIdLen>(s.toStdString());
}

bool isActive(uconnect::PeerState s) {
    using S = uconnect::PeerState;
    return s == S::Probing || s == S::Handshaking || s == S::Connected;
}

QString formatEndpoint(const uconnect::Endpoint& ep) {
    QHostAddress addr;
    if (ep.ip.family == uconnect::IpAddr::Family::V4) {
        const auto& b = ep.ip.bytes;
        addr.setAddress((quint32{b[0]} << 24) | (quint32{b[1]} << 16) | (quint32{b[2]} << 8) | b[3]);
        return QStringLiteral("%1:%2").arg(addr.toString()).arg(ep.port);
    }
    addr.setAddress(ep.ip.bytes.data());
    return QStringLiteral("[%1]:%2").arg(addr.toString()).arg(ep.port);
}

}  // namespace

QString shortId(const QString& devId) {
    return devId.left(8);
}

Session::Session(QObject* parent)
    : QObject(parent), pollTimer_(new QTimer(this)), snapshotTimer_(new QTimer(this)) {
    storage_ = new StorageService(
        [this](const uconnect::DevId& dev, std::span<const uint8_t> bytes) {
            return topic_ && topic_->send(dev, bytes);
        },
        [this](const uconnect::DevId& dev, const QString& permission) {
            return permissionsFor(dev).contains(permission);
        },
        this);
    pollTimer_->setInterval(kPollInterval);
    snapshotTimer_->setInterval(kSnapshotInterval);
    connect(pollTimer_, &QTimer::timeout, this, &Session::poll);
    connect(snapshotTimer_, &QTimer::timeout, this, [this] {
        emitSnapshot();
        updateStatus();
    });
}

Session::~Session() {
    stop();
}

void Session::start(const Options& options) {
    stop();

    const QString server = options.server.trimmed();
    if (server.isEmpty()) {
        setStatus(Status::Error, tr("Enter the rendezvous server address (host:port)."));
        return;
    }
    const auto creds = uconnect::TopicCreds::parse(options.topicLink.trimmed().toStdString());
    if (!creds) {
        setStatus(Status::Error, tr("The topic link is not a valid uconn:// link."));
        return;
    }
    // Open topics are encrypted against passive observers only: the server or
    // anyone on path could impersonate a peer. Not acceptable for an app that
    // hands out file access and remote control.
    if (!creds->is_keyed()) {
        setStatus(Status::Error, tr("Open topics are not allowed; use a keyed (protected) topic link."));
        return;
    }

    const quint64 gen = ++generation_;
    try {
        uconnect::Node::Config cfg;
        cfg.server  = server.toStdString();
        cfg.verbose = options.verbose;
        node_       = std::make_unique<uconnect::Node>(cfg);
        node_->run_in_background();
        topic_ = &node_->join(*creds);
    } catch (const std::exception& e) {
        node_.reset();
        topic_ = nullptr;
        setStatus(Status::Error, tr("Could not start: %1").arg(QString::fromUtf8(e.what())));
        return;
    }

    // These run on the library's loop thread: copy what is needed and hop to
    // ours. `this` outlives them because stop() shuts the Node down first.
    topic_->on_peer([this, gen](uconnect::DevId dev, uconnect::PeerState st) {
        QMetaObject::invokeMethod(this, [this, gen, dev, st] {
            if (gen == generation_) onPeerState(dev, st);
        }, Qt::QueuedConnection);
    });
    topic_->on_peer_closed([this, gen](uconnect::DevId dev, uconnect::PeerGone why) {
        QMetaObject::invokeMethod(this, [this, gen, dev, why] {
            if (gen == generation_) onPeerClosed(dev, why);
        }, Qt::QueuedConnection);
    });
    topic_->on_data([this, gen](uconnect::DevId dev, std::span<const uint8_t> bytes) {
        std::vector<uint8_t> copy(bytes.begin(), bytes.end());
        QMetaObject::invokeMethod(this, [this, gen, dev, copy = std::move(copy)] {
            if (gen == generation_) onData(dev, copy);
        }, Qt::QueuedConnection);
    });

    log(tr("Started; local TCP port %1, topic %2")
            .arg(node_->local_port())
            .arg(QString::fromStdString(uconnect::to_hex(creds->id)).left(8)));
    setStatus(Status::Connecting, tr("Registering with %1...").arg(server));

    pollTimer_->start();
    snapshotTimer_->start();
    poll();
}

void Session::stop() {
    pollTimer_->stop();
    snapshotTimer_->stop();
    ++generation_;
    storage_->reset();

    if (node_) {
        try {
            node_->shutdown();
        } catch (const std::exception& e) {
            log(tr("Shutdown error: %1").arg(QString::fromUtf8(e.what())));
        }
        node_.reset();
        log(tr("Stopped"));
    }
    topic_     = nullptr;
    self_.reset();
    published_ = false;
    peers_.clear();
    userDisconnected_.clear();

    emit peersChanged({});
    emit snapshotChanged({});
    if (status_ != Status::Error) setStatus(Status::Offline);
}

void Session::setIdentity(const Identity& identity) {
    identity_ = identity;
}

void Session::setAccessPolicy(const AccessPolicy& policy) {
    policy_ = policy;
    if (topic_)
        for (const uconnect::DevId& dev : topic_->connected()) sendGrants(dev);
    emitPeers();
}

void Session::setLocalHello(const Hello& hello) {
    storage_->setOffered(hello.services & ServiceStorage);
    helloMessage_ = hello.encode();
    if (topic_) topic_->broadcast(helloMessage_);
}

void Session::connectPeer(const QString& devId) {
    const auto dev = parseDevId(devId);
    if (!topic_ || !dev) return;
    userDisconnected_.erase(*dev);
    peers_[*dev].lastAttempt = Clock::now();
    peers_[*dev].gone.reset();
    topic_->connect(*dev);
    log(tr("Connecting to %1").arg(shortId(devId)));
    emitPeers();
}

void Session::disconnectPeer(const QString& devId) {
    const auto dev = parseDevId(devId);
    if (!topic_ || !dev) return;
    userDisconnected_.insert(*dev);
    topic_->disconnect(*dev);
    log(tr("Disconnecting from %1").arg(shortId(devId)));
    emitPeers();
}

void Session::refreshNow() {
    if (topic_) poll();
}

void Session::setSharedFolders(const QStringList& folders) {
    storage_->setSharedFolders(folders);
}

void Session::storageList(const QString& devId, const QString& path, quint32 requestId) {
    const auto dev = parseDevId(devId);
    if (!topic_ || !dev) {
        emit storage_->listFinished(requestId, path, {}, tr("Not connected."));
        return;
    }
    storage_->list(*dev, path, requestId);
}

void Session::storageDownload(const QString& devId, const QString& remotePath, const QString& folder,
                              quint32 requestId) {
    const auto dev = parseDevId(devId);
    if (!topic_ || !dev) {
        emit storage_->downloadFinished(requestId, {}, tr("Not connected."));
        return;
    }
    storage_->download(*dev, remotePath, folder, requestId);
}

void Session::storageCancel(quint32 requestId) {
    storage_->cancel(requestId);
}

void Session::requestServerStats() {
    if (!node_) {
        emit serverReport(tr("Not running."));
        return;
    }
    const auto s = node_->stats(2s);
    if (!s) {
        emit serverReport(tr("No reply from the server."));
        return;
    }
    emit serverReport(QStringLiteral(
        "connections=%1  topics=%2 (listed %3)\n"
        "records=%4 (fresh %5)  expired=%6\n"
        "registers=%7  lookups=%8  connects=%9\n"
        "relays open=%10 allocated=%11 bytes=%12\n"
        "rejected: quota=%13 rate=%14")
        .arg(s->connections).arg(s->topics_total).arg(s->topics_listed)
        .arg(s->entries_total).arg(s->entries_fresh).arg(s->expired)
        .arg(s->registers).arg(s->lookups).arg(s->connects)
        .arg(s->relays_open).arg(s->relays_allocated).arg(s->relay_bytes)
        .arg(s->rej_quota).arg(s->rej_rate_limited));
}

void Session::requestTopicListing() {
    if (!node_) {
        emit serverReport(tr("Not running."));
        return;
    }
    const auto list = node_->try_explore(0, 50, 3s);
    if (!list) {
        emit serverReport(tr("Listing failed (no reply from the server)."));
        return;
    }
    QString text = tr("%1 listed topic(s)\n").arg(list->size());
    for (const auto& t : *list) {
        text += QStringLiteral("%1  %2  peers=%3 fresh=%4\n")
                    .arg(QString::fromStdString(uconnect::to_hex(t.id)),
                         t.mode == uconnect::TopicMode::Keyed ? QStringLiteral("keyed")
                                                              : QStringLiteral("open"))
                    .arg(t.peers)
                    .arg(t.fresh_peers);
    }
    emit serverReport(text);
}

void Session::poll() {
    if (!topic_) return;

    if (!published_) {
        // Unlisted: a private device group has no business in the public
        // directory. The meta blob stays empty because the server stores it in
        // plaintext; names and services travel in the encrypted hello instead.
        published_ = topic_->publish({}, /*unlisted=*/true);
        if (!published_) {
            setStatus(Status::Connecting, tr("Waiting for the rendezvous server..."));
            return;
        }
        self_ = topic_->self();
        log(tr("Published as %1%2")
                .arg(self_ ? shortId(hex(*self_)) : QStringLiteral("?"))
                .arg(node_->reflexive()
                         ? tr(" (server sees %1)").arg(formatEndpoint(*node_->reflexive()))
                         : QString()));
    }

    const auto list = topic_->try_peers(kLookupMax, /*want_meta=*/false, kLookupTimeout);
    if (list) {
        for (auto& [dev, p] : peers_) p.inLookup = false;
        for (const auto& info : *list) {
            if (isSelf(info.dev_id)) continue;
            auto& p    = peers_[info.dev_id];
            p.inLookup = true;
            p.stale    = info.stale;
        }
        std::erase_if(peers_, [this](const auto& kv) {
            return !kv.second.inLookup && !isActive(topic_->state(kv.first));
        });

        const auto now = Clock::now();
        for (auto& [dev, p] : peers_) {
            if (!p.inLookup || p.stale || userDisconnected_.contains(dev)) continue;
            const auto st = topic_->state(dev);
            // A peer that said goodbye on purpose is left alone; one that
            // vanished or could not be reached is retried, but not hammered.
            const bool deliberate = p.gone && *p.gone != uconnect::PeerGone::TimedOut;
            const bool retry = (st == uconnect::PeerState::Failed ||
                                (st == uconnect::PeerState::Closed && !deliberate)) &&
                               now - p.lastAttempt > kRetryAfter;
            if (st == uconnect::PeerState::Unknown || retry) {
                p.lastAttempt = now;
                topic_->connect(dev);
            }
        }
    }

    emitPeers();
    updateStatus();
}

void Session::emitSnapshot() {
    NodeSnapshot s;
    if (node_) {
        s.running            = node_->is_running();
        s.serverConnected    = node_->server_connected();
        s.localPort          = node_->local_port();
        s.pendingRequests    = node_->pending_requests();
        s.pendingConnections = node_->pending_connections();
        if (const auto r = node_->reflexive()) s.reflexive = formatEndpoint(*r);
    }
    if (topic_) {
        s.published       = published_;
        s.authenticated   = topic_->is_authenticated();
        s.topicId         = QString::fromStdString(uconnect::to_hex(topic_->id()));
        s.rememberedPeers = topic_->remembered_peers();
        if (self_) s.selfId = hex(*self_);
    }
    emit snapshotChanged(s);
}

void Session::emitPeers() {
    QVector<PeerRecord> out;
    if (topic_) {
        out.reserve(static_cast<qsizetype>(peers_.size()));
        for (const auto& [dev, p] : peers_) {
            PeerRecord r;
            const auto st = topic_->state(dev);
            r.devId     = hex(dev);
            r.stateCode = static_cast<int>(st);
            r.state     = QString::fromLatin1(uconnect::to_string(st));
            r.stale     = p.stale;
            if (st == uconnect::PeerState::Closed && p.gone)
                r.closeReason = QString::fromLatin1(uconnect::to_string(*p.gone));
            if (p.hello) {
                r.hasHello = true;
                r.name     = p.hello->name;
                r.os       = p.hello->os;
                r.services = p.hello->services;
            }
            if (const auto link = topic_->link(dev)) {
                r.relayed          = link->relayed;
                r.messagesSent     = link->messages_sent;
                r.messagesReceived = link->messages_received;
                r.bytesSent        = link->bytes_sent;
                r.bytesReceived    = link->bytes_received;
            }
            if (p.identity) {
                r.identityState = QStringLiteral("verified");
                r.identityKey   = keyToHex(*p.identity);
                r.fingerprint   = fingerprint(*p.identity);
                r.trusted       = policy_.trusted.contains(r.identityKey);
            } else {
                r.identityState = p.identityInvalid ? QStringLiteral("invalid") : QStringLiteral("pending");
            }
            r.permissions = permissionsFor(dev).values();
            r.permissions.sort();
            r.grantedToMe = p.grantedToMe;
            out.push_back(std::move(r));
        }
    }
    emit peersChanged(out);
}

void Session::updateStatus() {
    if (!node_) return;
    if (!published_) return;  // poll() owns the Connecting message
    if (node_->server_connected())
        setStatus(Status::Online, tr("Connected to the rendezvous server"));
    else
        setStatus(Status::Reconnecting, tr("Lost the rendezvous server; retrying..."));
}

void Session::setStatus(Status status, const QString& detail) {
    if (status == status_ && detail == statusDetail_) return;
    status_       = status;
    statusDetail_ = detail;
    if (!detail.isEmpty()) log(detail);
    emit statusChanged(status, detail);
}

void Session::log(const QString& line) {
    emit logMessage(line);
}

void Session::onPeerState(const uconnect::DevId& dev, uconnect::PeerState state) {
    if (!topic_ || isSelf(dev)) return;
    auto& p = peers_[dev];
    log(tr("Peer %1 -> %2").arg(shortId(hex(dev)), QString::fromLatin1(uconnect::to_string(state))));

    if (state == uconnect::PeerState::Connected) {
        p.gone.reset();
        p.identity.reset();
        p.identityInvalid = false;
        p.grantedToMe.clear();
        if (!helloMessage_.empty()) topic_->send(dev, helloMessage_);
        sendIdentity(dev);
        sendGrants(dev);
    } else if (state == uconnect::PeerState::Closed || state == uconnect::PeerState::Failed) {
        storage_->peerGone(dev);
        p.identity.reset();
        p.identityInvalid = false;
        p.grantedToMe.clear();
    }
    emitPeers();
}

void Session::onPeerClosed(const uconnect::DevId& dev, uconnect::PeerGone why) {
    if (!topic_ || isSelf(dev)) return;
    auto it = peers_.find(dev);
    if (it != peers_.end()) it->second.gone = why;
    log(tr("Peer %1 closed: %2").arg(shortId(hex(dev)), QString::fromLatin1(uconnect::to_string(why))));
    emitPeers();
}

void Session::onData(const uconnect::DevId& dev, const std::vector<uint8_t>& data) {
    if (!topic_) return;
    if (auto hello = Hello::decode(data)) {
        const bool first = !peers_[dev].hello;
        peers_[dev].hello = std::move(*hello);
        if (first) {
            const auto& h = *peers_[dev].hello;
            log(tr("Peer %1 is \"%2\" (%3) offering [%4]")
                    .arg(shortId(hex(dev)), h.name, h.os, serviceNames(h.services).join(", ")));
        }
        emitPeers();
        return;
    }
    if (const auto msg = decodeControl(data)) {
        const QString t = msg->value("t").toString();
        if (t == QLatin1String("id")) return onIdentity(dev, *msg);
        if (t == QLatin1String("perm.granted")) return onGrants(dev, *msg);
    }
    if (storage_->handleMessage(dev, data)) return;
    log(tr("Peer %1 sent %2 bytes of an unknown message kind")
            .arg(shortId(hex(dev)))
            .arg(data.size()));
}

bool Session::isSelf(const uconnect::DevId& dev) const {
    return self_ && *self_ == dev;
}

void Session::sendIdentity(const uconnect::DevId& dev) {
    if (!identity_ || !topic_) return;
    if (const auto binding = topic_->channel_binding(dev)) topic_->send(dev, identity_->proof(*binding));
}

// Tells a peer what it may do here, so its UI can say so up front. Purely
// informative: every request is still checked when it arrives.
void Session::sendGrants(const uconnect::DevId& dev) {
    if (!topic_) return;
    QStringList granted = permissionsFor(dev).values();
    granted.sort();
    topic_->send(dev, encodeControl({{"t", "perm.granted"}, {"perms", QJsonArray::fromStringList(granted)}}));
}

void Session::onIdentity(const uconnect::DevId& dev, const QJsonObject& message) {
    auto& p = peers_[dev];
    if (p.identity || p.identityInvalid) return;  // one proof per session

    const auto binding = topic_->channel_binding(dev);
    if (const auto key = binding ? verifyProof(message, *binding) : std::nullopt) {
        p.identity = key;
        log(tr("Peer %1 proved identity %2").arg(shortId(hex(dev)), fingerprint(*key)));
    } else {
        p.identityInvalid = true;
        log(tr("Peer %1 sent an identity proof that does not verify").arg(shortId(hex(dev))));
    }
    sendGrants(dev);  // what it may do depends on who it turned out to be
    emitPeers();
}

void Session::onGrants(const uconnect::DevId& dev, const QJsonObject& message) {
    QStringList granted;
    for (const QJsonValue& v : message.value("perms").toArray()) {
        const QString id = v.toString();
        if (isKnownPermission(id)) granted << id;
    }
    peers_[dev].grantedToMe = granted;
    emitPeers();
}

PermissionSet Session::permissionsFor(const uconnect::DevId& dev) const {
    const auto it = peers_.find(dev);
    if (it != peers_.end() && it->second.identity) {
        const auto trusted = policy_.trusted.constFind(keyToHex(*it->second.identity));
        if (trusted != policy_.trusted.constEnd()) return trusted.value();
    }
    return policy_.defaults;
}

}  // namespace ucd
