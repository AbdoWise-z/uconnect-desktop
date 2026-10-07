#pragma once
// The data the sync service keeps and exchanges: one entry per file or folder
// in a synced folder, versioned with a version vector so devices can tell,
// without a central node or clocks, which of two versions is newer or that
// they conflict. See docs/protocol.md, "Folder sync".

#include <optional>

#include <QByteArray>
#include <QCborMap>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QVector>

namespace ucd::sync {

// member id -> how many changes that member has made to this file.
using VersionVector = QMap<QString, quint64>;

enum class Order {
    Equal,
    Newer,       // the first has seen everything the second has, and more
    Older,
    Concurrent,  // each has a change the other has not seen: a conflict
};
Order compare(const VersionVector& a, const VersionVector& b);
// The highest counter of each member.
VersionVector merged(const VersionVector& a, const VersionVector& b);
// `v` with `member`'s counter raised by one: a new version that follows `v`.
VersionVector bumped(VersionVector v, const QString& member);

// A member's id in version vectors: the first 16 hex digits of the SHA-256 of
// its identity key, so it stays the same whatever the device's session id.
QString memberIdOf(const QString& identityKeyHex);

struct FileEntry {
    QString path;  // relative, '/'-separated
    bool    dir     = false;
    bool    deleted = false;
    // The device no longer takes part in this path (.syncskip); others leave
    // it out when choosing a version.
    bool    skipped = false;
    qint64  size  = 0;
    qint64  mtime = 0;  // ms since epoch
    QByteArray         hash;  // SHA-256 of the whole file, raw
    qint32             blockSize = 0;
    QVector<QByteArray> blocks;  // SHA-256 of each block, raw
    VersionVector      version;
    quint64            seq = 0;  // when it last changed in the index that holds it

    // Same thing on disk: both deleted, both the same folder, or files with
    // the same contents.
    bool sameContent(const FileEntry& other) const;
    qint64 blockLength(int index) const;

    QJsonObject                     toJson() const;
    static std::optional<FileEntry> fromJson(const QJsonObject& o);
    QCborMap                        toCbor() const;
    static FileEntry                fromCbor(const QCborMap& m);
};

// Blocks are 128 KiB, doubled until a file has at most 2000 of them, up to
// 16 MiB.
qint32 blockSizeFor(qint64 fileSize);
inline constexpr qint32 kMaxBlockSize = 16 * 1024 * 1024;

// A relative path another device may name: valid components only, and none
// of the names the sync service keeps for itself.
bool isValidSyncPath(const QString& path);
// Temporary files of downloads in progress.
inline constexpr char kPartPrefix[] = ".syncpart-";
inline constexpr char kIgnoreFile[] = ".syncignore";  // synced
inline constexpr char kSkipFile[]   = ".syncskip";    // this device only

}  // namespace ucd::sync
