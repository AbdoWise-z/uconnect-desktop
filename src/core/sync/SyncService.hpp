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
#include "core/sync/SyncScanner.hpp"
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
    // Whether a peer is told which folders it is a member of (its "Offer
    // sync" permission here); without it the offers it gets are empty.
    using OfferFn = std::function<bool(const uconnect::DevId&)>;

    SyncService(SendFn send, PeerKeyFn peerKey, SinceFn since, QObject* parent = nullptr, OfferFn mayOffer = {});
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

    // --- what the user sees of a folder (docs/protocol.md has the rules) ---

    // Everything in the folder as sync sees it, sorted by path: [{path,
    // name, dir, size, mtime, state, progress, detail, version: {memberId:
    // n}}]. `state`: synced | send (a change of ours some device has not
    // got yet; `detail` how many) | receive (a change to fetch; `detail`
    // "waiting" when no device online has it) | transferring (`progress`
    // 0..1) | conflict | ignored (.syncignore) | skipped (.syncskip; for
    // both, `detail` is the pattern) | held (not syncable here; `detail` the
    // reason, see SyncFolder::heldReason()) | blocked (`detail` the reason,
    // as in the "blocked:" events).
    QVariantList files(const QString& folderId) const;
    // One path in depth: {path, dir, size, mtime, hash, version, state,
    // holders: [{key, size, mtime, version, deleted, dir, skipped}], kept:
    // [{kept, time, size}] (its versions in .synctrash)}.
    QVariantMap fileInfo(const QString& folderId, const QString& path) const;

    // Editing .syncignore (`skipFile` false) and .syncskip (true). The
    // preview says what `text` would do: {problems: [{line, pattern}],
    // matches: [{path, dir, line, pattern, negate, via}] for every known
    // path a rule decides, stop: files synced now that would stop, resume:
    // files left out now that would sync, probe: {line, pattern, negate,
    // via} for `probe`}.
    QString     rulesText(const QString& folderId, bool skipFile) const;
    QVariantMap previewRules(const QString& folderId, bool skipFile, const QString& text,
                             const QString& probe = {}) const;
    bool saveRules(const QString& folderId, bool skipFile, const QString& text);
    // Adds a pattern matching exactly `path` ("Ignore everywhere" / "Skip on this device").
    bool addRule(const QString& folderId, bool skipFile, const QString& path);

    // Files syncing set aside in .synctrash: [{kept (its path there), path
    // (where it was), time, size}], newest first. Restoring puts one back
    // (beside what is there now, if anything), as a new change of ours.
    QVariantList keptFiles(const QString& folderId) const;
    bool restoreKept(const QString& folderId, const QString& kept);
    bool discardKept(const QString& folderId, const QString& kept);

    // Deletes held back by the large-delete guard (folderStatus()
    // `pendingDeletes`): apply them, or keep the files, which makes them
    // changes of ours that the other devices take back.
    void confirmDeletes(const QString& folderId, bool apply);

    // Transfers across every folder: [{folder, path, size, done, key,
    // active}], active ones first, then what waits its turn.
    QVariantList transfers() const;
    // Bytes per second fetched from every peer, over the last seconds.
    qint64 receiveRate() const;

    // Tests.
    void setScanInterval(int ms) { scanInterval_ = ms; }
    void setStableAfter(int ms) { stableAfter_ = ms; }
    // Files kept in .synctrash go to the trash (or, without one, are
    // deleted) after this many days.
    void setKeepDays(int days) { keepDays_ = days; }
    void setUseTrash(bool use) { useTrash_ = use; }
    // The large-delete guard: holds back a round of deletes of at least
    // `min` paths that is also at least `share` of the folder's files, or
    // `many` paths however large the folder. 0 for `min` turns it off.
    void setDeleteGuard(int min, int many, double share) {
        guardMin_   = min;
        guardMany_  = many;
        guardShare_ = share;
    }

