#pragma once
// Owns the uconnect Node for one topic. A Session lives on its own worker
// thread, and every public method must be called on that thread (post to it
// with QMetaObject::invokeMethod). That rule exists because the library's
// synchronous calls -- publish(), try_peers(), stats() -- block for up to
// their timeout, and are rejected outright on the library's own loop thread.
//
// The library's callbacks arrive on its loop thread; they are re-posted onto
// the worker thread, so all of this class's state is touched by one thread.

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include "core/Hello.hpp"
#include "core/Identity.hpp"
#include "core/Permissions.hpp"
#include "core/StorageService.hpp"
#include "core/sync/SyncService.hpp"
#include "core/screen/DatagramChannel.hpp"
#ifdef UCD_HAS_SCREEN
#include "core/screen/ScreenService.hpp"
#include "core/display/DisplayService.hpp"
#endif
#include <atomic>
#include <QByteArray>
#include "uconnect/uconnect.hpp"

class QTimer;

namespace ucd {

struct PeerRecord {
    QString  devId;  // 32 hex chars
    QString  state;
    int      stateCode = 0;  // uconnect::PeerState
    QString  closeReason;    // set while Closed
    bool     stale    = false;
    bool     relayed  = false;
    bool     hasHello = false;
    QString  name;
    QString  os;
    uint32_t services = ServiceNone;

    quint64 messagesSent     = 0;
    quint64 messagesReceived = 0;
    quint64 bytesSent        = 0;
    quint64 bytesReceived    = 0;

    // Identity, as proven in this session.
    QString     identityState;  // "pending" | "verified" | "invalid"
    QString     identityKey;    // hex public key, once verified
    QString     fingerprint;
    bool        trusted = false;
    QStringList permissions;  // what this device grants it
    QStringList grantedToMe;  // what it says it grants this device

    bool operator==(const PeerRecord&) const = default;
};

struct NodeSnapshot {
    bool    running         = false;
    bool    serverConnected = false;
    bool    published       = false;
    bool    authenticated   = false;
    quint16 localPort       = 0;
    QString reflexive;
    QString selfId;
    QString topicId;
    quint64 pendingRequests    = 0;
    quint64 pendingConnections = 0;
    quint64 rememberedPeers    = 0;
};

// What this device lets others do. Owned by the GUI side, which pushes a copy
// to the Session whenever it changes.
struct AccessPolicy {
    PermissionSet                 defaults;  // every device that is not trusted
    QHash<QString, PermissionSet> trusted;   // public key (hex) -> its own permissions
};

class Session : public QObject {
    Q_OBJECT
public:
    enum class Status { Offline, Connecting, Online, Reconnecting, Error };
    Q_ENUM(Status)

    struct Options {
        QString server;
        QString topicLink;
        bool    verbose = false;
        // Skip punching and use the fallbacks at once (the server's relay for
        // connections, TCP for datagrams): for tests, and for networks where
        // punching is known to fail. Set by --force-relay for this run.
        bool    forceRelay = false;
        // Persisted relay policy: 0 auto, 1 always (same as forceRelay),
        // 2 direct (media never relays). See RelayMode in AppController.
        int     relayMode = 0;
    };

    explicit Session(QObject* parent = nullptr);
    ~Session() override;

    void start(const Options& options);
    void stop();
    void setLocalHello(const Hello& hello);
    void setIdentity(const Identity& identity);
    void setAccessPolicy(const AccessPolicy& policy);
    void connectPeer(const QString& devId);
    void disconnectPeer(const QString& devId);
    void refreshNow();
    void requestServerStats();
    void requestTopicListing();
    // Datagram fallback: TCP for screen and camera; extended displays use
    // none, since they only run between nearby devices.
    bool openMedia(const uconnect::DevId& peer, uconnect::DatagramFallback fallback = uconnect::DatagramFallback::Tcp);
    void closeMedia(const uconnect::DevId& peer);
    bool sendMedia(const uconnect::DevId& peer, std::span<const uint8_t> bytes);
#ifdef UCD_HAS_SCREEN
    screen::ScreenService* screen() const { return screen_; }
    screen::ScreenService* camera() const { return camera_; }
    // Extended displays: the media session, and the handshake and monitors.
    screen::ScreenService* display() const { return display_; }
    display::DisplayService* displays() const { return displays_; }
#endif

