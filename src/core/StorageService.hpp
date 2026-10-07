#pragma once
// The storage service: a device shares folders, and authorized devices in
// the topic can browse, download and upload files. One instance plays both
// roles, and lives on the Session's worker thread.
//
// Wire protocol (inside the [u8 kind][payload] envelope from Hello.hpp):
//
//   requester -> offerer, kind 1 (JSON)
//     {"t":"fs.list",   "id":u32, "path":"/Docs/sub", "offset":n}
//     {"t":"fs.read",   "id":u32, "path":"/Docs/file.txt"}
//     {"t":"fs.cancel", "id":u32}
//     {"t":"fs.write", "id":u32, "path":"/Docs", "name":"file.txt", "size":n,
//      "replace":bool}   replace: overwrite a file of that name instead of
//                         adding "file (1).txt"
//       waits for fs.write.r, then sends kind 3 [u8 3][u32 id, big-endian][bytes]
//     {"t":"fs.write.end", "id":u32, "sent":n, "sha256":"hex"}
//     {"t":"fs.write.cancel", "id":u32}
//     {"t":"fs.rename", "id":u32, "path":"/Docs/a.txt", "name":"b.txt"}
//     {"t":"fs.delete", "id":u32, "path":"/Docs/a.txt"}   files or folders
//     {"t":"fs.mkdir",  "id":u32, "path":"/Docs", "name":"New folder",
//      "existing":bool}  existing: answer with a folder of that name if there
//                         is one instead of adding "New folder (1)"
//     {"t":"fs.watch", "id":u32, "path":"/Docs"}   tell me when this folder's
//     {"t":"fs.unwatch", "id":u32}                  listing changes; no reply
//     {"t":"fs.move", "id":u32, "path":"/Docs/a.txt", "to":"/Other/folder"}
//     {"t":"fs.copy", "id":u32, "path":"/Docs/a.txt", "to":"/Other/folder"}
//       files or folders, into `to`; a name already there gets a suffix
//
//   offerer -> requester
//     kind 1  {"t":"fs.list.r", "id", "path", "offset", "total",
//              "entries":[{"name", "dir", "size", "mtime"}]}       mtime: ms since epoch
//     kind 1  {"t":"fs.read.r", "id", "size"}       then the file as
//     kind 2  [u8 2][u32 id, big-endian][bytes]      frames, in order, then
//     kind 1  {"t":"fs.read.end", "id", "sent", "sha256"}
//     kind 1  {"t":"fs.err", "id", "code", "error"}
//     kind 1  {"t":"fs.write.r", "id"}
//     kind 1  {"t":"fs.write.done", "id", "path"}  after saving the file
//     kind 1  {"t":"fs.write.err", "id", "code"}
//     kind 1  {"t":"fs.done", "id", "path"}   rename, delete or mkdir succeeded;
//             path is the result (delete: what was removed; also "trashed";
//             mkdir: also "existed" when an existing folder was reused)
//     kind 1  {"t":"fs.changed", "id", "path"}  a watched folder changed (or
//             stopped resolving); list it again
//     kind 1  {"t":"fs.progress", "id", "done", "total"}  every few seconds
//             while a copy (or a move between drives) runs; bytes
//       code: not_offered | forbidden | not_found | denied | not_a_dir |
//             not_a_file | busy | io | bad_request | exists | root | inside |
//             corrupt
//
// "sha256" is the SHA-256 of the whole file, lowercase hex. The receiver
// checks it when present and discards a file that does not match ("corrupt");
// a sender that leaves it out is trusted on size alone.
//
// Permissions (see Permissions.hpp): fs.list and fs.watch need storage.observe, fs.read
// needs storage.read, fs.write, fs.mkdir and fs.copy need storage.upload, and
// fs.rename, fs.delete, fs.move and fs.write with "replace" need storage.write.
// Uploads otherwise add files; name collisions get a suffix.
//
// Rename and delete act on the named entry itself: a link inside a shared
// folder is renamed or removed, never what it points to. A shared folder
// itself cannot be renamed, moved or deleted ("root"). Deleted items go to the
// offering device's trash when it has one. Copies leave out links inside the
// copied folder, and a link itself can be moved but not copied.
//
// Paths are virtual. "/" lists the shared folders by name, and "/<name>/..."
// is inside one; a requester never learns where a folder really is. Listings
// are paged (offset/total); the requester asks for pages until it has all.

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include "uconnect/types.hpp"

class QCryptographicHash;
class QFile;
class QTimer;
class QTemporaryFile;

namespace ucd {

class StorageService : public QObject {
    Q_OBJECT
public:
    // Queues a message to a connected peer; false if it could not be queued
    // right now (not connected, or the peer's queue is full).
    using SendFn = std::function<bool(const uconnect::DevId&, std::span<const uint8_t>)>;
    // Whether a peer holds a permission (e.g. "storage.observe").
    using AuthorizeFn = std::function<bool(const uconnect::DevId&, const QString&)>;

