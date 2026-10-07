#include "StorageService.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <limits>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QPromise>
#include <QStorageInfo>
#include <QThreadPool>
#include <QTimer>
#include <QTemporaryFile>

#include "core/Hello.hpp"
#include "core/Permissions.hpp"

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace std::chrono_literals;

namespace ucd {
namespace {

constexpr qint64 kChunkSize          = 256 * 1024;
constexpr qint64 kPumpBudget         = 4 * 1024 * 1024;  // per event-loop turn
constexpr int    kPageSize           = 500;
constexpr int    kMaxEntries         = 20000;
constexpr size_t kMaxOutgoingPerPeer = 4;
constexpr auto   kRequestTimeout     = 20s;
constexpr auto   kStallTimeout       = 30s;
constexpr auto   kCacheFor           = 10s;
constexpr auto   kProgressEvery      = 100ms;
constexpr auto   kJobBeatEvery       = 2s;
constexpr auto   kWatchEvery         = 2s;
constexpr size_t kMaxWatchesPerPeer  = 16;

#ifdef Q_OS_WIN
constexpr Qt::CaseSensitivity kPathCase = Qt::CaseInsensitive;
#else
constexpr Qt::CaseSensitivity kPathCase = Qt::CaseSensitive;
#endif

bool isReservedDeviceName(const QString& name) {
    static const QStringList reserved = [] {
        QStringList r{"CON", "PRN", "AUX", "NUL"};
        for (int i = 1; i <= 9; ++i) r << QStringLiteral("COM%1").arg(i) << QStringLiteral("LPT%1").arg(i);
        return r;
    }();
    return reserved.contains(name.section(QLatin1Char('.'), 0, 0).trimmed().toUpper());
}

// One component of a virtual path. Rejects anything that could step outside
// a folder or name something other than a plain file: "..", NUL, and on
// Windows backslashes, alternate data streams ("file:stream") and devices.
bool isValidComponent(const QString& c) {
    if (c.isEmpty() || c == QLatin1String(".") || c == QLatin1String("..") || c.contains(QChar(0)))
        return false;
#ifdef Q_OS_WIN
    if (c.contains(QLatin1Char('\\')) || c.contains(QLatin1Char(':')) || isReservedDeviceName(c))
        return false;
#endif
    return true;
}

// Where a path really leads, with every link along it followed; empty if it
// does not exist. On Windows QFileInfo::canonicalFilePath() does not follow
// junctions, so a junction inside a shared folder could point anywhere; ask
// the filesystem for the final path of an open handle instead.
QString realPath(const QString& path) {
#ifdef Q_OS_WIN
    const std::wstring native = QDir::toNativeSeparators(QDir::cleanPath(path)).toStdWString();
    HANDLE h = CreateFileW(native.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD n = GetFinalPathNameByHandleW(h, buffer.data(), static_cast<DWORD>(buffer.size()),
                                        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (n >= buffer.size()) {
        buffer.resize(n + 1);
        n = GetFinalPathNameByHandleW(h, buffer.data(), static_cast<DWORD>(buffer.size()),
                                      FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    }
    CloseHandle(h);
    if (n == 0 || n >= buffer.size()) return {};
    QString out = QString::fromWCharArray(buffer.data(), static_cast<qsizetype>(n));
    if (out.startsWith(QLatin1String("\\\\?\\UNC\\"))) out = QLatin1String("\\\\") + out.mid(8);
    else if (out.startsWith(QLatin1String("\\\\?\\"))) out = out.mid(4);
    return QDir::fromNativeSeparators(out);
#else
    return QFileInfo(path).canonicalFilePath();
#endif
}

bool isWithin(const QString& path, const QString& root) {
    if (path.compare(root, kPathCase) == 0) return true;
    const QString prefix = root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/');
    return path.startsWith(prefix, kPathCase);
}

std::optional<quint32> idOf(const QJsonObject& o) {
    const qint64 v = o.value("id").toInteger(-1);
    if (v < 0 || v > std::numeric_limits<quint32>::max()) return std::nullopt;
    return static_cast<quint32>(v);
}

QJsonObject entryJson(const QFileInfo& fi, const QString& name) {
    const bool dir = fi.isDir();
    return {
        {"name", name},
        {"dir", dir},
        {"size", dir ? qint64(0) : fi.size()},
        {"mtime", fi.lastModified().toMSecsSinceEpoch()},
    };
}

std::vector<uint8_t> dataFrame(quint32 id, const QByteArray& bytes, bool upload = false) {
    std::vector<uint8_t> out(static_cast<size_t>(bytes.size()) + 5);
    out[0] = static_cast<uint8_t>(upload ? MessageKind::StorageUploadData : MessageKind::Data);
    out[1] = static_cast<uint8_t>(id >> 24);
    out[2] = static_cast<uint8_t>(id >> 16);
    out[3] = static_cast<uint8_t>(id >> 8);
    out[4] = static_cast<uint8_t>(id);
    std::memcpy(out.data() + 5, bytes.constData(), static_cast<size_t>(bytes.size()));
    return out;
}

QString errorText(const QString& code) {
    if (code == QLatin1String("not_offered")) return StorageService::tr("This device does not offer storage.");
    if (code == QLatin1String("forbidden")) return StorageService::tr("This device has not given you permission to do that.");
    if (code == QLatin1String("not_found")) return StorageService::tr("Not found.");
    if (code == QLatin1String("denied")) return StorageService::tr("Access denied.");
    if (code == QLatin1String("not_a_dir")) return StorageService::tr("Not a folder.");
    if (code == QLatin1String("not_a_file")) return StorageService::tr("Not a file.");
    if (code == QLatin1String("busy")) return StorageService::tr("The device is busy with other transfers; try again shortly.");
    if (code == QLatin1String("io")) return StorageService::tr("The device could not read or save the file.");
    if (code == QLatin1String("bad_request")) return StorageService::tr("The device rejected the request.");
    if (code == QLatin1String("exists")) return StorageService::tr("Something with that name already exists there.");
    if (code == QLatin1String("root")) return StorageService::tr("A shared folder itself cannot be renamed, moved or deleted.");
    if (code == QLatin1String("inside")) return StorageService::tr("A folder cannot be moved or copied into itself.");
    if (code == QLatin1String("corrupt")) return StorageService::tr("The file was damaged on the way; try again.");
    if (code == QLatin1String("too_large")) return StorageService::tr("The file is larger than the device accepts.");
    if (code == QLatin1String("no_space")) return StorageService::tr("The device does not have enough free space.");
    if (code == QLatin1String("read_only")) return StorageService::tr("This shared folder is read-only.");
    return StorageService::tr("The request failed.");
}

QString uniquePath(const QString& folder, const QString& name) {
    const QDir dir(folder);
    QString candidate = dir.filePath(name);
    if (!QFileInfo::exists(candidate)) return candidate;
    const QFileInfo fi(name);
    const QString base   = fi.completeBaseName();
    const QString suffix = fi.suffix();
    for (int n = 1;; ++n) {
        candidate = dir.filePath(suffix.isEmpty() ? QStringLiteral("%1 (%2)").arg(base).arg(n)
                                                  : QStringLiteral("%1 (%2).%3").arg(base).arg(n).arg(suffix));
        if (!QFileInfo::exists(candidate)) return candidate;
    }
}

// Removes a file, folder or link without ever following a link: a link (or
// junction) is removed itself, so a delete cannot reach what it points to.
// QDir::removeRecursively() descends into junctions on Windows, so it is not
// used here.
bool removeEntry(const QFileInfo& fi) {
    const QString path = fi.absoluteFilePath();
    if (fi.isSymLink() || fi.isJunction()) return QFile::remove(path) || QDir().rmdir(path);
    if (fi.isDir()) {
        const QFileInfoList children = QDir(path).entryInfoList(
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        for (const QFileInfo& child : children)
            if (!removeEntry(child)) return false;
        return QDir().rmdir(path);
    }
    QFile file(path);
    if (file.remove()) return true;
    file.setPermissions(file.permissions() | QFileDevice::WriteOwner);  // read-only files on Windows
    return file.remove();
}

// Moves `source` onto `target` in one step, replacing a file already there.
bool replaceWith(const QString& source, const QString& target) {
#ifdef Q_OS_WIN
    return MoveFileExW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(source).utf16()),
                       reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(target).utf16()),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return std::rename(QFile::encodeName(source).constData(), QFile::encodeName(target).constData()) == 0;
#endif
}

constexpr auto kAllEntries = QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System;

qint64 treeSize(const QFileInfo& fi) {
    if (fi.isSymLink() || fi.isJunction()) return 0;
    if (!fi.isDir()) return fi.size();
    qint64 n = 0;
    for (const QFileInfo& child : QDir(fi.absoluteFilePath()).entryInfoList(kAllEntries)) n += treeSize(child);
    return n;
}

// Copies a file or folder to `to`, which must not exist. Links inside a
// folder are left out rather than followed, so a copy cannot reach outside.
bool copyTree(const QFileInfo& from, const QString& to, std::atomic<qint64>& done, const std::atomic<bool>& cancel) {
    if (cancel) return false;
    if (from.isSymLink() || from.isJunction()) return true;
    if (from.isDir()) {
        if (!QDir().mkdir(to)) return false;
        for (const QFileInfo& child : QDir(from.absoluteFilePath()).entryInfoList(kAllEntries))
            if (!copyTree(child, QDir(to).filePath(child.fileName()), done, cancel)) return false;
        return true;
    }
    if (!QFile::copy(from.absoluteFilePath(), to)) return false;
    done += from.size();
    return true;
}

// Runs `work` on a pool thread and `done` with its result (nullopt if it never
// ran) back on `context`'s thread; nothing runs once `context` is gone.
template <class T, class Work, class Done>
void runOffThread(QObject* context, Work work, Done done) {
    auto  promise = std::make_shared<QPromise<T>>();
    auto* watcher = new QFutureWatcher<T>(context);
    QObject::connect(watcher, &QFutureWatcherBase::finished, context, [watcher, done] {
        std::optional<T> result;
        if (watcher->future().resultCount() > 0) result = watcher->result();
        watcher->deleteLater();
        done(std::move(result));
    });
    watcher->setFuture(promise->future());
    promise->start();
    QThreadPool::globalInstance()->start([promise, work] {
        promise->addResult(work());
        promise->finish();
    });
}

// The partial download of `remotePath` in `folder`: one name per remote
// file, so a second attempt finds what the first one left.
QString partPathFor(const QString& folder, const QString& name, const QString& remotePath) {
    const QByteArray tag = QCryptographicHash::hash(remotePath.toUtf8(), QCryptographicHash::Sha256).toHex().left(8);
    return QDir(folder).filePath(QStringLiteral("%1.%2.part").arg(name, QString::fromLatin1(tag)));
}

std::unique_ptr<QCryptographicHash> newHash() {
    return std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
}

// A digest sent by the other side, if any, matches ours.
bool digestMatches(const QJsonObject& msg, const QCryptographicHash& hash) {
    const QJsonValue sent = msg.value("sha256");
    return sent.isUndefined() || sent.toString().compare(QString::fromLatin1(hash.result().toHex()), Qt::CaseInsensitive) == 0;
}

bool sameVolume(const QString& a, const QString& b) {
    const QStorageInfo x(a), y(b);
    return x.isValid() && y.isValid() && x.device() == y.device();
}

QString virtualPath(const QStringList& parts) {
    return QLatin1Char('/') + parts.join(QLatin1Char('/'));
}

bool isAcceptableName(const QString& name) {
    return isValidComponent(name) && safeFileName(name) == name;
}

}  // namespace

QString safeFileName(const QString& name) {
    QString out = name;
    static const QString forbidden = QStringLiteral("/\\:*?\"<>|");
    for (QChar& c : out)
        if (c.unicode() < 0x20 || forbidden.contains(c)) c = QLatin1Char('_');
    out = out.trimmed();
    while (out.endsWith(QLatin1Char('.')) || out.endsWith(QLatin1Char(' '))) out.chop(1);
    if (out.isEmpty()) out = QStringLiteral("download");
    if (isReservedDeviceName(out)) out.prepend(QLatin1Char('_'));
    return out.left(200);
}

StorageService::StorageService(SendFn send, AuthorizeFn authorize, QObject* parent)
    : QObject(parent),
      send_(std::move(send)),
      authorize_(std::move(authorize)),
      pumpTimer_(new QTimer(this)),
      ticker_(new QTimer(this)),
      watchTimer_(new QTimer(this)) {
    pumpTimer_->setSingleShot(true);
    connect(pumpTimer_, &QTimer::timeout, this, &StorageService::pump);
    ticker_->setInterval(1s);
    connect(ticker_, &QTimer::timeout, this, &StorageService::checkDeadlines);
    watchTimer_->setInterval(kWatchEvery);
    connect(watchTimer_, &QTimer::timeout, this, &StorageService::pollWatches);
}

struct StorageService::JobState {
    std::atomic<qint64> done{0};
    std::atomic<qint64> total{-1};
    std::atomic<bool>   cancel{false};
};

StorageService::~StorageService() {
    for (auto& [key, job] : jobs_) job.state->cancel = true;
}

// --- offerer ------------------------------------------------------------------

void StorageService::setOffered(bool offered) {
    offered_ = offered;
    if (offered_) return;
    for (const auto& [key, w] : watches_)
        send_(key.first, encodeControl({{"t", "fs.changed"}, {"id", qint64(key.second)}, {"path", w.path}}));
    watches_.clear();
    watchTimer_->stop();
    std::erase_if(outgoing_, [&](const auto& kv) {
        if (kv.second.upload) return false;
        sendError(kv.second.dev, kv.second.id, QStringLiteral("not_offered"));
        return true;
    });
    while (!incoming_.empty()) {
        const auto key = incoming_.begin()->first;
        failIncoming(key.first, key.second, QStringLiteral("not_offered"));
    }
}

void StorageService::setSharedFolders(const QStringList& folders, const QStringList& readOnly) {
    while (!incoming_.empty()) {
        const auto key = incoming_.begin()->first;
        failIncoming(key.first, key.second, QStringLiteral("denied"));
    }
    folders_.clear();
    cachedPath_.clear();
    QTimer::singleShot(0, this, &StorageService::pollWatches);  // the shares themselves changed
    for (const QString& path : folders) {
        const QFileInfo fi(path);
        const QString   canonical = realPath(path);
        if (!fi.isDir() || canonical.isEmpty()) continue;

        QString name = fi.fileName();
        if (name.isEmpty()) {  // a drive or filesystem root
            QString drive = QDir::toNativeSeparators(canonical);
            drive.remove(QLatin1Char(':')).remove(QLatin1Char('\\')).remove(QLatin1Char('/'));
            name = drive.isEmpty() ? tr("Root") : tr("Drive %1").arg(drive);
        }
        const QString base = name;
        auto taken = [&](const QString& n) {
            return std::any_of(folders_.begin(), folders_.end(),
                               [&](const SharedFolder& f) { return f.name.compare(n, kPathCase) == 0; });
        };
        for (int n = 2; taken(name); ++n) name = QStringLiteral("%1 (%2)").arg(base).arg(n);
        folders_.push_back({name, canonical, readOnly.contains(path)});
    }
}

std::optional<std::pair<const StorageService::SharedFolder*, QString>>
StorageService::resolve(const QString& path, QString* code) const {
    const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (!path.startsWith(QLatin1Char('/')) || parts.isEmpty() ||
        !std::all_of(parts.begin(), parts.end(), isValidComponent)) {
        *code = QStringLiteral("bad_request");
        return std::nullopt;
    }
    const auto folder = std::find_if(folders_.begin(), folders_.end(),
                                     [&](const SharedFolder& f) { return f.name == parts.first(); });
    if (folder == folders_.end()) {
        *code = QStringLiteral("not_found");
        return std::nullopt;
    }

    const QString joined = folder->canonical + QLatin1Char('/') + parts.mid(1).join(QLatin1Char('/'));
    const QString real   = realPath(joined);
    if (real.isEmpty()) {
        *code = QStringLiteral("not_found");
        return std::nullopt;
    }
    // realPath() follows every link and junction on the way; one that leads
    // out of the folder must not become a way out of it.
    if (!isWithin(real, folder->canonical)) {
        *code = QStringLiteral("denied");
        return std::nullopt;
    }
    return std::make_pair(&*folder, real);
}

QJsonArray StorageService::listing(const QString& absolutePath, const SharedFolder& folder, bool firstPage) {
    // Later pages come from the read that served the first, so a listing is
    // one consistent snapshot; a first page always reads the folder afresh.
    const auto now = Clock::now();
    if (!firstPage && cachedPath_ == absolutePath && now - cachedAt_ < kCacheFor) return cachedEntries_;

    QJsonArray out;
    const QFileInfoList infos = QDir(absolutePath).entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden,
        QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo& fi : infos) {
        if (!isValidComponent(fi.fileName())) continue;
        const bool partial = std::any_of(incoming_.begin(), incoming_.end(), [&](const auto& kv) {
            return kv.second.file->fileName().compare(fi.absoluteFilePath(), kPathCase) == 0;
        });
        if (partial) continue;
        if (fi.isSymLink() || fi.isJunction()) {
            const QString target = realPath(fi.absoluteFilePath());
            if (target.isEmpty() || !isWithin(target, folder.canonical)) continue;
        }
        out.append(entryJson(fi, fi.fileName()));
    }
    cachedPath_    = absolutePath;
    cachedEntries_ = out;
    cachedAt_      = now;
    return out;
}

void StorageService::serveList(const uconnect::DevId& dev, const QJsonObject& req) {
    const auto id = idOf(req);
    if (!id) return;
    if (!offered_) return sendError(dev, *id, QStringLiteral("not_offered"));
    if (!authorize_(dev, perm::StorageObserve)) return sendError(dev, *id, QStringLiteral("forbidden"));

    const QString path   = req.value("path").toString();
    const qint64  offset = std::max<qint64>(0, req.value("offset").toInteger());

    QJsonArray all;
    bool       readOnly = false;
    if (path == QLatin1String("/")) {
        for (const SharedFolder& f : folders_) {
            QJsonObject entry = entryJson(QFileInfo(f.canonical), f.name);
            if (f.readOnly) entry.insert("readOnly", true);
            all.append(entry);
        }
    } else {
        QString    code;
        const auto resolved = resolve(path, &code);
        if (!resolved) return sendError(dev, *id, code);
        const QFileInfo info(resolved->second);
        if (!info.isDir()) return sendError(dev, *id, QStringLiteral("not_a_dir"));
        if (!info.isReadable()) return sendError(dev, *id, QStringLiteral("denied"));
        all      = listing(resolved->second, *resolved->first, offset == 0);
        readOnly = resolved->first->readOnly;
    }

    QJsonArray page;
    for (qint64 i = offset; i < all.size() && page.size() < kPageSize; ++i) page.append(all.at(i));
    sendReliably(dev, {
                          {"t", "fs.list.r"},
                          {"id", qint64(*id)},
                          {"path", path},
                          {"offset", offset},
                          {"total", qint64(all.size())},
                          {"readOnly", readOnly},
                          {"entries", page},
                      });
}

void StorageService::serveRead(const uconnect::DevId& dev, const QJsonObject& req) {
    const auto id = idOf(req);
    if (!id) return;
    if (!offered_) return sendError(dev, *id, QStringLiteral("not_offered"));
    if (!authorize_(dev, perm::StorageRead)) return sendError(dev, *id, QStringLiteral("forbidden"));

    QString    code;
    const auto resolved = resolve(req.value("path").toString(), &code);
    if (!resolved) return sendError(dev, *id, code);
    const QFileInfo info(resolved->second);
    if (info.isDir()) return sendError(dev, *id, QStringLiteral("not_a_file"));
    if (!info.isFile()) return sendError(dev, *id, QStringLiteral("not_found"));

    const auto key = std::make_tuple(dev, *id, false);
    if (outgoing_.contains(key)) return sendError(dev, *id, QStringLiteral("bad_request"));
    const auto mine = std::count_if(outgoing_.begin(), outgoing_.end(),
                                    [&](const auto& kv) { return kv.second.dev == dev && !kv.second.upload; });
    if (static_cast<size_t>(mine) >= kMaxOutgoingPerPeer) return sendError(dev, *id, QStringLiteral("busy"));

    auto file = std::make_unique<QFile>(resolved->second);
    if (!file->open(QIODevice::ReadOnly)) return sendError(dev, *id, QStringLiteral("denied"));

    Outgoing o;
    o.dev  = dev;
    o.id   = *id;
    // Resuming: the bytes before `offset` are hashed but not sent, so the
    // digest at the end still covers the whole file.
    qint64 offset = req.value("offset").toInteger(0);
    if (offset < 0 || offset > info.size()) offset = 0;
    o.file = std::move(file);
    o.hash = newHash();
    o.skip = offset;
    o.queue.push_back(encodeControl({{"t", "fs.read.r"}, {"id", qint64(*id)}, {"size", info.size()}, {"offset", offset}}));
    outgoing_.emplace(key, std::move(o));
    if (!pumpTimer_->isActive()) pumpTimer_->start(0);
    if (offset == 0) emit activity(dev, QStringLiteral("download"), req.value("path").toString(), {});  // not again on resume
}

void StorageService::sendError(const uconnect::DevId& dev, quint32 id, const QString& code) {
    sendReliably(dev, {{"t", "fs.err"}, {"id", qint64(id)}, {"code", code}, {"error", errorText(code)}});
}

// Sends file chunks round-robin across transfers until the budget for this
// turn is spent or every peer's queue is full, then yields to the event loop.
void StorageService::pump() {
    for (auto it = replies_.begin(); it != replies_.end();) {
        if (Clock::now() > it->deadline || send_(it->dev, it->message)) it = replies_.erase(it);
        else ++it;
    }
    // Start at most four uploads per peer; additional files wait locally.
    std::vector<quint32> unreadable;
    for (auto& [id, u] : uploads_) {
        if (u.requested) continue;
        const auto active = std::count_if(uploads_.begin(), uploads_.end(), [&](const auto& kv) {
            return kv.second.dev == u.dev && kv.second.requested;
        });
        if (static_cast<size_t>(active) >= kMaxOutgoingPerPeer) {
            u.deadline = Clock::now() + kRequestTimeout;
            continue;
        }
        auto& o = outgoing_.at(std::make_tuple(u.dev, id, true));
        if (!o.file) {
            o.file = std::make_unique<QFile>(u.localPath);
            if (!o.file->open(QIODevice::ReadOnly)) {
                unreadable.push_back(id);
                continue;
            }
            u.total = o.file->size();
            o.hash  = newHash();
        }
        if (!send_(u.dev, encodeControl({{"t", "fs.write"}, {"id", qint64(id)}, {"path", u.folder},
                                         {"name", safeFileName(QFileInfo(u.localPath).fileName())}, {"size", u.total},
                                         {"replace", u.replace}})))
            continue;
        u.requested = true;
        u.deadline = Clock::now() + kRequestTimeout;
        o.ready = false;
    }
    for (quint32 id : unreadable) finishUpload(id, {}, tr("Could not read the local file."));
    qint64 budget     = kPumpBudget;
    bool   progressed = true;
    while (budget > 0 && progressed) {
        progressed = false;
        for (auto it = outgoing_.begin(); it != outgoing_.end() && budget > 0;) {
            Outgoing& o = it->second;
            if (!o.ready) { ++it; continue; }
            if (o.skip > 0) {
                const QByteArray prefix = o.file->read(std::min(o.skip, 4 * kChunkSize));
                if (prefix.isEmpty()) o.skip = 0;  // the file shrank; the digest will not match
                o.hash->addData(prefix);
                o.skip -= prefix.size();
                o.sent += prefix.size();
                budget -= prefix.size() / 8;  // reading to hash costs less than sending
                progressed = true;
                ++it;
                continue;
            }
            if (o.queue.empty() && !o.done) {
                const QByteArray chunk = o.file->read(kChunkSize);
                if (!chunk.isEmpty()) {
                    o.hash->addData(chunk);
                    o.queue.push_back(dataFrame(o.id, chunk, o.upload));
                    o.sent += chunk.size();
                } else {
                    const bool failed = o.file->error() != QFileDevice::NoError;
                    o.file->close();
                    o.done = true;
                    o.queue.push_back(failed ? encodeControl({{"t", o.upload ? "fs.write.cancel" : "fs.err"},
                                                              {"id", qint64(o.id)},
                                                              {"code", "io"},
                                                              {"error", errorText(QStringLiteral("io"))}})
                                             : encodeControl({{"t", o.upload ? "fs.write.end" : "fs.read.end"},
                                                              {"id", qint64(o.id)},
                                                              {"sent", o.sent},
                                                              {"sha256", QString::fromLatin1(o.hash->result().toHex())}}));
                }
            }
            if (!send_(o.dev, o.queue.front())) {
                ++it;  // that peer's queue is full; try the others
                continue;
            }
            budget -= static_cast<qint64>(o.queue.front().size());
            if (o.upload) {
                auto& u = uploads_.at(o.id);
                if (o.queue.front()[0] == static_cast<uint8_t>(MessageKind::StorageUploadData))
                    u.sent += static_cast<qint64>(o.queue.front().size()) - 5;
                u.deadline = Clock::now() + kStallTimeout;
                if (Clock::now() - u.lastProgress >= kProgressEvery || o.done) {
                    u.lastProgress = Clock::now();
                    emit uploadProgress(o.id, u.sent, u.total);
                }
            }
            o.queue.pop_front();
            progressed = true;
            if (o.done && o.queue.empty()) {
                const quint32 id = o.id;
                const bool failed = o.upload && o.file->error() != QFileDevice::NoError;
                it = outgoing_.erase(it);
                if (failed) finishUpload(id, {}, tr("Could not read the local file."));
            }
            else ++it;
        }
    }
    if (!outgoing_.empty() || !replies_.empty()) pumpTimer_->start(budget > 0 ? 25 : 0);
}

// --- requester ----------------------------------------------------------------

void StorageService::upload(const uconnect::DevId& dev, const QString& localPath,
                            const QString& remoteFolder, quint32 id, bool replace) {
    // The file is opened only when its turn comes: a drop of hundreds of files
    // must not hold hundreds open (macOS allows 256 by default).
    const QFileInfo info(localPath);
    if (!info.isFile() || !info.isReadable()) {
        emit uploadFinished(id, {}, tr("Select a readable file to upload. Folders are not supported."));
        return;
    }
    Upload u;
    u.dev = dev;
    u.localPath = localPath;
    u.folder = remoteFolder;
    u.replace = replace;
    u.total = info.size();
    u.deadline = Clock::now() + kRequestTimeout;
    uploads_.emplace(id, std::move(u));
    Outgoing o;
    o.dev = dev;
    o.id = id;
    o.upload = true;
    o.ready = false;
    outgoing_.emplace(std::make_tuple(dev, id, true), std::move(o));
    emit uploadProgress(id, 0, info.size());
    updateTicker();
    if (!pumpTimer_->isActive()) pumpTimer_->start(0);
}

void StorageService::serveWrite(const uconnect::DevId& dev, const QJsonObject& req) {
    const auto id = idOf(req);
    if (!id) return;
    const auto key = std::make_pair(dev, *id);
    if (incoming_.contains(key)) return failIncoming(dev, *id, QStringLiteral("bad_request"));
    if (!offered_) return failIncoming(dev, *id, QStringLiteral("not_offered"));
    if (!authorize_(dev, perm::StorageUpload)) return failIncoming(dev, *id, QStringLiteral("forbidden"));
    const auto active = std::count_if(incoming_.begin(), incoming_.end(), [&](const auto& kv) {
        return kv.first.first == dev;
    });
    if (static_cast<size_t>(active) >= kMaxOutgoingPerPeer)
        return failIncoming(dev, *id, QStringLiteral("busy"));

    const QString path = req.value("path").toString();
    const QString name = req.value("name").toString();
    const qint64 total = req.value("size").toInteger(-1);
    if (total < 0 || !isAcceptableName(name))
        return failIncoming(dev, *id, QStringLiteral("bad_request"));
    const bool replace = req.value("replace").toBool();
    if (replace && !authorize_(dev, perm::StorageWrite))
        return failIncoming(dev, *id, QStringLiteral("forbidden"));
    if (uploadLimit_ > 0 && total > uploadLimit_) return failIncoming(dev, *id, QStringLiteral("too_large"));
    QString code;
    const auto resolved = resolve(path, &code);
    if (!resolved) return failIncoming(dev, *id, code);
    if (resolved->first->readOnly) return failIncoming(dev, *id, QStringLiteral("read_only"));
    if (!QFileInfo(resolved->second).isDir()) return failIncoming(dev, *id, QStringLiteral("not_a_dir"));
    if (!hasRoomFor(resolved->second, total)) return failIncoming(dev, *id, QStringLiteral("no_space"));

    Incoming in;
    in.folder = resolved->second;
    in.root = resolved->first->canonical;
    in.name = name;
    in.virtualFolder = QLatin1Char('/') + path.split(QLatin1Char('/'), Qt::SkipEmptyParts).join(QLatin1Char('/'));
    in.total = total;
    in.replace = replace;
    in.deadline = Clock::now() + kStallTimeout;
    in.hash = newHash();
    in.file = std::make_unique<QTemporaryFile>(QDir(in.folder).filePath(QStringLiteral(".uconnect-upload-XXXXXX.part")));
    if (!in.file->open()) return failIncoming(dev, *id, QStringLiteral("io"));
    incoming_.emplace(key, std::move(in));
    updateTicker();
    sendReliably(dev, {{"t", "fs.write.r"}, {"id", qint64(*id)}});
}

bool StorageService::hasRoomFor(const QString& folder, qint64 bytes) const {
    const QStorageInfo disk(folder);
    if (!disk.isValid()) return true;  // unknown: let the write itself fail if it must
    qint64 pending = 0;
    for (const auto& [key, in] : incoming_)
        if (QStorageInfo(in.folder).device() == disk.device()) pending += in.total - in.received;
    return disk.bytesAvailable() - pending - bytes >= minimumFree_;
}

void StorageService::sendReliably(const uconnect::DevId& dev, const QJsonObject& reply) {
    auto message = encodeControl(reply);
    if (send_(dev, message)) return;
    // Bound retries even if a peer keeps requesting while its queue is full.
    if (replies_.size() >= 256) return;
    replies_.push_back({dev, std::move(message), Clock::now() + kRequestTimeout});
    if (!pumpTimer_->isActive()) pumpTimer_->start(25);
}

void StorageService::failIncoming(const uconnect::DevId& dev, quint32 id, const QString& code) {
    incoming_.erase(std::make_pair(dev, id));  // QTemporaryFile removes the partial file
    updateTicker();
    sendReliably(dev, {{"t", "fs.write.err"}, {"id", qint64(id)}, {"code", code}});
}

void StorageService::onWriteData(const uconnect::DevId& dev, std::span<const uint8_t> message) {
    if (message.size() < 5) return;
    const quint32 id = (quint32{message[1]} << 24) | (quint32{message[2]} << 16) |
                       (quint32{message[3]} << 8) | quint32{message[4]};
    const auto it = incoming_.find(std::make_pair(dev, id));
    if (it == incoming_.end()) return;
    if (!offered_ || !authorize_(dev, perm::StorageUpload))
        return failIncoming(dev, id, QStringLiteral("forbidden"));
    Incoming& in = it->second;
    const auto bytes = message.subspan(5);
    const qint64 n = static_cast<qint64>(bytes.size());
    if (n > kChunkSize || n > in.total - in.received)
        return failIncoming(dev, id, QStringLiteral("bad_request"));
    if (in.file->write(reinterpret_cast<const char*>(bytes.data()), n) != n)
        return failIncoming(dev, id, QStringLiteral("io"));
    in.hash->addData(QByteArrayView(reinterpret_cast<const char*>(bytes.data()), n));
    in.received += n;
    in.deadline = Clock::now() + kStallTimeout;
}

void StorageService::onWriteEnd(const uconnect::DevId& dev, const QJsonObject& msg) {
    const auto id = idOf(msg);
    const auto it = id ? incoming_.find(std::make_pair(dev, *id)) : incoming_.end();
    if (it == incoming_.end()) return;
    if (!offered_ || !authorize_(dev, perm::StorageUpload))
        return failIncoming(dev, *id, QStringLiteral("forbidden"));
    Incoming& in = it->second;
    if (in.received != in.total || msg.value("sent").toInteger(-1) != in.received)
        return failIncoming(dev, *id, QStringLiteral("bad_request"));
    if (!digestMatches(msg, *in.hash)) return failIncoming(dev, *id, QStringLiteral("corrupt"));
    // Re-resolve before saving: a changed share or link must not redirect a write.
    QString code;
    const auto resolved = resolve(in.virtualFolder, &code);
    if (!resolved || resolved->second.compare(in.folder, kPathCase) != 0 ||
        resolved->first->canonical.compare(in.root, kPathCase) != 0 ||
        realPath(in.file->fileName()).isEmpty() || !isWithin(realPath(in.file->fileName()), in.root))
        return failIncoming(dev, *id, QStringLiteral("denied"));
    if (!in.file->flush()) return failIncoming(dev, *id, QStringLiteral("io"));
    QString finalPath;
    const bool replaced = in.replace && QFileInfo::exists(QDir(in.folder).filePath(in.name));
    if (in.replace) {
        if (!authorize_(dev, perm::StorageWrite)) return failIncoming(dev, *id, QStringLiteral("forbidden"));
        finalPath = QDir(in.folder).filePath(in.name);
        if (QFileInfo(finalPath).isDir()) return failIncoming(dev, *id, QStringLiteral("exists"));
        // QTemporaryFile keeps a hold on its file that stops Windows moving it
        // over another, even after close(); give it a plain name first.
        const QString staged = uniquePath(in.folder, QStringLiteral(".uconnect-replace-") + in.name);
        if (!in.file->rename(staged)) return failIncoming(dev, *id, QStringLiteral("io"));
        in.file->setAutoRemove(false);
        in.file.reset();
        if (!replaceWith(staged, finalPath)) {
            QFile::remove(staged);
            return failIncoming(dev, *id, QStringLiteral("io"));
        }
    } else {
        finalPath = uniquePath(in.folder, in.name);
        if (!in.file->rename(finalPath)) return failIncoming(dev, *id, QStringLiteral("io"));
        in.file->setAutoRemove(false);
    }
    const QString remotePath = in.virtualFolder + QLatin1Char('/') + QFileInfo(finalPath).fileName();
    incoming_.erase(it);
    cachedPath_.clear();
    updateTicker();
    sendReliably(dev, {{"t", "fs.write.done"}, {"id", qint64(*id)}, {"path", remotePath}});
    emit activity(dev, QStringLiteral("upload"), remotePath, replaced ? tr("replaced the file there") : QString());
}

void StorageService::onWriteReply(const uconnect::DevId& dev, const QJsonObject& msg) {
    const auto id = idOf(msg);
    const auto it = id ? uploads_.find(*id) : uploads_.end();
    if (it == uploads_.end() || it->second.dev != dev || !it->second.requested) return;
    const QString type = msg.value("t").toString();
    if (type == QLatin1String("fs.write.r")) {
        const auto out = outgoing_.find(std::make_tuple(dev, *id, true));
        if (out == outgoing_.end() || out->second.ready) return;
        out->second.ready = true;
        it->second.deadline = Clock::now() + kStallTimeout;
        if (!pumpTimer_->isActive()) pumpTimer_->start(0);
    } else if (type == QLatin1String("fs.write.done")) {
        // A completion is valid only after every byte and the end frame were sent.
        if (outgoing_.contains(std::make_tuple(dev, *id, true)) || it->second.sent != it->second.total) return;
        emit uploadProgress(*id, it->second.sent, it->second.total);
        finishUpload(*id, msg.value("path").toString(), {});
    } else {
        finishUpload(*id, {}, errorText(msg.value("code").toString()));
    }
}

void StorageService::finishUpload(quint32 id, const QString& remotePath, const QString& error) {
    auto node = uploads_.extract(id);
    if (node.empty()) return;
    outgoing_.erase(std::make_tuple(node.mapped().dev, id, true));
    updateTicker();
    if (!outgoing_.empty() && !pumpTimer_->isActive()) pumpTimer_->start(0);
    emit uploadFinished(id, remotePath, error);
}

// --- write operations ----------------------------------------------------------

std::optional<StorageService::Entry> StorageService::resolveEntry(const QString& path, QString* code) const {
    const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (!path.startsWith(QLatin1Char('/')) || parts.isEmpty() ||
        !std::all_of(parts.begin(), parts.end(), isValidComponent)) {
        *code = QStringLiteral("bad_request");
        return std::nullopt;
    }
    if (parts.size() == 1) {
        *code = QStringLiteral("root");
        return std::nullopt;
    }
    const auto folder = std::find_if(folders_.begin(), folders_.end(),
                                     [&](const SharedFolder& f) { return f.name == parts.first(); });
    if (folder == folders_.end()) {
        *code = QStringLiteral("not_found");
        return std::nullopt;
    }
    // The parent is resolved and checked like any other path; the entry itself
    // is then addressed inside it, so a link is acted on, not followed.
    const QStringList between = parts.mid(1, parts.size() - 2);
    const QString     parent  = realPath(between.isEmpty()
                                             ? folder->canonical
                                             : folder->canonical + QLatin1Char('/') + between.join(QLatin1Char('/')));
    if (parent.isEmpty()) {
        *code = QStringLiteral("not_found");
        return std::nullopt;
    }
    if (!isWithin(parent, folder->canonical)) {
        *code = QStringLiteral("denied");
        return std::nullopt;
    }
    const QString   entry = QDir(parent).filePath(parts.last());
    const QFileInfo info(entry);
    if (!info.exists() && !info.isSymLink()) {
        *code = QStringLiteral("not_found");
        return std::nullopt;
    }
    return Entry{&*folder, parent, entry, virtualPath(parts.mid(0, parts.size() - 1))};
}

void StorageService::serveRename(const uconnect::DevId& dev, const QJsonObject& req) {
    const auto id = idOf(req);
    if (!id) return;
    if (!offered_) return sendError(dev, *id, QStringLiteral("not_offered"));
    if (!authorize_(dev, perm::StorageWrite)) return sendError(dev, *id, QStringLiteral("forbidden"));

    QString    code;
    const auto e = resolveEntry(req.value("path").toString(), &code);
    if (!e) return sendError(dev, *id, code);
    if (e->folder->readOnly) return sendError(dev, *id, QStringLiteral("read_only"));
    const QString name = req.value("name").toString();
    if (!isAcceptableName(name)) return sendError(dev, *id, QStringLiteral("bad_request"));

    const QString   target = QDir(e->parent).filePath(name);
    const QFileInfo existing(target);
    // A change of case only ("a.txt" -> "A.txt") names the same file on Windows.
    if ((existing.exists() || existing.isSymLink()) && target.compare(e->path, kPathCase) != 0)
        return sendError(dev, *id, QStringLiteral("exists"));
    if (!QDir().rename(e->path, target)) return sendError(dev, *id, QStringLiteral("io"));

    cachedPath_.clear();
    sendReliably(dev, {{"t", "fs.done"}, {"id", qint64(*id)},
                         {"path", e->virtualParent + QLatin1Char('/') + name}});
    emit activity(dev, QStringLiteral("rename"), req.value("path").toString(), name);
}

void StorageService::serveDelete(const uconnect::DevId& dev, const QJsonObject& req) {
    const auto id = idOf(req);
    if (!id) return;
    if (!offered_) return sendError(dev, *id, QStringLiteral("not_offered"));
    if (!authorize_(dev, perm::StorageWrite)) return sendError(dev, *id, QStringLiteral("forbidden"));

    QString    code;
    const auto e = resolveEntry(req.value("path").toString(), &code);
    if (!e) return sendError(dev, *id, code);
    if (e->folder->readOnly) return sendError(dev, *id, QStringLiteral("read_only"));

    const bool trashed = useTrash_ && QFile::moveToTrash(e->path);
    if (!trashed && !removeEntry(QFileInfo(e->path))) return sendError(dev, *id, QStringLiteral("io"));

    cachedPath_.clear();
    sendReliably(dev, {{"t", "fs.done"}, {"id", qint64(*id)},
                         {"path", e->virtualParent + QLatin1Char('/') + QFileInfo(e->path).fileName()},
                         {"trashed", trashed}});
    emit activity(dev, QStringLiteral("delete"), e->virtualParent + QLatin1Char('/') + QFileInfo(e->path).fileName(),
                  trashed ? tr("moved to the trash") : QString());
}

void StorageService::serveMkdir(const uconnect::DevId& dev, const QJsonObject& req) {
    const auto id = idOf(req);
    if (!id) return;
    if (!offered_) return sendError(dev, *id, QStringLiteral("not_offered"));
    if (!authorize_(dev, perm::StorageUpload)) return sendError(dev, *id, QStringLiteral("forbidden"));

    const QString path = req.value("path").toString();
    const QString name = req.value("name").toString();
    if (!isAcceptableName(name)) return sendError(dev, *id, QStringLiteral("bad_request"));
    QString    code;
    const auto resolved = resolve(path, &code);
    if (!resolved) return sendError(dev, *id, code);
    if (resolved->first->readOnly) return sendError(dev, *id, QStringLiteral("read_only"));
    if (!QFileInfo(resolved->second).isDir()) return sendError(dev, *id, QStringLiteral("not_a_dir"));

    const QString parent = virtualPath(path.split(QLatin1Char('/'), Qt::SkipEmptyParts));
    // "existing": a folder upload merging into a folder of the same name. It
    // is reused only if it resolves inside the share like any other path.
    if (req.value("existing").toBool()) {
        QString    ignored;
        const auto there = resolve(parent + QLatin1Char('/') + name, &ignored);
        if (there && QFileInfo(there->second).isDir())
            return sendReliably(dev, {{"t", "fs.done"}, {"id", qint64(*id)},
                                        {"path", parent + QLatin1Char('/') + name}, {"existed", true}});
    }

    const QString target = uniquePath(resolved->second, name);
    if (!QDir().mkdir(target)) return sendError(dev, *id, QStringLiteral("io"));

    cachedPath_.clear();
    sendReliably(dev, {{"t", "fs.done"}, {"id", qint64(*id)},
                         {"path", parent + QLatin1Char('/') + QFileInfo(target).fileName()}});
    emit activity(dev, QStringLiteral("mkdir"), parent + QLatin1Char('/') + QFileInfo(target).fileName(), {});
}

size_t StorageService::watchSignature(const QString& virtualPath) const {
    size_t h = qHash(virtualPath);
    if (virtualPath == QLatin1String("/")) {
        for (const SharedFolder& f : folders_) h = qHashMulti(h, f.name);
        return h | 1;  // never 0
    }
    QString    code;
    const auto resolved = resolve(virtualPath, &code);
    if (!resolved || !QFileInfo(resolved->second).isDir()) return 0;
    const QFileInfoList infos = QDir(resolved->second).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    for (const QFileInfo& fi : infos) {
        // Uploads in progress are not listed, so their growth is no change.
        const bool partial = std::any_of(incoming_.begin(), incoming_.end(), [&](const auto& kv) {
            return kv.second.file && kv.second.file->fileName().compare(fi.absoluteFilePath(), kPathCase) == 0;
        });
        if (!partial) h = qHashMulti(h, fi.fileName(), fi.isDir(), fi.size(), fi.lastModified().toMSecsSinceEpoch());
    }
    return h | 1;
}

void StorageService::serveWatch(const uconnect::DevId& dev, const QJsonObject& req) {
    const auto id = idOf(req);
    if (!id || !offered_ || !authorize_(dev, perm::StorageObserve)) return;
    const auto mine = std::count_if(watches_.begin(), watches_.end(), [&](const auto& kv) { return kv.first.first == dev; });
    if (static_cast<size_t>(mine) >= kMaxWatchesPerPeer) return;
    const QString path      = req.value("path").toString();
    const size_t  signature = watchSignature(path);
    if (signature == 0) return;
    watches_[std::make_pair(dev, *id)] = Watch{path, signature};
    if (!watchTimer_->isActive()) watchTimer_->start();
}

// Tells watchers about folders whose listing changed since the last look,
// and about watches that no longer resolve or are no longer allowed (those
// end here).
void StorageService::pollWatches() {
    std::map<QString, size_t> seen;  // each folder is read once per round
    for (auto it = watches_.begin(); it != watches_.end();) {
        const auto [dev, id] = it->first;
        Watch& w = it->second;
        if (!offered_ || !authorize_(dev, perm::StorageObserve)) {
            it = watches_.erase(it);
            continue;
        }
        auto sig = seen.find(w.path);
        if (sig == seen.end()) sig = seen.emplace(w.path, watchSignature(w.path)).first;
        if (sig->second != w.signature) {
            cachedPath_.clear();
            send_(dev, encodeControl({{"t", "fs.changed"}, {"id", qint64(id)}, {"path", w.path}}));
        }
        w.signature = sig->second;
        if (sig->second == 0) it = watches_.erase(it);
        else ++it;
    }
    if (watches_.empty()) watchTimer_->stop();
}

void StorageService::serveTransfer(const uconnect::DevId& dev, const QJsonObject& req, bool copy) {
    const auto id = idOf(req);
    if (!id) return;
    if (!offered_) return sendError(dev, *id, QStringLiteral("not_offered"));
    if (!authorize_(dev, copy ? perm::StorageUpload : perm::StorageWrite))
        return sendError(dev, *id, QStringLiteral("forbidden"));

    QString    code;
    const auto e = resolveEntry(req.value("path").toString(), &code);
    if (!e) return sendError(dev, *id, code);
    const QString to   = req.value("to").toString();
    const auto    dest = resolve(to, &code);
    if (!dest) return sendError(dev, *id, code);
    if (!QFileInfo(dest->second).isDir()) return sendError(dev, *id, QStringLiteral("not_a_dir"));
    if ((!copy && e->folder->readOnly) || dest->first->readOnly) return sendError(dev, *id, QStringLiteral("read_only"));

    const QFileInfo source(e->path);
    const bool      link = source.isSymLink() || source.isJunction();
    if (!link && source.isDir() && isWithin(dest->second, e->path))
        return sendError(dev, *id, QStringLiteral("inside"));
    const QString destVirtual = virtualPath(to.split(QLatin1Char('/'), Qt::SkipEmptyParts));
    if (!copy && dest->second.compare(e->parent, kPathCase) == 0)  // already there
        return sendReliably(dev, {{"t", "fs.done"}, {"id", qint64(*id)},
                                    {"path", destVirtual + QLatin1Char('/') + source.fileName()}});
    const QString target        = uniquePath(dest->second, source.fileName());
    const QString targetVirtual = destVirtual + QLatin1Char('/') + QFileInfo(target).fileName();

    if (!copy && !alwaysCopyMoves_ && sameVolume(e->parent, dest->second)) {
        if (!QDir().rename(e->path, target)) return sendError(dev, *id, QStringLiteral("io"));
        cachedPath_.clear();
        sendReliably(dev, {{"t", "fs.done"}, {"id", qint64(*id)}, {"path", targetVirtual}});
        emit activity(dev, QStringLiteral("move"), e->virtualParent + QLatin1Char('/') + source.fileName(), targetVirtual);
        return;
    }

    // A copy, or a move between drives (a copy and a delete), can take a
    // while, so it runs on a pool thread; the requester hears fs.progress.
    if (link) return sendError(dev, *id, QStringLiteral("denied"));  // links are not followed
    const auto key = std::make_pair(dev, *id);
    if (jobs_.contains(key)) return sendError(dev, *id, QStringLiteral("bad_request"));
    const auto mine = std::count_if(jobs_.begin(), jobs_.end(), [&](const auto& kv) { return kv.first.first == dev; });
    if (static_cast<size_t>(mine) >= kMaxOutgoingPerPeer) return sendError(dev, *id, QStringLiteral("busy"));

    auto state = std::make_shared<JobState>();
    jobs_.emplace(key, Job{state, targetVirtual, Clock::now(), e->virtualParent + QLatin1Char('/') + source.fileName(), copy});
    updateTicker();
    const qint64 available = QStorageInfo(dest->second).bytesAvailable() - minimumFree_;
    runOffThread<QString>(
        this,
        [state, from = e->path, target, move = !copy, available] {
            state->total = treeSize(QFileInfo(from));
            if (state->total > available) return QStringLiteral("no_space");
            if (!copyTree(QFileInfo(from), target, state->done, state->cancel)) {
                removeEntry(QFileInfo(target));  // whatever was copied before it failed
                return QStringLiteral("io");
            }
            if (move && !removeEntry(QFileInfo(from)))
                return QStringLiteral("io");  // copied, but not all of the original could be removed
            return QString();
        },
        [this, dev, id = *id](std::optional<QString> result) {
            finishJob(dev, id, result.value_or(QStringLiteral("io")));  // none: the pool was shutting down
        });
}

void StorageService::finishJob(const uconnect::DevId& dev, quint32 id, const QString& code) {
    auto node = jobs_.extract(std::make_pair(dev, id));
    if (node.empty()) return;
    cachedPath_.clear();
    updateTicker();
    const Job& job = node.mapped();
    if (code.isEmpty()) {
        sendReliably(dev, {{"t", "fs.done"}, {"id", qint64(id)}, {"path", job.target}});
        emit activity(dev, job.copy ? QStringLiteral("copy") : QStringLiteral("move"), job.source, job.target);
    } else
        sendReliably(dev, {{"t", "fs.err"}, {"id", qint64(id)}, {"code", code}, {"error", errorText(code)}});
}

void StorageService::operation(const uconnect::DevId& dev, QJsonObject request, quint32 id) {
    request.insert("id", qint64(id));
    ops_[id] = PendingOp{dev, Clock::now() + kRequestTimeout};
    updateTicker();
    if (!send_(dev, encodeControl(request))) finishOperation(id, {}, tr("Could not reach the device."));
}

void StorageService::watch(const uconnect::DevId& dev, const QString& path, quint32 id) {
    if (send_(dev, encodeControl({{"t", "fs.watch"}, {"id", qint64(id)}, {"path", path}})))
        watching_[id] = Watching{dev, path};
}

void StorageService::unwatch(quint32 id) {
    auto node = watching_.extract(id);
    if (!node.empty()) send_(node.mapped().dev, encodeControl({{"t", "fs.unwatch"}, {"id", qint64(id)}}));
}

void StorageService::finishOperation(quint32 id, const QString& path, const QString& error) {
    if (ops_.erase(id) == 0) return;
    updateTicker();
    emit operationFinished(id, path, error);
}

void StorageService::list(const uconnect::DevId& dev, const QString& path, quint32 id) {
    lists_[id] = PendingList{dev, path, {}, Clock::now() + kRequestTimeout};
    updateTicker();
    if (!send_(dev, encodeControl({{"t", "fs.list"}, {"id", qint64(id)}, {"path", path}, {"offset", 0}})))
        finishList(id, tr("Could not reach the device."));
}

void StorageService::download(const uconnect::DevId& dev, const QString& remotePath,
                              const QString& folder, quint32 id) {
    Download d;
    d.dev        = dev;
    d.remotePath = remotePath;
    d.folder     = folder;
    d.name       = safeFileName(remotePath.section(QLatin1Char('/'), -1));
    d.partPath   = partPathFor(folder, d.name, remotePath);
    // Two downloads of one file into one folder cannot share a part file.
    d.resumable  = std::none_of(downloads_.begin(), downloads_.end(), [&](const auto& kv) {
        return kv.second.partPath.compare(d.partPath, kPathCase) == 0;
    });
    if (!d.resumable) d.partPath.clear();  // a unique one is picked when it starts
    downloads_.emplace(id, std::move(d));
    updateTicker();
    startQueuedDownloads(dev);
}

void StorageService::startQueuedDownloads(const uconnect::DevId& dev) {
    auto active = static_cast<size_t>(std::count_if(downloads_.begin(), downloads_.end(), [&](const auto& kv) {
        return kv.second.dev == dev && kv.second.requested;
    }));
    std::vector<quint32> start;
    for (auto& [id, d] : downloads_) {
        if (active >= kMaxOutgoingPerPeer) break;
        if (d.dev != dev || d.requested) continue;
        d.requested = true;
        d.deadline  = Clock::now() + kRequestTimeout;
        ++active;
        start.push_back(id);
    }
    for (quint32 id : start) {
        Download& d = downloads_.at(id);
        const qint64 have = d.resumable ? QFileInfo(d.partPath).size() : 0;
        if (have <= 0) {
            sendRead(id);
            continue;
        }
        // Part of it arrived before: hash that off this thread, then resume.
        d.deadline = Clock::now() + std::chrono::hours(1);  // a large part takes a while
        runOffThread<std::pair<std::shared_ptr<QCryptographicHash>, qint64>>(
            this,
            [path = d.partPath, have] {
                auto  hash = std::make_shared<QCryptographicHash>(QCryptographicHash::Sha256);
                QFile part(path);
                if (!part.open(QIODevice::ReadOnly)) return std::make_pair(hash, qint64(0));
                qint64 read = 0;
                while (read < have) {
                    const QByteArray chunk = part.read(std::min<qint64>(have - read, 4 * kChunkSize));
                    if (chunk.isEmpty()) break;
                    hash->addData(chunk);
                    read += chunk.size();
                }
                return std::make_pair(hash, read);
            },
            [this, id](auto result) {
                const auto it = downloads_.find(id);
                if (it == downloads_.end()) return;  // canceled or failed meanwhile
                if (result && result->second > 0) {
                    it->second.hash       = std::make_unique<QCryptographicHash>(std::move(*result->first));
                    it->second.resumeFrom = result->second;
                }
                sendRead(id);
            });
    }
}

void StorageService::sendRead(quint32 id) {
    Download& d = downloads_.at(id);
    QJsonObject req{{"t", "fs.read"}, {"id", qint64(id)}, {"path", d.remotePath}};
    if (d.resumeFrom > 0) req.insert("offset", d.resumeFrom);
    d.deadline = Clock::now() + kRequestTimeout;
    if (!send_(d.dev, encodeControl(req))) finishDownload(id, tr("Could not reach the device."), true);
}

void StorageService::cancel(quint32 id) {
    if (auto it = uploads_.find(id); it != uploads_.end()) {
        if (it->second.requested)
            sendReliably(it->second.dev, {{"t", "fs.write.cancel"}, {"id", qint64(id)}});
        finishUpload(id, {}, tr("Canceled"));
    }
    if (auto it = downloads_.find(id); it != downloads_.end()) {
        if (it->second.requested) sendReliably(it->second.dev, {{"t", "fs.cancel"}, {"id", qint64(id)}});
        finishDownload(id, tr("Canceled"));
    }
    lists_.erase(id);
    updateTicker();
}

void StorageService::onListPage(const uconnect::DevId& dev, const QJsonObject& msg) {
    const auto id = idOf(msg);
    const auto it = id ? lists_.find(*id) : lists_.end();
    if (it == lists_.end() || it->second.dev != dev) return;
    PendingList& p = it->second;
    p.readOnly = msg.value("readOnly").toBool();

    const QJsonArray entries = msg.value("entries").toArray();
    for (const QJsonValue& v : entries) {
        const QJsonObject e    = v.toObject();
        const QString     name = e.value("name").toString();
        if (name.isEmpty() || name.contains(QLatin1Char('/')) || name == QLatin1String(".") ||
            name == QLatin1String(".."))
            continue;
        p.entries.push_back(QVariantMap{
            {"name", name},
            {"dir", e.value("dir").toBool()},
            {"size", e.value("size").toDouble()},
            {"mtime", e.value("mtime").toDouble()},
            {"readOnly", e.value("readOnly").toBool()},
        });
    }
    if (p.entries.size() > kMaxEntries) return finishList(*id, tr("This folder has too many entries to show."));

    const qint64 next = msg.value("offset").toInteger() + entries.size();
    if (!entries.isEmpty() && next < msg.value("total").toInteger()) {
        p.deadline = Clock::now() + kRequestTimeout;
        if (!send_(dev, encodeControl({{"t", "fs.list"}, {"id", qint64(*id)}, {"path", p.path}, {"offset", next}})))
            finishList(*id, tr("Could not reach the device."));
        return;
    }
    finishList(*id, {});
}

void StorageService::onReadStart(const uconnect::DevId& dev, const QJsonObject& msg) {
    const auto id = idOf(msg);
    const auto it = id ? downloads_.find(*id) : downloads_.end();
    if (it == downloads_.end() || it->second.dev != dev || it->second.file) return;
    Download& d = it->second;

    if (!QDir().mkpath(d.folder)) {
        sendReliably(dev, {{"t", "fs.cancel"}, {"id", qint64(*id)}});
        return finishDownload(*id, tr("Cannot create the download folder."));
    }
    // The device resumes where asked, or from the start (offset 0).
    const qint64 offset  = msg.value("offset").toInteger(0);
    const bool   resumed = offset > 0 && offset == d.resumeFrom && d.hash;
    if (!resumed) {
        d.hash     = newHash();
        d.received = 0;
    } else {
        d.received = offset;
    }
    if (d.partPath.isEmpty()) d.partPath = uniquePath(d.folder, d.name + QStringLiteral(".part"));
    d.file = std::make_unique<QFile>(d.partPath);
    // Exactly the bytes that were hashed, then append to them.
    if ((resumed && !d.file->resize(offset)) ||
        !d.file->open(resumed ? QIODevice::WriteOnly | QIODevice::Append : QIODevice::WriteOnly)) {
        d.file.reset();
        sendReliably(dev, {{"t", "fs.cancel"}, {"id", qint64(*id)}});
        return finishDownload(*id, tr("Cannot write to the download folder."));
    }
    d.total        = msg.value("size").toInteger(-1);
    d.deadline     = Clock::now() + kStallTimeout;
    d.lastProgress = Clock::now();
    emit downloadProgress(*id, d.received, d.total);
}

void StorageService::onReadData(const uconnect::DevId& dev, std::span<const uint8_t> message) {
    if (message.size() < 5) return;
    const quint32 id = (quint32{message[1]} << 24) | (quint32{message[2]} << 16) |
                       (quint32{message[3]} << 8) | quint32{message[4]};
    const auto it = downloads_.find(id);
    if (it == downloads_.end() || it->second.dev != dev || !it->second.file) return;  // e.g. canceled
    Download& d = it->second;

    const auto   bytes = message.subspan(5);
    const qint64 n     = static_cast<qint64>(bytes.size());
    if (d.file->write(reinterpret_cast<const char*>(bytes.data()), n) != n) {
        sendReliably(dev, {{"t", "fs.cancel"}, {"id", qint64(id)}});
        return finishDownload(id, tr("Could not write the file (is the disk full?)."));
    }
    d.hash->addData(QByteArrayView(reinterpret_cast<const char*>(bytes.data()), n));
    d.received += n;
    const auto now = Clock::now();
    d.deadline     = now + kStallTimeout;
    if (now - d.lastProgress >= kProgressEvery) {
        d.lastProgress = now;
        emit downloadProgress(id, d.received, d.total);
    }
}

void StorageService::onReadEnd(const uconnect::DevId& dev, const QJsonObject& msg) {
    const auto id = idOf(msg);
    const auto it = id ? downloads_.find(*id) : downloads_.end();
    if (it == downloads_.end() || it->second.dev != dev || !it->second.file) return;
    Download& d = it->second;

    d.file->close();
    if (msg.value("sent").toInteger(-1) != d.received)
        return finishDownload(*id, tr("The transfer was incomplete."));
    if (!digestMatches(msg, *d.hash)) return finishDownload(*id, errorText(QStringLiteral("corrupt")));

    const QString finalPath = uniquePath(d.folder, d.name);
    if (!QFile::rename(d.partPath, finalPath)) return finishDownload(*id, tr("Could not save the file."));

    const qint64 received = d.received;
    downloads_.erase(it);
    updateTicker();
    emit downloadProgress(*id, received, received);
    emit downloadFinished(*id, finalPath, {});
    startQueuedDownloads(dev);
}

void StorageService::onError(const uconnect::DevId& dev, const QJsonObject& msg) {
    const auto id = idOf(msg);
    if (!id) return;
    const QString text = errorText(msg.value("code").toString());
    if (auto it = lists_.find(*id); it != lists_.end() && it->second.dev == dev) finishList(*id, text);
    if (auto it = downloads_.find(*id); it != downloads_.end() && it->second.dev == dev) finishDownload(*id, text);
    if (auto it = ops_.find(*id); it != ops_.end() && it->second.dev == dev) finishOperation(*id, {}, text);
}

void StorageService::finishList(quint32 id, const QString& error) {
    auto node = lists_.extract(id);
    if (node.empty()) return;
    updateTicker();
    emit listFinished(id, node.mapped().path, error.isEmpty() ? node.mapped().entries : QVariantList{}, error,
                      node.mapped().readOnly);
}

void StorageService::finishDownload(quint32 id, const QString& error, bool keepPart) {
    auto node = downloads_.extract(id);
    if (node.empty()) return;
    Download& d = node.mapped();
    if (d.file) d.file->close();
    if ((!keepPart || !d.resumable) && !d.partPath.isEmpty()) QFile::remove(d.partPath);
    updateTicker();
    emit downloadFinished(id, {}, error);
    startQueuedDownloads(d.dev);
}

// --- shared -------------------------------------------------------------------

bool StorageService::handleMessage(const uconnect::DevId& dev, std::span<const uint8_t> message) {
    if (message.empty()) return false;
    if (message[0] == static_cast<uint8_t>(MessageKind::StorageUploadData)) {
        onWriteData(dev, message);
        return true;
    }
    // Storage is the only service sending data frames so far.
    if (message[0] == static_cast<uint8_t>(MessageKind::Data)) {
        onReadData(dev, message);
        return true;
    }
    const auto msg = decodeControl(message);
    if (!msg) return false;
    const QString t = msg->value("t").toString();
    if (!t.startsWith(QLatin1String("fs."))) return false;

    if (t == QLatin1String("fs.list")) serveList(dev, *msg);
    else if (t == QLatin1String("fs.read")) serveRead(dev, *msg);
    else if (t == QLatin1String("fs.write")) serveWrite(dev, *msg);
    else if (t == QLatin1String("fs.rename")) serveRename(dev, *msg);
    else if (t == QLatin1String("fs.delete")) serveDelete(dev, *msg);
    else if (t == QLatin1String("fs.mkdir")) serveMkdir(dev, *msg);
    else if (t == QLatin1String("fs.watch")) serveWatch(dev, *msg);
    else if (t == QLatin1String("fs.unwatch")) {
        if (const auto id = idOf(*msg)) watches_.erase(std::make_pair(dev, *id));
    }
    else if (t == QLatin1String("fs.changed")) {
        const auto id = idOf(*msg);
        if (const auto it = id ? watching_.find(*id) : watching_.end(); it != watching_.end() && it->second.dev == dev)
            emit folderChanged(*id, it->second.path);
    }
    else if (t == QLatin1String("fs.move")) serveTransfer(dev, *msg, false);
    else if (t == QLatin1String("fs.copy")) serveTransfer(dev, *msg, true);
    else if (t == QLatin1String("fs.progress")) {
        const auto id = idOf(*msg);
        if (const auto it = id ? ops_.find(*id) : ops_.end(); it != ops_.end() && it->second.dev == dev) {
            it->second.deadline = Clock::now() + kStallTimeout;
            emit operationProgress(*id, msg->value("done").toInteger(), msg->value("total").toInteger(-1));
        }
    }
    else if (t == QLatin1String("fs.done")) {
        const auto id = idOf(*msg);
        if (const auto it = id ? ops_.find(*id) : ops_.end(); it != ops_.end() && it->second.dev == dev)
            finishOperation(*id, msg->value("path").toString(), {});
    }
    else if (t == QLatin1String("fs.write.end")) onWriteEnd(dev, *msg);
    else if (t == QLatin1String("fs.write.cancel")) {
        if (const auto id = idOf(*msg)) incoming_.erase(std::make_pair(dev, *id));
        updateTicker();
    }
    else if (t == QLatin1String("fs.write.r") || t == QLatin1String("fs.write.done") ||
             t == QLatin1String("fs.write.err")) onWriteReply(dev, *msg);
    else if (t == QLatin1String("fs.cancel")) {
        if (const auto id = idOf(*msg)) outgoing_.erase(std::make_tuple(dev, *id, false));
    }
    else if (t == QLatin1String("fs.list.r")) onListPage(dev, *msg);
    else if (t == QLatin1String("fs.read.r")) onReadStart(dev, *msg);
    else if (t == QLatin1String("fs.read.end")) onReadEnd(dev, *msg);
    else if (t == QLatin1String("fs.err")) onError(dev, *msg);
    return true;
}

void StorageService::peerGone(const uconnect::DevId& dev) {
    std::erase_if(outgoing_, [&](const auto& kv) { return kv.second.dev == dev; });
    std::erase_if(incoming_, [&](const auto& kv) { return kv.first.first == dev; });
    std::erase_if(replies_, [&](const Reply& r) { return r.dev == dev; });
    std::erase_if(watches_, [&](const auto& kv) { return kv.first.first == dev; });
    std::erase_if(watching_, [&](const auto& kv) { return kv.second.dev == dev; });
    std::erase_if(jobs_, [&](auto& kv) {
        if (kv.first.first != dev) return false;
        kv.second.state->cancel = true;
        return true;
    });
    std::vector<quint32> uploads;
    for (const auto& [id, u] : uploads_)
        if (u.dev == dev) uploads.push_back(id);
    for (quint32 id : uploads) finishUpload(id, {}, tr("The device disconnected."));

    std::vector<quint32> lists, downloads;
    for (const auto& [id, p] : lists_)
        if (p.dev == dev) lists.push_back(id);
    for (const auto& [id, d] : downloads_)
        if (d.dev == dev) downloads.push_back(id);
    for (quint32 id : lists) finishList(id, tr("The device disconnected."));
    for (quint32 id : downloads) finishDownload(id, tr("The device disconnected."), true);
    std::vector<quint32> ops;
    for (const auto& [id, op] : ops_)
        if (op.dev == dev) ops.push_back(id);
    for (quint32 id : ops) finishOperation(id, {}, tr("The device disconnected."));
}

void StorageService::reset() {
    outgoing_.clear();
    incoming_.clear();
    replies_.clear();
    for (auto& [key, job] : jobs_) job.state->cancel = true;
    jobs_.clear();
    watches_.clear();
    watching_.clear();
    watchTimer_->stop();
    while (!uploads_.empty()) finishUpload(uploads_.begin()->first, {}, tr("Disconnected."));
    cachedPath_.clear();
    std::vector<quint32> lists, downloads;
    for (const auto& [id, p] : lists_) lists.push_back(id);
    for (const auto& [id, d] : downloads_) downloads.push_back(id);
    for (quint32 id : lists) finishList(id, tr("Disconnected."));
    for (quint32 id : downloads) finishDownload(id, tr("Disconnected."), true);
    while (!ops_.empty()) finishOperation(ops_.begin()->first, {}, tr("Disconnected."));
}

void StorageService::checkDeadlines() {
    const auto           now = Clock::now();
    std::vector<quint32> lists, downloads;
    std::vector<quint32> uploads;
    for (const auto& [id, u] : uploads_)
        if (now > u.deadline) uploads.push_back(id);
    for (quint32 id : uploads) {
        sendReliably(uploads_.at(id).dev, {{"t", "fs.write.cancel"}, {"id", qint64(id)}});
        finishUpload(id, {}, tr("The upload stalled or the device did not respond."));
    }
    std::vector<std::pair<uconnect::DevId, quint32>> incoming;
    for (const auto& [key, in] : incoming_)
        if (now > in.deadline) incoming.push_back(key);
    for (const auto& key : incoming) failIncoming(key.first, key.second, QStringLiteral("io"));
    for (const auto& [id, p] : lists_)
        if (now > p.deadline) lists.push_back(id);
    for (const auto& [id, d] : downloads_)
        if (d.requested && now > d.deadline) downloads.push_back(id);
    for (quint32 id : lists) finishList(id, tr("No response from the device."));
    std::vector<quint32> ops;
    for (const auto& [id, op] : ops_)
        if (now > op.deadline) ops.push_back(id);
    for (quint32 id : ops) finishOperation(id, {}, tr("No response from the device."));
    for (auto& [key, job] : jobs_) {
        if (now - job.lastBeat < kJobBeatEvery) continue;
        job.lastBeat = now;
        send_(key.first, encodeControl({{"t", "fs.progress"}, {"id", qint64(key.second)},
                                        {"done", job.state->done.load()}, {"total", job.state->total.load()}}));
    }
    for (quint32 id : downloads) {
        const Download& d = downloads_.at(id);
        sendReliably(d.dev, {{"t", "fs.cancel"}, {"id", qint64(id)}});
        finishDownload(id, d.file ? tr("The transfer stalled.") : tr("No response from the device."), true);
    }
}

void StorageService::updateTicker() {
    if (lists_.empty() && downloads_.empty() && uploads_.empty() && incoming_.empty() && ops_.empty() && jobs_.empty())
        ticker_->stop();
    else if (!ticker_->isActive()) ticker_->start();
}

}  // namespace ucd
