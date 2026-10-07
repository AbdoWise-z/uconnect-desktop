#pragma once
// Folder sync: folders kept the same on every member, peer to peer, with no
// device in charge. Members exchange indexes (core/sync/SyncFolder.hpp),
// fetch the blocks they lack from any member that has them, and resolve
// conflicts as described in docs/protocol.md, "Folder sync".
//
// Lives on the Session's worker thread, like StorageService.

#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <vector>

#include <QFile>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

#include "core/sync/SyncFolder.hpp"
#include "uconnect/types.hpp"

class QTimer;

namespace ucd {

class SyncService : public QObject {
    Q_OBJECT
public:
    using SendFn = std::function<bool(const uconnect::DevId&, std::span<const uint8_t>)>;
    // The identity key (hex) of a peer whose identity is proven and whom this
    // device trusts; empty for any other peer, which takes no part in sync.
    using PeerKeyFn = std::function<QString(const uconnect::DevId&)>;
    // When the connection to a peer began, ms since epoch; -1 if none.
    using SinceFn = std::function<qint64(const uconnect::DevId&)>;

    SyncService(SendFn send, PeerKeyFn peerKey, SinceFn since, QObject* parent = nullptr);
    ~SyncService() override;

    // This device's identity key and where folder state is kept. Needed
    // before setFolders().
    void setIdentity(const QString& keyHex, const QString& stateDir);
    // The folders this device takes part in. A folder no longer listed is
    // left: its state is forgotten, its files stay.
    void setFolders(const QVector<sync::FolderConfig>& folders);

    // A peer connected, proved who it is, or its trust changed.
    void peerUp(const uconnect::DevId& dev);
    void peerGone(const uconnect::DevId& dev);
    bool handleMessage(const uconnect::DevId& dev, std::span<const uint8_t> message);
    void reset();

    // `choice`: "mine", "theirs" (with the holder's key) or "skip" (adds the
    // path to .syncskip).
    void resolve(const QString& folderId, const QString& path, const QString& choice, const QString& key);
    void rescan(const QString& folderId);

    // Tests.
    void setScanInterval(int ms) { scanInterval_ = ms; }
    void setStableAfter(int ms) { stableAfter_ = ms; }
    // Files kept in .synctrash go to the trash (or, without one, are
    // deleted) after this many days.
    void setKeepDays(int days) { keepDays_ = days; }
    void setUseTrash(bool use) { useTrash_ = use; }

signals:
    // The folders a peer offers this device: [{id, label}].
    void offersChanged(const QString& peerKey, const QVariantList& offers);
    // A folder's state: {id, state, error, needFiles, needBytes, files,
    // bytes, conflicts: [...]}; see folderStatus().
    void folderChanged(const QString& folderId, const QVariantMap& status);
    // Something the user should hear about, once. `kind`: "conflict" (theirs
    // to resolve), "following:<key>" (behind both sides, followed that
    // peer's chain), "kept" (stayed on our chain), "waiting" (no holder
    // online), or "blocked:<reason>" with reason outside | in_the_way |
    // changed | not_empty | io.
    void event(const QString& folderId, const QString& path, const QString& kind);

private:
    using Clock = std::chrono::steady_clock;

    struct Pull {
        sync::FileEntry target;
        QStringList     sources;
        QString         temp;
        std::unique_ptr<QFile> file;
        QVector<int>    missing;     // blocks still to fetch
        QSet<int>       requested;
        bool            keepOld = false;  // set the file it replaces aside (a conflict resolved for theirs)
        Clock::time_point lastProgress;
    };
    struct Folder {
        std::unique_ptr<sync::SyncFolder> state;
        QString root;  // real path; empty if missing
        bool    scanning = false;
        bool    rescanSoon = true;
        Clock::time_point nextCleanup;  // of .synctrash
        Clock::time_point nextScan;
        bool    dirty = false;     // state to save
        bool    replan = true;     // decisions to make again
        std::map<QString, Pull> pulls;
        QHash<QString, QString> blocked;  // path -> why it cannot be applied now
        QSet<QString> logged;             // conflicts already told
        QVariantList  conflicts;
        QVariantMap   lastStatus;
        qint64 needFiles = 0, needBytes = 0;
    };
    struct Peer {
        QString key;
        QHash<QString, quint64> sent;      // folder -> our seq the peer has
        QSet<QString>           helloSent;
        QSet<QString>           listening;  // folders it asked for our index of
        QSet<QString>           heard;      // folders whose index it has sent us
        std::deque<std::vector<uint8_t>> queue;
        int inflight = 0;  // block requests to it
    };
    struct Request {
        uconnect::DevId dev{};
        QString    folder;
        QString    path;
        int        block = 0;
        QByteArray hash;
        qint64     expected = 0;
        QByteArray data;
        Clock::time_point deadline;
    };

    Folder* folder(const QString& id);
    sync::Grant grantFor(const QString& folderId, const QString& key) const;
    void openFolder(const sync::FolderConfig& config);
    void sendOffers(const uconnect::DevId& dev);
    // `reply`: answering the peer's hello; a reply is never answered.
    void sendHellos(const uconnect::DevId& dev, bool reply = false);
    void sendIndex(const uconnect::DevId& dev, const QString& folderId, bool full);
    void enqueue(const uconnect::DevId& dev, std::vector<uint8_t> message);
    void pump();
    void work();
    void startScan(const QString& folderId);
    void plan(const QString& folderId);
    void apply(Folder& f, const QString& folderId, const sync::Decision& d);
    void startPull(Folder& f, const QString& folderId, const sync::FileEntry& target, const QStringList& sources);
    void requestBlocks(const QString& folderId);
    void finishPull(const QString& folderId, const QString& path);
    void dropPull(Folder& f, const QString& path);
    void onHello(const uconnect::DevId& dev, const QJsonObject& msg);
    void onIndex(const uconnect::DevId& dev, const QJsonObject& msg);
    void onGet(const uconnect::DevId& dev, const QJsonObject& msg);
    void onData(const uconnect::DevId& dev, std::span<const uint8_t> message);
    void onError(const uconnect::DevId& dev, const QJsonObject& msg);
    void failRequest(quint32 id, bool dropSource);
    QString absolute(const Folder& f, const QString& path) const;
    bool    diskMatches(const Folder& f, const QString& path) const;
    // Moves a file into .synctrash, its path kept and the time added to its name.
    bool    keepAside(const Folder& f, const QString& path) const;
    void    cleanKept(const QString& folderId);
    bool    safeParent(const Folder& f, const QString& path, bool create) const;
    qint64  sinceOf(const QString& key) const;
    QVariantMap folderStatus(const QString& folderId, const Folder& f) const;
    void    emitStatus(const QString& folderId);

    SendFn    send_;
    PeerKeyFn peerKey_;
    SinceFn   since_;
    QString   myKey_;
    QString   me_;
    QString   stateDir_;

    std::map<QString, Folder>          folders_;
    std::map<uconnect::DevId, Peer>    peers_;
    QHash<QString, uconnect::DevId>    devOfKey_;
    std::map<quint32, Request>         requests_;
    quint32                            nextRequest_ = 1;

    QTimer* workTimer_ = nullptr;
    QTimer* pumpTimer_ = nullptr;
    int  scanInterval_ = 10000;
    int  stableAfter_  = 2000;
    bool useTrash_     = true;
    int  keepDays_     = 30;
    Clock::time_point lastSave_;
};

}  // namespace ucd