signals:
    // The folders a peer offers this device: [{id, label}].
    void offersChanged(const QString& peerKey, const QVariantList& offers);
    // A folder's state: {id, state, files, bytes, dirs, free, needFiles,
    // needBytes, conflicts, blocked, held, members: [{key, online,
    // paused, toSendFiles, toSendBytes, toReceiveFiles, toReceiveBytes,
    // current, sending, rate, eta, skipped, lastSeen, lastInSync}],
    // pendingDeletes: [path], pendingDeleteCount, transfers: [{path,
    // size, done, key}]}; see folderStatus().
    void folderChanged(const QString& folderId, const QVariantMap& status);
    // Something the user should hear about, once. `kind`: "conflict" (theirs
    // to resolve), "following:<key>" (behind both sides, followed that
    // peer's chain), "kept" (stayed on our chain), "waiting" (no holder
    // online), or "blocked:<reason>" with reason outside | in_the_way |
    // changed | not_empty | io | clash, or "held:<reason>" (see
    // SyncFolder::heldReason()) for a file present here that is not synced,
    // or "deletes:<n>" (`path` empty): that many deletes wait for the user.
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
        Clock::time_point nextPlan;  // planning a large folder is paced by how long it takes
        // Every block of every file here, by hash: where a fetch can copy it
        // from. Built once per change of the index, not once per fetch.
        QHash<QByteArray, std::pair<QString, qint64>> blockIndex;
        quint64 blockIndexSeq = 0;
        bool    blockIndexValid = false;
        std::map<QString, Pull> pulls;
        QHash<QString, QString> blocked;  // path -> why it cannot be applied now
        QSet<QString> logged;             // conflicts already told
        QVariantList  conflicts;
        QVariantList  held;  // [{path, reason}]: present here but not synced
        QVariantMap   lastStatus;
        Clock::time_point lastEmit;
        qint64 needFiles = 0, needBytes = 0;
        QStringList   queued;  // files to fetch that have not started
        QHash<QString, sync::ScanResult::LeftOut> leftOut;  // by the last scan
        // The large-delete guard.
        QStringList   pendingDeletes;
        QSet<QString> approvedDeletes;
        qint64        free = -1;  // bytes left on the disk, looked up now and then
        Clock::time_point nextFree;
        // What each member still lacks from us, or we from it (by key), as
        // of the last plan.
        struct Need {
            qint64 sendFiles = 0, sendBytes = 0, sendChanges = 0;
            qint64 receiveFiles = 0, receiveBytes = 0, receiveChanges = 0;
            qint64 skipped = 0;  // paths it holds back
        };
        QHash<QString, Need> needs;
    };
    // Bytes moved lately, to give a rate.
    struct Meter {
        std::deque<std::pair<Clock::time_point, qint64>> samples;
        void   add(qint64 bytes);
        qint64 rate() const;  // bytes per second over the last seconds
    };
    struct Peer {
        QString key;
        QHash<QString, quint64> sent;      // folder -> our seq the peer has
        QSet<QString>           helloSent;
        QSet<QString>           listening;  // folders it asked for our index of
        QSet<QString>           heard;      // folders whose index it has sent us
        std::deque<std::vector<uint8_t>> queue;
        int inflight = 0;  // block requests to it
        Meter    received, served;
        QString  serving;  // the file it last asked a block of
        QString  servingFolder;
        Clock::time_point servedAt;
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
    const QHash<QByteArray, std::pair<QString, qint64>>& blockIndex(Folder& f);
    void requestBlocks(const QString& folderId);
    void finishPull(const QString& folderId, const QString& path);
    void dropPull(Folder& f, const QString& path);
    bool renameLocal(Folder& f, const QString& from, const sync::FileEntry& to);
    void onHello(const uconnect::DevId& dev, const QJsonObject& msg);
    void onIndex(const uconnect::DevId& dev, const QJsonObject& msg);
    void onGet(const uconnect::DevId& dev, const QJsonObject& msg);
    void onData(const uconnect::DevId& dev, std::span<const uint8_t> message);
    void onError(const uconnect::DevId& dev, const QJsonObject& msg);
    void failRequest(quint32 id, bool dropSource);
    QString absolute(const Folder& f, const QString& path) const;
    bool    diskMatches(const Folder& f, const QString& path) const;
    // The first part of `path` that is on disk only under another case (a
    // disk that ignores case finds it anyway); empty if there is none.
    QString caseMismatch(const Folder& f, const QString& path) const;
    // Moves a file into .synctrash, its path kept and the time added to its name.
    bool    keepAside(const Folder& f, const QString& path) const;
    void    cleanKept(const QString& folderId);
    bool    safeParent(const Folder& f, const QString& path, bool create) const;
    qint64  sinceOf(const QString& key) const;
    QVariantMap folderStatus(const QString& folderId, const Folder& f) const;
    QVariantList memberStatus(const QString& folderId, const Folder& f) const;
    void    emitStatus(const QString& folderId);
    const Peer* peerOf(const QString& key) const;
    QString rulesPath(const Folder& f, bool skipFile) const;
    // Whether a change of ours at `path` (its entry) has yet to reach the
    // peer with `key`.
    bool    pendingFor(const Folder& f, const sync::FileEntry& e, const QString& key) const;

    SendFn    send_;
    PeerKeyFn peerKey_;
    OfferFn   mayOffer_;  // empty: every peer
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
    int    guardMin_   = 10;
    int    guardMany_  = 25;
    double guardShare_ = 0.5;
    Clock::time_point lastSave_;
};

}  // namespace ucd
