#include "SyncModel.hpp"

#include <QCborArray>
#include <QCryptographicHash>
#include <QJsonArray>

#include "core/FsUtil.hpp"

namespace ucd::sync {

Order compare(const VersionVector& a, const VersionVector& b) {
    bool aAhead = false, bAhead = false;
    for (auto it = a.begin(); it != a.end(); ++it) {
        const quint64 other = b.value(it.key(), 0);
        if (it.value() > other) aAhead = true;
        else if (it.value() < other) bAhead = true;
    }
    for (auto it = b.begin(); it != b.end(); ++it)
        if (!a.contains(it.key()) && it.value() > 0) bAhead = true;
    if (aAhead && bAhead) return Order::Concurrent;
    if (aAhead) return Order::Newer;
    if (bAhead) return Order::Older;
    return Order::Equal;
}

VersionVector merged(const VersionVector& a, const VersionVector& b) {
    VersionVector out = a;
    for (auto it = b.begin(); it != b.end(); ++it)
        if (it.value() > out.value(it.key(), 0)) out[it.key()] = it.value();
    return out;
}

VersionVector bumped(VersionVector v, const QString& member) {
    v[member] = v.value(member, 0) + 1;
    return v;
}

QString memberIdOf(const QString& identityKeyHex) {
    const QByteArray key = QByteArray::fromHex(identityKeyHex.toLatin1());
    return QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Sha256).toHex().left(16));
}

bool FileEntry::sameContent(const FileEntry& other) const {
    if (deleted || other.deleted) return deleted == other.deleted;
    if (dir || other.dir) return dir == other.dir;
    return size == other.size && hash == other.hash;
}

qint64 FileEntry::blockLength(int index) const {
    const qint64 start = qint64(index) * blockSize;
    return std::min<qint64>(blockSize, size - start);
}

QJsonObject FileEntry::toJson() const {
    QJsonObject o{{"p", path}, {"v", [&] {
                       QJsonObject v;
                       for (auto it = version.begin(); it != version.end(); ++it) v.insert(it.key(), qint64(it.value()));
                       return v;
                   }()},
                  {"q", qint64(seq)}};
    if (skipped) o.insert("skip", true);
    if (deleted) {
        o.insert("del", true);
        return o;
    }
    if (dir) {
        o.insert("dir", true);
        return o;
    }
    o.insert("s", size);
    o.insert("m", mtime);
    o.insert("h", QString::fromLatin1(hash.toHex()));
    o.insert("bs", blockSize);
    QJsonArray b;
    for (const QByteArray& h : blocks) b.append(QString::fromLatin1(h.toHex()));
    o.insert("b", b);
    return o;
}

std::optional<FileEntry> FileEntry::fromJson(const QJsonObject& o) {
    FileEntry e;
    e.path    = o.value("p").toString();
    e.deleted = o.value("del").toBool();
    e.dir     = o.value("dir").toBool();
    e.skipped = o.value("skip").toBool();
    e.seq     = quint64(std::max<qint64>(0, o.value("q").toInteger()));
    const QJsonObject v = o.value("v").toObject();
    for (auto it = v.begin(); it != v.end(); ++it) {
        const qint64 n = it.value().toInteger(-1);
        if (n < 0 || it.key().isEmpty() || it.key().size() > 64) return std::nullopt;
        e.version.insert(it.key(), quint64(n));
    }
    if (!isValidSyncPath(e.path) || e.version.isEmpty() || e.version.size() > 1000) return std::nullopt;
    if (e.deleted || e.dir) return e;

    e.size      = o.value("s").toInteger(-1);
    e.mtime     = o.value("m").toInteger();
    e.hash      = QByteArray::fromHex(o.value("h").toString().toLatin1());
    e.blockSize = o.value("bs").toInt();
    for (const auto& b : o.value("b").toArray()) e.blocks << QByteArray::fromHex(b.toString().toLatin1());
    // Everything a receiver will rely on when fetching: sizes and hashes that agree.
    const qint64 expected = e.size > 0 && e.blockSize > 0 ? (e.size + e.blockSize - 1) / e.blockSize : 0;
    if (e.size < 0 || e.hash.size() != 32 || e.blockSize <= 0 || e.blockSize > kMaxBlockSize ||
        e.blocks.size() != expected)
        return std::nullopt;
    for (const QByteArray& h : e.blocks)
        if (h.size() != 32) return std::nullopt;
    return e;
}

QCborMap FileEntry::toCbor() const {
    QCborMap m;
    m.insert(QStringLiteral("p"), path);
    QCborMap v;
    for (auto it = version.begin(); it != version.end(); ++it) v.insert(it.key(), qint64(it.value()));
    m.insert(QStringLiteral("v"), v);
    m.insert(QStringLiteral("q"), qint64(seq));
    if (skipped) m.insert(QStringLiteral("skip"), true);
    if (deleted) m.insert(QStringLiteral("del"), true);
    if (dir) m.insert(QStringLiteral("dir"), true);
    if (!deleted && !dir) {
        m.insert(QStringLiteral("s"), size);
        m.insert(QStringLiteral("m"), mtime);
        m.insert(QStringLiteral("h"), hash);
        m.insert(QStringLiteral("bs"), blockSize);
        QCborArray b;
        for (const QByteArray& h : blocks) b.append(h);
        m.insert(QStringLiteral("b"), b);
    }
    return m;
}

FileEntry FileEntry::fromCbor(const QCborMap& m) {
    FileEntry e;
    e.path    = m.value(QStringLiteral("p")).toString();
    e.seq     = quint64(m.value(QStringLiteral("q")).toInteger());
    e.skipped = m.value(QStringLiteral("skip")).toBool();
    e.deleted = m.value(QStringLiteral("del")).toBool();
    e.dir     = m.value(QStringLiteral("dir")).toBool();
    const QCborMap v = m.value(QStringLiteral("v")).toMap();
    for (auto it = v.begin(); it != v.end(); ++it) e.version.insert(it.key().toString(), quint64(it.value().toInteger()));
    e.size      = m.value(QStringLiteral("s")).toInteger();
    e.mtime     = m.value(QStringLiteral("m")).toInteger();
    e.hash      = m.value(QStringLiteral("h")).toByteArray();
    e.blockSize = qint32(m.value(QStringLiteral("bs")).toInteger());
    for (const auto& b : m.value(QStringLiteral("b")).toArray()) e.blocks << b.toByteArray();
    return e;
}

qint32 blockSizeFor(qint64 fileSize) {
    qint64 size = 128 * 1024;
    while (size < kMaxBlockSize && fileSize / size > 2000) size *= 2;
    return qint32(size);
}

bool isValidSyncPath(const QString& path) {
    if (path.isEmpty() || path.size() > 4096 || path.startsWith(QLatin1Char('/')) || path.endsWith(QLatin1Char('/')))
        return false;
    const QStringList parts = path.split(QLatin1Char('/'));
    for (const QString& part : parts) {
        if (!isValidComponent(part) || part.startsWith(QLatin1String(kPartPrefix))) return false;
    }
    if (parts.first() == QLatin1String(kKeptDir)) return false;
    return parts.first() != QLatin1String(kSkipFile) || parts.size() > 1;
}

}  // namespace ucd::sync
