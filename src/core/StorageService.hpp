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
//     {"t":"fs.write", "id":u32, "path":"/Docs", "name":"file.txt", "size":n}
//       waits for fs.write.r, then sends kind 3 [u8 3][u32 id, big-endian][bytes]
//     {"t":"fs.write.end", "id":u32, "sent":n}
//     {"t":"fs.write.cancel", "id":u32}
//
//   offerer -> requester
//     kind 1  {"t":"fs.list.r", "id", "path", "offset", "total",
//              "entries":[{"name", "dir", "size", "mtime"}]}       mtime: ms since epoch
//     kind 1  {"t":"fs.read.r", "id", "size"}       then the file as
//     kind 2  [u8 2][u32 id, big-endian][bytes]      frames, in order, then
//     kind 1  {"t":"fs.read.end", "id", "sent"}
//     kind 1  {"t":"fs.err", "id", "code", "error"}
//     kind 1  {"t":"fs.write.r", "id"}
//     kind 1  {"t":"fs.write.done", "id", "path"}  after saving the file
//     kind 1  {"t":"fs.write.err", "id", "code"}
//       code: not_offered | forbidden | not_found | denied | not_a_dir |
//             not_a_file | busy | io | bad_request
//
// Permissions (see Permissions.hpp): fs.list needs storage.observe, fs.read
// needs storage.download, fs.write needs storage.upload. Uploads add files;
// name collisions get a suffix, and existing files are never overwritten.
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
    void upload(const uconnect::DevId& dev, const QString& localPath, const QString& remoteFolder, quint32 id);
    void cancel(quint32 id);

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
    };
    struct PendingList {
        uconnect::DevId   dev{};
        QString           path;
        QVariantList      entries;
        Clock::time_point deadline;
    };
    struct Download {
        uconnect::DevId        dev{};
        QString                folder;
        QString                name;
        QString                partPath;
        std::unique_ptr<QFile> file;
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
        Clock::time_point deadline;
        Clock::time_point lastProgress;
    };
    struct Incoming {
        QString folder;
        QString root;
        QString name;
        QString virtualFolder;
        std::unique_ptr<QTemporaryFile> file;
        qint64 total = 0;
        qint64 received = 0;
        Clock::time_point deadline;
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
    QTimer*                        ticker_ = nullptr;
};

// A remote file name made safe to create locally: no separators, reserved
// characters or device names, never "." or "..".
QString safeFileName(const QString& name);

}  // namespace ucd
