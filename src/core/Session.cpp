#include "Session.hpp"

#include <algorithm>
#include <exception>

#include <QDateTime>
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
    : QObject(parent), pollTimer_(new QTimer(this)), snapshotTimer_(new QTimer(this)),
      media_([this](const uconnect::DevId& dev, uint8_t kind, std::span<const uint8_t> bytes) {
          emit mediaDatagram(hex(dev), kind, QByteArray(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
#ifdef UCD_HAS_SCREEN
          const auto packet = QByteArray(reinterpret_cast<const char*>(bytes.data()), bytes.size());
          if (kind == 3 && camera_) camera_->receive(dev, packet);
          else if ((kind == 4 || kind == 5) && display_) display_->receive(dev, packet);
          else if (screen_) screen_->receive(dev, packet);
#endif
      }) {
#ifdef UCD_HAS_SCREEN
    const auto sendControl = [this](const auto& peer, const QJsonObject& msg) {
        const auto data = encodeControl(msg); return topic_ && topic_->send(peer, data);
    };
    const auto access = [this](const auto& peer) { return accessFor(peer); };
    const auto sendDatagram = [this](const auto& peer, const QByteArray& bytes) {
        return sendMedia(peer, {reinterpret_cast<const uint8_t*>(bytes.constData()), size_t(bytes.size())});
    };
    const auto close = [this](const auto& peer) { closeMedia(peer); };
    screen_ = new screen::ScreenService(sendControl, access,
        [this](const auto& peer) { return openMedia(peer, mediaFallback()); },
        close, sendDatagram, this);
    camera_ = new screen::ScreenService(sendControl, access,
        [this](const auto& peer) { return openMedia(peer, mediaFallback()); },
        close, sendDatagram, this, {}, {}, screen::Kind::Camera);
    display_ = new screen::ScreenService(sendControl, access,
        [this](const auto& peer) { return openMedia(peer, uconnect::DatagramFallback::None); },
        close, sendDatagram, this, {}, {}, screen::Kind::Display);
    displays_ = new display::DisplayService(sendControl, access, display_, display::create, display::status, this);
    connect(screen_, &screen::ScreenService::activity, this, &Session::logMessage);
    connect(camera_, &screen::ScreenService::activity, this, &Session::logMessage);
    connect(display_, &screen::ScreenService::activity, this, &Session::logMessage);
    connect(displays_, &display::DisplayService::activity, this, &Session::logMessage);
    connect(this, &Session::mediaPathChanged, this, [this](const QString& id, const QString& path) {
        if (const auto peer = parseDevId(id)) {
            screen_->pathChanged(*peer, path); camera_->pathChanged(*peer, path); display_->pathChanged(*peer, path);
        }
    });
#endif
    storage_ = new StorageService(
        [this](const uconnect::DevId& dev, std::span<const uint8_t> bytes) {
            return topic_ && topic_->send(dev, bytes);
        },
        [this](const uconnect::DevId& dev, const QString& permission) {
            return permissionsFor(dev).contains(permission);
        },
        this);
    connect(storage_, &StorageService::activity, this,
            [this](const uconnect::DevId& dev, const QString& action, const QString& path, const QString& detail) {
                const auto it   = peers_.find(dev);
                const bool have = it != peers_.end();
                const QString name = have && it->second.hello ? it->second.hello->name : shortId(hex(dev));
                const QString key  = have && it->second.identity ? keyToHex(*it->second.identity) : QString();
                emit storageActivity(hex(dev), name, key, action, path, detail);
            });
    // Sync is only for peers that proved who they are; each folder then names
    // its members (by key) and what they may send and receive.
    sync_ = new SyncService(
        [this](const uconnect::DevId& dev, std::span<const uint8_t> bytes) {
            return topic_ && topic_->send(dev, bytes);
        },
        [this](const uconnect::DevId& dev) {
            const auto it = peers_.find(dev);
            if (it == peers_.end() || !it->second.identity) return QString();
            return keyToHex(*it->second.identity);
        },
        [this](const uconnect::DevId& dev) {
            const auto it = peers_.find(dev);
            return it == peers_.end() ? qint64(-1) : it->second.connectedAt;
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
        cfg.force_relay = options.forceRelay || options.relayMode == 1;
        relayMode_ = options.relayMode;
        node_       = std::make_unique<uconnect::Node>(cfg);
        node_->run_in_background();
        topic_ = &node_->join(*creds);
        media_.attach(topic_);
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
    topic_->on_datagram([this, gen](uconnect::DevId dev, std::span<const uint8_t> bytes) {
        if (mediaPending_.fetch_add(1) >= 512) { --mediaPending_; return; }
        const QByteArray copy(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        QMetaObject::invokeMethod(this, [this, gen, dev, copy] {
            --mediaPending_;
            if (gen == generation_) media_.receive(dev,
                {reinterpret_cast<const uint8_t*>(copy.constData()), size_t(copy.size())});
        }, Qt::QueuedConnection);
    });
    topic_->on_datagram_path([this, gen](uconnect::DevId dev, uconnect::DatagramPath path) {
        QMetaObject::invokeMethod(this, [this, gen, dev, path] {
            if (gen != generation_) return;
            log(tr("Media path to %1: %2").arg(shortId(hex(dev)), QString::fromLatin1(uconnect::to_string(path))));
            if (media_.active(dev))
                emit mediaPathChanged(hex(dev), QString::fromLatin1(uconnect::to_string(path)));
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
#ifdef UCD_HAS_SCREEN
    displays_->reset();  // first: it removes the virtual monitors
    screen_->reset(); camera_->reset(); display_->reset();
#endif
    media_.attach(nullptr); mediaUsers_.clear();
    storage_->reset();
    sync_->reset();

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

bool Session::openMedia(const uconnect::DevId& peer, uconnect::DatagramFallback fallback) {
    // A proven identity is enough here: whether this device may share with
    // the peer (which needs special or network rules) is the screen service's decision.
    const auto it = peers_.find(peer);
    if (it == peers_.end() || !it->second.identity) return false;
    if (!mediaUsers_.contains(peer) && !media_.open(peer, fallback)) return false;
    ++mediaUsers_[peer];
    emit mediaPathChanged(hex(peer), QString::fromLatin1(uconnect::to_string(media_.path(peer))));
    return true;
}

void Session::closeMedia(const uconnect::DevId& peer) {
    const auto it = mediaUsers_.find(peer); if (it == mediaUsers_.end()) return;
    if (--it->second == 0) { media_.close(peer); mediaUsers_.erase(it); }
}
bool Session::sendMedia(const uconnect::DevId& peer, std::span<const uint8_t> bytes) { return media_.send(peer, bytes); }

void Session::configureSync(const QString& keyHex, const QString& stateDir) {
    sync_->setIdentity(keyHex, stateDir);
}

void Session::setSyncFolders(const QVector<ucd::sync::FolderConfig>& folders) {
    sync_->setFolders(folders);
}

void Session::setAccessPolicy(const AccessPolicy& policy) {
    policy_ = policy;
#ifdef UCD_HAS_SCREEN
    screen_->policyChanged(); camera_->policyChanged(); display_->policyChanged(); displays_->policyChanged();
#endif
    if (topic_)
        for (const uconnect::DevId& dev : topic_->connected()) {
            sendGrants(dev);
            sync_->peerUp(dev);  // its rules may have changed
        }
    emitPeers();
}

void Session::setLocalHello(const Hello& hello) {
    storage_->setOffered(hello.services & ServiceStorage);
#ifdef UCD_HAS_SCREEN
    screen_->setOffered(hello.services & (ServiceScreen | ServiceControl));
    screen_->setControlOffered(hello.services & ServiceControl);
    camera_->setOffered(hello.services & ServiceCamera);
    displays_->setOffered(hello.services & ServiceDisplay);
#endif
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

void Session::setSharedFolders(const QStringList& folders, const QStringList& readOnly) {
    storage_->setSharedFolders(folders, readOnly);
}

void Session::setUploadLimit(qint64 bytes) {
    storage_->setUploadLimit(bytes);
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

void Session::storageUpload(const QString& devId, const QString& localPath, const QString& remoteFolder,
                            quint32 requestId, bool replace) {
    const auto dev = parseDevId(devId);
    if (!topic_ || !dev) {
        emit storage_->uploadFinished(requestId, {}, tr("Not connected."));
        return;
    }
    storage_->upload(*dev, localPath, remoteFolder, requestId, replace);
}

void Session::storageOperation(const QString& devId, const QJsonObject& request, quint32 requestId) {
    const auto dev = parseDevId(devId);
    if (!topic_ || !dev) {
        emit storage_->operationFinished(requestId, {}, tr("Not connected."));
        return;
    }
    storage_->operation(*dev, request, requestId);
}

void Session::storageWatch(const QString& devId, const QString& path, quint32 requestId) {
    const auto dev = parseDevId(devId);
    if (topic_ && dev) storage_->watch(*dev, path, requestId);
}

void Session::storageUnwatch(quint32 requestId) {
    storage_->unwatch(requestId);
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
                r.special       = policy_.special.contains(r.identityKey);
            } else {
                r.identityState = p.identityInvalid ? QStringLiteral("invalid") : QStringLiteral("pending");
            }
            if (const QHostAddress a = peerAddress(dev); !a.isNull()) r.address = a.toString();
            r.networkRule = networkRuleFor(dev);
            const auto [perms, source] = resolvePermissions(dev);
            r.permissions      = perms.values();
            r.permissionSource = source;
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
        p.connectedAt = QDateTime::currentMSecsSinceEpoch();
        p.gone.reset();
        p.identity.reset();
        p.identityInvalid = false;
        p.grantedToMe.clear();
        if (!helloMessage_.empty()) topic_->send(dev, helloMessage_);
        sendIdentity(dev);
        sendGrants(dev);
    } else if (state == uconnect::PeerState::Closed || state == uconnect::PeerState::Failed) {
        storage_->peerGone(dev);
        sync_->peerGone(dev);
#ifdef UCD_HAS_SCREEN
        displays_->peerGone(dev);
        screen_->peerGone(dev); camera_->peerGone(dev); display_->peerGone(dev);
#endif
        media_.close(dev); mediaUsers_.erase(dev);
        p.connectedAt = -1;
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
        if (t == QLatin1String("rules.get")) {
            // Only a proven device with special rules here, or one a network
            // rule covers, may copy them -- not one on the defaults alone.
            const bool allowed = resolvePermissions(dev).second != QLatin1String("default");
            topic_->send(dev, encodeControl(allowed
                ? QJsonObject{{"t", "rules.list"}, {"rules", networkRulesToJson(policy_.rules)}}
                : QJsonObject{{"t", "rules.err"}, {"code", "forbidden"}}));
            log(tr("Peer %1 asked for the network rules: %2")
                    .arg(shortId(hex(dev)), allowed ? tr("sent") : tr("refused: it has no rules here")));
            return;
        }
        if (t == QLatin1String("rules.list")) {
            emit networkRulesReceived(hex(dev), msg->value("rules").toArray(), {});
            return;
        }
        if (t == QLatin1String("rules.err")) {
            emit networkRulesReceived(hex(dev), {}, msg->value("code").toString());
            return;
        }
#ifdef UCD_HAS_SCREEN
        // The display handshake before the media sessions' messages.
        if (displays_->handle(dev, *msg) || screen_->handle(dev, *msg) || camera_->handle(dev, *msg) ||
            display_->handle(dev, *msg)) return;
#endif
    }
    if (sync_->handleMessage(dev, data)) return;
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
        sync_->peerUp(dev);
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

PermissionSet Session::permissionsFor(const uconnect::DevId& dev) const { return resolvePermissions(dev).first; }

std::pair<PermissionSet, QString> Session::resolvePermissions(const uconnect::DevId& dev) const {
    const auto it = peers_.find(dev);
    if (it != peers_.end() && it->second.identity) {
        const auto special = policy_.special.constFind(keyToHex(*it->second.identity));
        if (special != policy_.special.constEnd()) return {special.value(), QStringLiteral("device")};
        if (const int rule = networkRuleFor(dev); rule >= 0) {
            const QStringList& list = policy_.rules[rule].permissions;
            return {PermissionSet(list.begin(), list.end()), QStringLiteral("rule")};
        }
    }
    return {permissionsForUntrusted(policy_.defaults), QStringLiteral("default")};
}

QHostAddress Session::peerAddress(const uconnect::DevId& dev) const {
    if (!topic_) return {};
    const auto ep = topic_->peer_address(dev);
    if (!ep) return {};
    if (ep->ip.family == uconnect::IpAddr::Family::V4)
        return QHostAddress(quint32(ep->ip.bytes[0]) << 24 | quint32(ep->ip.bytes[1]) << 16 |
                            quint32(ep->ip.bytes[2]) << 8 | quint32(ep->ip.bytes[3]));
    Q_IPV6ADDR v6;
    std::copy(ep->ip.bytes.begin(), ep->ip.bytes.end(), v6.c);
    return QHostAddress(v6);
}

int Session::networkRuleFor(const uconnect::DevId& dev) const {
    if (policy_.rules.isEmpty()) return -1;
    const QHostAddress a = peerAddress(dev);
    return a.isNull() ? -1 : firstMatchingRule(policy_.rules, a, policy_.localAddresses);
}

void Session::requestNetworkRules(const QString& devId) {
    const auto dev = uconnect::from_hex<uconnect::kDevIdLen>(devId.toStdString());
    if (!dev || !topic_) return;
    topic_->send(*dev, encodeControl({{"t", "rules.get"}}));
}

#ifdef UCD_HAS_SCREEN
screen::PeerAccess Session::accessFor(const uconnect::DevId& peer) const {
    screen::PeerAccess access;
    const auto it = peers_.find(peer);
    if (it != peers_.end() && it->second.identity) {
        access.key = keyToHex(*it->second.identity);
        access.fingerprint = fingerprint(*it->second.identity);
        access.name = it->second.hello ? it->second.hello->name : shortId(hex(peer));
        access.verified = true;
        const auto [perms, source] = resolvePermissions(peer);
        // Sharing needs more than the defaults: special or network rules.
        access.trusted = source != QLatin1String("default");
        access.permissions = perms;
    }
    return access;
}
#endif

}  // namespace ucd