    StorageService(SendFn send, AuthorizeFn authorize, QObject* parent = nullptr);
    ~StorageService() override;

    // --- offerer ---
    void setOffered(bool offered);
    void setSharedFolders(const QStringList& folders);

    // --- requester ---
    void list(const uconnect::DevId& dev, const QString& path, quint32 id);
    void download(const uconnect::DevId& dev, const QString& remotePath, const QString& folder, quint32 id);
    void upload(const uconnect::DevId& dev, const QString& localPath, const QString& remoteFolder,
                quint32 id, bool replace = false);
    // fs.rename, fs.delete or fs.mkdir; `request` is the message without "id".
    void operation(const uconnect::DevId& dev, QJsonObject request, quint32 id);
    // Asks the device to report changes to a folder's listing (folderChanged)
    // until unwatch(). Ends quietly if the device drops it.
    void watch(const uconnect::DevId& dev, const QString& path, quint32 id);
    void unwatch(quint32 id);
    void cancel(quint32 id);

    // Deleted items go to the trash when possible; tests turn that off so
    // they do not fill the user's Recycle Bin.
    void setUseTrash(bool use) { useTrash_ = use; }
    // Tests: moves copy and delete, as they must between drives.
    void setAlwaysCopyMoves(bool copy) { alwaysCopyMoves_ = copy; }

    // True if the message belonged to this service.
    bool handleMessage(const uconnect::DevId& dev, std::span<const uint8_t> message);
    void peerGone(const uconnect::DevId& dev);
    // The session ended: fail everything in flight.
    void reset();

signals:
    void listFinished(quint32 id, const QString& path, const QVariantList& entries, const QString& error);
    void downloadProgress(quint32 id, qint64 received, qint64 total);
    void downloadFinished(quint32 id, const QString& localPath, const QString& error);
    void uploadProgress(quint32 id, qint64 sent, qint64 total);
    void uploadFinished(quint32 id, const QString& remotePath, const QString& error);
    void operationFinished(quint32 id, const QString& path, const QString& error);
    void folderChanged(quint32 watchId, const QString& path);
    // A long fs.copy or fs.move; `total` is -1 until known.
    void operationProgress(quint32 id, qint64 done, qint64 total);

private:
    using Clock = std::chrono::steady_clock;

    struct SharedFolder {
        QString name;       // what requesters see
        QString canonical;  // where it is
    };
    struct Outgoing {  // a file being sent
        uconnect::DevId                  dev{};
        quint32                          id = 0;
        std::unique_ptr<QFile>           file;
        qint64                           sent = 0;
        bool                             done = false;  // nothing left to read
        bool                             upload = false;
        bool                             ready = true;  // uploads wait for the receiver
        std::deque<std::vector<uint8_t>> queue;          // built, not yet accepted by send
        std::unique_ptr<QCryptographicHash> hash;         // of what was read so far
    };
    struct PendingList {
        uconnect::DevId   dev{};
        QString           path;
        QVariantList      entries;
        Clock::time_point deadline;
    };
    struct Download {
        uconnect::DevId        dev{};
        QString                remotePath;
        bool                   requested = false;  // false while queued locally
        QString                folder;
        QString                name;
        QString                partPath;
        std::unique_ptr<QFile> file;
        std::unique_ptr<QCryptographicHash> hash;
        qint64                 total    = -1;
        qint64                 received = 0;
        Clock::time_point      deadline;
        Clock::time_point      lastProgress;
    };
    struct Upload {
        uconnect::DevId dev{};
        QString localPath;
        QString folder;
        qint64 total = 0;
        qint64 sent = 0;
        bool requested = false;
        bool replace = false;
        Clock::time_point deadline;
        Clock::time_point lastProgress;
    };
    struct PendingOp {
        uconnect::DevId   dev{};
        Clock::time_point deadline;
    };
    // An existing entry named by a virtual path, located without following
    // the entry itself if it is a link.
    struct Entry {
        const SharedFolder* folder = nullptr;
        QString             parent;   // real, inside the folder
        QString             path;     // parent + name
        QString             virtualParent;
    };
    struct Incoming {
        QString folder;
        QString root;
        QString name;
        QString virtualFolder;
        std::unique_ptr<QTemporaryFile> file;
        std::unique_ptr<QCryptographicHash> hash;
        qint64 total = 0;
        qint64 received = 0;
        bool replace = false;
        Clock::time_point deadline;
    };
    // A folder a peer is looking at. Polled rather than watched with the OS:
    // on Windows a change notification holds the folder open, which would stop
    // anyone renaming or deleting it or its parents.
    struct Watch {
        QString path;  // virtual
        size_t  signature = 0;
    };
    struct Watching {  // requester side
        uconnect::DevId dev{};
        QString         path;
    };
    struct JobState;  // shared with the thread running the job
    // A copy, or a move between drives, running off this thread.
    struct Job {
        std::shared_ptr<JobState> state;
        QString                   target;  // virtual path of the result
        Clock::time_point         lastBeat;
    };
    struct Reply {
        uconnect::DevId dev{};
        std::vector<uint8_t> message;
        Clock::time_point deadline;
    };