    // Storage: this device's shared folders, and requests to other devices'.
    void setSharedFolders(const QStringList& folders, const QStringList& readOnly = {});
    void setUploadLimit(qint64 bytes);
    void storageList(const QString& devId, const QString& path, quint32 requestId);
    void storageDownload(const QString& devId, const QString& remotePath, const QString& folder, quint32 requestId);
    void storageUpload(const QString& devId, const QString& localPath, const QString& remoteFolder, quint32 requestId,
                       bool replace = false);
    // fs.rename, fs.delete or fs.mkdir, without "id".
    void storageOperation(const QString& devId, const QJsonObject& request, quint32 requestId);
    void storageCancel(quint32 requestId);
    void storageWatch(const QString& devId, const QString& path, quint32 requestId);
    void storageUnwatch(quint32 requestId);
    StorageService* storage() const { return storage_; }
    SyncService*    sync() const { return sync_; }
    // This device's identity key and where sync keeps its state; before
    // setSyncFolders().
    void configureSync(const QString& keyHex, const QString& stateDir);
    void setSyncFolders(const QVector<ucd::sync::FolderConfig>& folders);

signals:
    void mediaDatagram(const QString& devId, quint8 kind, const QByteArray& bytes);
    void mediaPathChanged(const QString& devId, const QString& path);
    void statusChanged(ucd::Session::Status status, const QString& detail);
    void peersChanged(const QVector<ucd::PeerRecord>& peers);
    void snapshotChanged(const ucd::NodeSnapshot& snapshot);
    void logMessage(const QString& line);
    void serverReport(const QString& text);
    // Another device changed or took something in a shared folder; see
    // StorageService::activity(). `identityKey` is empty if unproven.
    void storageActivity(const QString& devId, const QString& deviceName, const QString& identityKey,
                         const QString& action, const QString& path, const QString& detail);

private:
    using Clock = std::chrono::steady_clock;

    struct Peer {
        bool                         inLookup = false;
        bool                         stale    = false;
        std::optional<Hello>         hello;
        std::optional<uconnect::PeerGone> gone;
        Clock::time_point            lastAttempt{};

        // Reset with every new session: the proof is bound to it.
        std::optional<PublicKey> identity;
        qint64                   connectedAt = -1;  // ms since epoch, while connected
        bool                     identityInvalid = false;
        QStringList              grantedToMe;
    };

    void poll();
    void emitSnapshot();
    void emitPeers();
    void updateStatus();
    void setStatus(Status status, const QString& detail = {});
    void log(const QString& line);

    void onPeerState(const uconnect::DevId& dev, uconnect::PeerState state);
    void onPeerClosed(const uconnect::DevId& dev, uconnect::PeerGone why);
    void onData(const uconnect::DevId& dev, const std::vector<uint8_t>& data);

    bool isSelf(const uconnect::DevId& dev) const;

    void          sendIdentity(const uconnect::DevId& dev);
    void          sendGrants(const uconnect::DevId& dev);
    void          onIdentity(const uconnect::DevId& dev, const QJsonObject& message);
    void          onGrants(const uconnect::DevId& dev, const QJsonObject& message);
    PermissionSet permissionsFor(const uconnect::DevId& dev) const;
#ifdef UCD_HAS_SCREEN
    screen::PeerAccess accessFor(const uconnect::DevId& dev) const;
#endif

    std::unique_ptr<uconnect::Node> node_;
    uconnect::Topic*                topic_ = nullptr;
    std::optional<uconnect::DevId>  self_;
    bool                            published_ = false;

    // The relay policy this session started with (0 auto, 1 always, 2 direct);
    // decides the datagram fallback screen and camera media open with.
    int relayMode_ = 0;
    uconnect::DatagramFallback mediaFallback() const {
        return relayMode_ == 2 ? uconnect::DatagramFallback::None
             : relayMode_ == 1 ? uconnect::DatagramFallback::Relay
                               : uconnect::DatagramFallback::Tcp;
    }

    // Bumped by every start() and stop(), so callbacks queued by a Node that
    // has since been torn down are recognised and dropped.
    quint64 generation_ = 0;

    Status  status_ = Status::Offline;
    QString statusDetail_;

    std::vector<uint8_t>                 helloMessage_;
    std::optional<Identity>              identity_;
    AccessPolicy                         policy_;
    std::map<uconnect::DevId, Peer>      peers_;
    std::set<uconnect::DevId>            userDisconnected_;

    QTimer* pollTimer_     = nullptr;
    QTimer* snapshotTimer_ = nullptr;

    StorageService* storage_ = nullptr;
    SyncService*    sync_    = nullptr;
    screen::DatagramChannel media_;
    std::map<uconnect::DevId, unsigned> mediaUsers_;
    std::atomic_uint mediaPending_{0};  // bound loop-to-worker queues under overload
#ifdef UCD_HAS_SCREEN
    screen::ScreenService* screen_ = nullptr;
    screen::ScreenService* camera_ = nullptr;
    screen::ScreenService* display_ = nullptr;
    display::DisplayService* displays_ = nullptr;
#endif
};

QString shortId(const QString& devId);

}  // namespace ucd

Q_DECLARE_METATYPE(ucd::PeerRecord)
Q_DECLARE_METATYPE(ucd::NodeSnapshot)
