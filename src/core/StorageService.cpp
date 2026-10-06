#include "StorageService.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTimer>

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

std::vector<uint8_t> dataFrame(quint32 id, const QByteArray& bytes) {
    std::vector<uint8_t> out(static_cast<size_t>(bytes.size()) + 5);
    out[0] = static_cast<uint8_t>(MessageKind::Data);
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
    if (code == QLatin1String("busy")) return StorageService::tr("The device is busy with other downloads; try again shortly.");
    if (code == QLatin1String("io")) return StorageService::tr("The device could not read the file.");
    if (code == QLatin1String("bad_request")) return StorageService::tr("The device rejected the request.");
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
      ticker_(new QTimer(this)) {
    pumpTimer_->setSingleShot(true);
    connect(pumpTimer_, &QTimer::timeout, this, &StorageService::pump);
    ticker_->setInterval(1s);
    connect(ticker_, &QTimer::timeout, this, &StorageService::checkDeadlines);
}

StorageService::~StorageService() = default;

// --- offerer ------------------------------------------------------------------

void StorageService::setOffered(bool offered) {
    offered_ = offered;
    if (offered_) return;
    for (const auto& [key, o] : outgoing_) sendError(o.dev, o.id, QStringLiteral("not_offered"));
    outgoing_.clear();
}

void StorageService::setSharedFolders(const QStringList& folders) {
    folders_.clear();
    cachedPath_.clear();
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
        folders_.push_back({name, canonical});
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

QJsonArray StorageService::listing(const QString& absolutePath, const SharedFolder& folder) {
    const auto now = Clock::now();
    if (cachedPath_ == absolutePath && now - cachedAt_ < kCacheFor) return cachedEntries_;

    QJsonArray out;
    const QFileInfoList infos = QDir(absolutePath).entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden,
        QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo& fi : infos) {
        if (!isValidComponent(fi.fileName())) continue;
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
    if (path == QLatin1String("/")) {
        for (const SharedFolder& f : folders_) all.append(entryJson(QFileInfo(f.canonical), f.name));
    } else {
        QString    code;
        const auto resolved = resolve(path, &code);
        if (!resolved) return sendError(dev, *id, code);
        const QFileInfo info(resolved->second);
        if (!info.isDir()) return sendError(dev, *id, QStringLiteral("not_a_dir"));
        if (!info.isReadable()) return sendError(dev, *id, QStringLiteral("denied"));
        all = listing(resolved->second, *resolved->first);
    }

    QJsonArray page;
    for (qint64 i = offset; i < all.size() && page.size() < kPageSize; ++i) page.append(all.at(i));
    send_(dev, encodeControl({
                   {"t", "fs.list.r"},
                   {"id", qint64(*id)},
                   {"path", path},
                   {"offset", offset},
                   {"total", qint64(all.size())},
                   {"entries", page},
               }));
}

void StorageService::serveRead(const uconnect::DevId& dev, const QJsonObject& req) {
    const auto id = idOf(req);
    if (!id) return;
    if (!offered_) return sendError(dev, *id, QStringLiteral("not_offered"));
    if (!authorize_(dev, perm::StorageDownload)) return sendError(dev, *id, QStringLiteral("forbidden"));

    QString    code;
    const auto resolved = resolve(req.value("path").toString(), &code);
    if (!resolved) return sendError(dev, *id, code);
    const QFileInfo info(resolved->second);
    if (info.isDir()) return sendError(dev, *id, QStringLiteral("not_a_file"));
    if (!info.isFile()) return sendError(dev, *id, QStringLiteral("not_found"));

    const auto key = std::make_pair(dev, *id);
    if (outgoing_.contains(key)) return sendError(dev, *id, QStringLiteral("bad_request"));
    const auto mine = std::count_if(outgoing_.begin(), outgoing_.end(),
                                    [&](const auto& kv) { return kv.first.first == dev; });
    if (static_cast<size_t>(mine) >= kMaxOutgoingPerPeer) return sendError(dev, *id, QStringLiteral("busy"));

    auto file = std::make_unique<QFile>(resolved->second);
    if (!file->open(QIODevice::ReadOnly)) return sendError(dev, *id, QStringLiteral("denied"));

    Outgoing o;
    o.dev  = dev;
    o.id   = *id;
    o.file = std::move(file);
    o.queue.push_back(encodeControl({{"t", "fs.read.r"}, {"id", qint64(*id)}, {"size", info.size()}}));
    outgoing_.emplace(key, std::move(o));
    if (!pumpTimer_->isActive()) pumpTimer_->start(0);
}

void StorageService::sendError(const uconnect::DevId& dev, quint32 id, const QString& code) {
    send_(dev, encodeControl({{"t", "fs.err"}, {"id", qint64(id)}, {"code", code}, {"error", errorText(code)}}));
}

// Sends file chunks round-robin across transfers until the budget for this
// turn is spent or every peer's queue is full, then yields to the event loop.
void StorageService::pump() {
    qint64 budget     = kPumpBudget;
    bool   progressed = true;
    while (budget > 0 && progressed) {
        progressed = false;
        for (auto it = outgoing_.begin(); it != outgoing_.end() && budget > 0;) {
            Outgoing& o = it->second;
            if (o.queue.empty() && !o.done) {
                const QByteArray chunk = o.file->read(kChunkSize);
                if (!chunk.isEmpty()) {
                    o.queue.push_back(dataFrame(o.id, chunk));
                    o.sent += chunk.size();
                } else {
                    const bool failed = o.file->error() != QFileDevice::NoError;
                    o.file->close();
                    o.done = true;
                    o.queue.push_back(failed ? encodeControl({{"t", "fs.err"},
                                                              {"id", qint64(o.id)},
                                                              {"code", "io"},
                                                              {"error", errorText(QStringLiteral("io"))}})
                                             : encodeControl({{"t", "fs.read.end"},
                                                              {"id", qint64(o.id)},
                                                              {"sent", o.sent}}));
                }
            }
            if (!send_(o.dev, o.queue.front())) {
                ++it;  // that peer's queue is full; try the others
                continue;
            }
            budget -= static_cast<qint64>(o.queue.front().size());
            o.queue.pop_front();
            progressed = true;
            if (o.done && o.queue.empty()) it = outgoing_.erase(it);
            else ++it;
        }
    }
    if (!outgoing_.empty()) pumpTimer_->start(budget > 0 ? 25 : 0);
}

// --- requester ----------------------------------------------------------------

void StorageService::list(const uconnect::DevId& dev, const QString& path, quint32 id) {
    lists_[id] = PendingList{dev, path, {}, Clock::now() + kRequestTimeout};
    updateTicker();
    if (!send_(dev, encodeControl({{"t", "fs.list"}, {"id", qint64(id)}, {"path", path}, {"offset", 0}})))
        finishList(id, tr("Could not reach the device."));
}

void StorageService::download(const uconnect::DevId& dev, const QString& remotePath,
                              const QString& folder, quint32 id) {
    Download d;
    d.dev      = dev;
    d.folder   = folder;
    d.name     = safeFileName(remotePath.section(QLatin1Char('/'), -1));
    d.deadline = Clock::now() + kRequestTimeout;
    downloads_.emplace(id, std::move(d));
    updateTicker();
    if (!send_(dev, encodeControl({{"t", "fs.read"}, {"id", qint64(id)}, {"path", remotePath}})))
        finishDownload(id, tr("Could not reach the device."));
}

void StorageService::cancel(quint32 id) {
    if (auto it = downloads_.find(id); it != downloads_.end()) {
        send_(it->second.dev, encodeControl({{"t", "fs.cancel"}, {"id", qint64(id)}}));
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
        send_(dev, encodeControl({{"t", "fs.cancel"}, {"id", qint64(*id)}}));
        return finishDownload(*id, tr("Cannot create the download folder."));
    }
    d.partPath = uniquePath(d.folder, d.name + QStringLiteral(".part"));
    d.file     = std::make_unique<QFile>(d.partPath);
    if (!d.file->open(QIODevice::WriteOnly)) {
        d.file.reset();
        send_(dev, encodeControl({{"t", "fs.cancel"}, {"id", qint64(*id)}}));
        return finishDownload(*id, tr("Cannot write to the download folder."));
    }
    d.total        = msg.value("size").toInteger(-1);
    d.deadline     = Clock::now() + kStallTimeout;
    d.lastProgress = Clock::now();
    emit downloadProgress(*id, 0, d.total);
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
        send_(dev, encodeControl({{"t", "fs.cancel"}, {"id", qint64(id)}}));
        return finishDownload(id, tr("Could not write the file (is the disk full?)."));
    }
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

    const QString finalPath = uniquePath(d.folder, d.name);
    if (!QFile::rename(d.partPath, finalPath)) return finishDownload(*id, tr("Could not save the file."));

    const qint64 received = d.received;
    downloads_.erase(it);
    updateTicker();
    emit downloadProgress(*id, received, received);
    emit downloadFinished(*id, finalPath, {});
}

void StorageService::onError(const uconnect::DevId& dev, const QJsonObject& msg) {
    const auto id = idOf(msg);
    if (!id) return;
    const QString text = errorText(msg.value("code").toString());
    if (auto it = lists_.find(*id); it != lists_.end() && it->second.dev == dev) finishList(*id, text);
    if (auto it = downloads_.find(*id); it != downloads_.end() && it->second.dev == dev) finishDownload(*id, text);
}

void StorageService::finishList(quint32 id, const QString& error) {
    auto node = lists_.extract(id);
    if (node.empty()) return;
    updateTicker();
    emit listFinished(id, node.mapped().path, error.isEmpty() ? node.mapped().entries : QVariantList{}, error);
}

void StorageService::finishDownload(quint32 id, const QString& error) {
    auto node = downloads_.extract(id);
    if (node.empty()) return;
    Download& d = node.mapped();
    if (d.file) {
        d.file->close();
        QFile::remove(d.partPath);
    }
    updateTicker();
    emit downloadFinished(id, {}, error);
}

// --- shared -------------------------------------------------------------------

bool StorageService::handleMessage(const uconnect::DevId& dev, std::span<const uint8_t> message) {
    if (message.empty()) return false;
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
    else if (t == QLatin1String("fs.cancel")) {
        if (const auto id = idOf(*msg)) outgoing_.erase(std::make_pair(dev, *id));
    }
    else if (t == QLatin1String("fs.list.r")) onListPage(dev, *msg);
    else if (t == QLatin1String("fs.read.r")) onReadStart(dev, *msg);
    else if (t == QLatin1String("fs.read.end")) onReadEnd(dev, *msg);
    else if (t == QLatin1String("fs.err")) onError(dev, *msg);
    return true;
}

void StorageService::peerGone(const uconnect::DevId& dev) {
    std::erase_if(outgoing_, [&](const auto& kv) { return kv.first.first == dev; });

    std::vector<quint32> lists, downloads;
    for (const auto& [id, p] : lists_)
        if (p.dev == dev) lists.push_back(id);
    for (const auto& [id, d] : downloads_)
        if (d.dev == dev) downloads.push_back(id);
    for (quint32 id : lists) finishList(id, tr("The device disconnected."));
    for (quint32 id : downloads) finishDownload(id, tr("The device disconnected."));
}

void StorageService::reset() {
    outgoing_.clear();
    cachedPath_.clear();
    std::vector<quint32> lists, downloads;
    for (const auto& [id, p] : lists_) lists.push_back(id);
    for (const auto& [id, d] : downloads_) downloads.push_back(id);
    for (quint32 id : lists) finishList(id, tr("Disconnected."));
    for (quint32 id : downloads) finishDownload(id, tr("Disconnected."));
}

void StorageService::checkDeadlines() {
    const auto           now = Clock::now();
    std::vector<quint32> lists, downloads;
    for (const auto& [id, p] : lists_)
        if (now > p.deadline) lists.push_back(id);
    for (const auto& [id, d] : downloads_)
        if (now > d.deadline) downloads.push_back(id);
    for (quint32 id : lists) finishList(id, tr("No response from the device."));
    for (quint32 id : downloads) {
        const Download& d = downloads_.at(id);
        send_(d.dev, encodeControl({{"t", "fs.cancel"}, {"id", qint64(id)}}));
        finishDownload(id, d.file ? tr("The transfer stalled.") : tr("No response from the device."));
    }
}

void StorageService::updateTicker() {
    if (lists_.empty() && downloads_.empty()) ticker_->stop();
    else if (!ticker_->isActive()) ticker_->start();
}

}  // namespace ucd