    // offerer
    void serveList(const uconnect::DevId& dev, const QJsonObject& req);
    void serveRead(const uconnect::DevId& dev, const QJsonObject& req);
    void serveWrite(const uconnect::DevId& dev, const QJsonObject& req);
    void serveRename(const uconnect::DevId& dev, const QJsonObject& req);
    void serveDelete(const uconnect::DevId& dev, const QJsonObject& req);
    void serveMkdir(const uconnect::DevId& dev, const QJsonObject& req);
    void serveTransfer(const uconnect::DevId& dev, const QJsonObject& req, bool copy);
    void finishJob(const uconnect::DevId& dev, quint32 id, const QString& code);
    void serveWatch(const uconnect::DevId& dev, const QJsonObject& req);
    // A summary of a watched folder's listing; 0 if it does not resolve.
    size_t watchSignature(const QString& virtualPath) const;
    void pollWatches();
    std::optional<Entry> resolveEntry(const QString& path, QString* code) const;
    void onWriteData(const uconnect::DevId& dev, std::span<const uint8_t> message);
    void onWriteEnd(const uconnect::DevId& dev, const QJsonObject& msg);
    void onWriteReply(const uconnect::DevId& dev, const QJsonObject& msg);
    void failIncoming(const uconnect::DevId& dev, quint32 id, const QString& code);
    void sendWriteReply(const uconnect::DevId& dev, const QJsonObject& reply);
    void finishUpload(quint32 id, const QString& remotePath, const QString& error);
    void sendError(const uconnect::DevId& dev, quint32 id, const QString& code);
    QJsonArray listing(const QString& absolutePath, const SharedFolder& folder);
    // Maps a virtual path to a real one inside a shared folder. nullopt with
    // `code` set when it does not resolve or would escape the folder.
    std::optional<std::pair<const SharedFolder*, QString>> resolve(const QString& path, QString* code) const;
    void pump();

    // requester
    void onListPage(const uconnect::DevId& dev, const QJsonObject& msg);
    void onReadStart(const uconnect::DevId& dev, const QJsonObject& msg);
    void onReadData(const uconnect::DevId& dev, std::span<const uint8_t> message);
    void onReadEnd(const uconnect::DevId& dev, const QJsonObject& msg);
    void onError(const uconnect::DevId& dev, const QJsonObject& msg);
    void finishList(quint32 id, const QString& error);
    void finishDownload(quint32 id, const QString& error);
    void finishOperation(quint32 id, const QString& path, const QString& error);
    // Sends queued fs.read requests while fewer than the offerer's limit are
    // in flight; it answers "busy" beyond that.
    void startQueuedDownloads(const uconnect::DevId& dev);
    void checkDeadlines();
    void updateTicker();

    SendFn      send_;
    AuthorizeFn authorize_;

    bool                      offered_ = false;
    std::vector<SharedFolder> folders_;
    // Direction is part of the key: both peers allocate their own request ids.
    std::map<std::tuple<uconnect::DevId, quint32, bool>, Outgoing> outgoing_;
    std::map<std::pair<uconnect::DevId, quint32>, Incoming> incoming_;
    std::deque<Reply> replies_;
    QTimer*                   pumpTimer_ = nullptr;

    // One-entry cache, so paging through a large directory reads it once.
    QString           cachedPath_;
    QJsonArray        cachedEntries_;
    Clock::time_point cachedAt_;

    std::map<quint32, PendingList> lists_;
    std::map<quint32, Download>    downloads_;
    std::map<quint32, Upload>      uploads_;
    std::map<quint32, PendingOp>   ops_;
    bool                           useTrash_ = true;
    bool                           alwaysCopyMoves_ = false;
    std::map<std::pair<uconnect::DevId, quint32>, Job> jobs_;
    std::map<std::pair<uconnect::DevId, quint32>, Watch> watches_;
    std::map<quint32, Watching>    watching_;
    QTimer*                        ticker_ = nullptr;
    QTimer*                        watchTimer_ = nullptr;
};

// A remote file name made safe to create locally: no separators, reserved
// characters or device names, never "." or "..".
QString safeFileName(const QString& name);

}  // namespace ucd
