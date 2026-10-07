#include "SyncService.hpp"

#include <algorithm>
#include <cstring>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QTimer>

#include "core/FsUtil.hpp"
#include "core/Hello.hpp"
#include "core/OffThread.hpp"
#include "core/sync/SyncScanner.hpp"

using namespace std::chrono_literals;

namespace ucd {
namespace {

using namespace sync;

constexpr int    kWorkEvery     = 200;              // ms
constexpr qint64 kFrameSize     = 256 * 1024;       // largest data frame
constexpr qint64 kIndexBatch    = 256 * 1024;       // about this much JSON per index message
constexpr int    kMaxInflight   = 16;               // block requests per peer
constexpr int    kMaxPulls      = 32;               // files being fetched per folder
constexpr qint64 kPumpBudget    = 8 * 1024 * 1024;  // bytes sent per event-loop turn
constexpr auto   kRequestTimeout = 30s;
constexpr auto   kSaveEvery      = 2s;

std::optional<quint32> idOf(const QJsonObject& o) {
    const qint64 v = o.value("id").toInteger(-1);
    if (v < 0 || v > 0xFFFFFFFFll) return std::nullopt;
    return quint32(v);
}

QByteArray sha256(const QByteArray& data) {
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
}

std::vector<uint8_t> dataFrame(quint32 id, const char* bytes, qsizetype size) {
    std::vector<uint8_t> out(static_cast<size_t>(size) + 5);
    out[0] = static_cast<uint8_t>(MessageKind::SyncData);
    out[1] = uint8_t(id >> 24);
    out[2] = uint8_t(id >> 16);
    out[3] = uint8_t(id >> 8);
    out[4] = uint8_t(id);
    std::memcpy(out.data() + 5, bytes, static_cast<size_t>(size));
    return out;
}

}  // namespace

SyncService::SyncService(SendFn send, PeerKeyFn peerKey, SinceFn since, QObject* parent)
    : QObject(parent),
      send_(std::move(send)),
      peerKey_(std::move(peerKey)),
      since_(std::move(since)),
      workTimer_(new QTimer(this)),
      pumpTimer_(new QTimer(this)) {
    workTimer_->setInterval(kWorkEvery);
    connect(workTimer_, &QTimer::timeout, this, &SyncService::work);
    pumpTimer_->setSingleShot(true);
    connect(pumpTimer_, &QTimer::timeout, this, &SyncService::pump);
}

SyncService::~SyncService() {
    for (auto& [id, f] : folders_)
        if (f.dirty) f.state->save();
}

void SyncService::setIdentity(const QString& keyHex, const QString& stateDir) {
    myKey_    = keyHex;
    me_       = memberIdOf(keyHex);
    stateDir_ = stateDir;
    QDir().mkpath(stateDir_);
}

SyncService::Folder* SyncService::folder(const QString& id) {
    const auto it = folders_.find(id);
    return it == folders_.end() ? nullptr : &it->second;
}

Grant SyncService::grantFor(const QString& folderId, const QString& key) const {
    const auto it = folders_.find(folderId);
    return it == folders_.end() ? Grant{} : it->second.state->config.peers.value(key);
}

void SyncService::openFolder(const FolderConfig& config) {
    Folder f;
    f.state = std::make_unique<SyncFolder>(config, me_, QDir(stateDir_).filePath(config.id + QStringLiteral(".cbor")));
    f.state->load();
    f.state->config = config;
    f.root          = realPath(config.path);
    folders_.emplace(config.id, std::move(f));
}

void SyncService::setFolders(const QVector<FolderConfig>& configs) {
    if (me_.isEmpty()) return;
    QSet<QString> wanted;
    for (const FolderConfig& c : configs) wanted.insert(c.id);
    // Left: forget the folder; its files stay where they are.
    for (auto it = folders_.begin(); it != folders_.end();) {
        const bool moved = std::any_of(configs.begin(), configs.end(), [&](const FolderConfig& c) {
            return c.id == it->first && c.path != it->second.state->config.path;
        });
        if (wanted.contains(it->first) && !moved) {
            ++it;
            continue;
        }
        const QString id = it->first;
        for (auto& [path, pull] : it->second.pulls) pull.file.reset();
        std::erase_if(requests_, [&](const auto& kv) {
            if (kv.second.folder != id) return false;
            if (auto p = peers_.find(kv.second.dev); p != peers_.end()) --p->second.inflight;
            return true;
        });
        it->second.state->removeState();
        it = folders_.erase(it);
        for (auto& [dev, peer] : peers_) {
            peer.sent.remove(id);
            peer.helloSent.remove(id);
            peer.listening.remove(id);
        }
    }
    for (const FolderConfig& c : configs) {
        if (Folder* f = folder(c.id)) {
            f->state->config = c;
            f->replan        = true;
        } else {
            openFolder(c);
        }
    }
    for (auto& [dev, peer] : peers_) {
        sendOffers(dev);
        sendHellos(dev);
    }
    if (!folders_.empty() && !workTimer_->isActive()) workTimer_->start();
    for (auto& [id, f] : folders_) emitStatus(id);
}

// --- peers ----------------------------------------------------------------------

void SyncService::peerUp(const uconnect::DevId& dev) {
    const QString key = peerKey_(dev);
    if (key.isEmpty()) {
        if (peers_.contains(dev)) peerGone(dev);
        return;
    }
    auto it = peers_.find(dev);
    if (it != peers_.end() && it->second.key != key) {
        peerGone(dev);
        it = peers_.end();
    }
    if (it == peers_.end()) {
        peers_[dev].key = key;
        devOfKey_.insert(key, dev);
    }
    sendOffers(dev);
    sendHellos(dev);
    for (auto& [id, f] : folders_) f.replan = true;
}

void SyncService::peerGone(const uconnect::DevId& dev) {
    const auto it = peers_.find(dev);
    if (it == peers_.end()) return;
    const QString key = it->second.key;
    std::vector<quint32> failed;
    for (const auto& [id, r] : requests_)
        if (r.dev == dev) failed.push_back(id);
    for (quint32 id : failed) failRequest(id, false);
    peers_.erase(it);
    if (devOfKey_.value(key) == dev) devOfKey_.remove(key);
    emit offersChanged(key, {});
    for (auto& [id, f] : folders_) f.replan = true;
}

void SyncService::reset() {
    for (const auto& [dev, peer] : peers_) emit offersChanged(peer.key, {});
    peers_.clear();
    devOfKey_.clear();
    requests_.clear();
    for (auto& [id, f] : folders_) {
        for (auto& [path, pull] : f.pulls) pull.file.reset();  // the part file stays, to resume
        f.pulls.clear();
        if (f.dirty) f.state->save();
        f.dirty = false;
    }
}

qint64 SyncService::sinceOf(const QString& key) const {
    const auto it = devOfKey_.constFind(key);
    return it == devOfKey_.cend() ? -1 : since_(*it);
}

void SyncService::sendOffers(const uconnect::DevId& dev) {
    const auto it = peers_.find(dev);
    if (it == peers_.end()) return;
    QJsonArray offers;
    for (const auto& [id, f] : folders_)
        if (f.state->config.peers.value(it->second.key).receive)
            offers.append(QJsonObject{{"id", id}, {"label", f.state->config.label}});
    enqueue(dev, encodeControl({{"t", "sync.offers"}, {"folders", offers}}));
}

void SyncService::sendHellos(const uconnect::DevId& dev) {
    auto it = peers_.find(dev);
    if (it == peers_.end()) return;
    Peer& peer = it->second;
    for (const auto& [id, f] : folders_) {
        const Grant g = f.state->config.peers.value(peer.key);
        if ((!g.receive && !g.contribute) || peer.helloSent.contains(id)) continue;
        peer.helloSent.insert(id);
        const auto r = f.state->remotes.constFind(peer.key);
        enqueue(dev, encodeControl({{"t", "sync.hello"},
                                    {"folder", id},
                                    {"epoch", QString::number(f.state->epoch)},
                                    {"peerEpoch", r == f.state->remotes.cend() ? QString() : QString::number(r->epoch)},
                                    {"seen", r == f.state->remotes.cend() ? 0 : qint64(r->seen)}}));
    }
}

void SyncService::onHello(const uconnect::DevId& dev, const QJsonObject& msg) {
    const auto p = peers_.find(dev);
    if (p == peers_.end()) return;
    const QString id = msg.value("folder").toString();
    Folder* f = folder(id);
    if (!f) return;
    const Grant g = f->state->config.peers.value(p->second.key);
    if (g.receive) {
        // It has our index up to `seen`, unless our index started over since.
        const bool same = msg.value("peerEpoch").toString() == QString::number(f->state->epoch);
        p->second.sent[id] = same ? quint64(std::max<qint64>(0, msg.value("seen").toInteger())) : 0;
        p->second.listening.insert(id);
        sendIndex(dev, id, !same);
    }
    if (g.receive || g.contribute) sendHellos(dev);
}

void SyncService::sendIndex(const uconnect::DevId& dev, const QString& folderId, bool full) {
    auto p = peers_.find(dev);
    Folder* f = folder(folderId);
    if (p == peers_.end() || !f) return;
    quint64& sent = p->second.sent[folderId];
    if (!full && f->state->seq <= sent) return;
    const QVector<FileEntry> changed = f->state->changedSince(full ? 0 : sent);
    const QString epoch = QString::number(f->state->epoch);
    QJsonArray batch;
    qint64     size  = 0;
    bool       first = true;
    auto flush = [&] {
        enqueue(dev, encodeControl({{"t", "sync.index"}, {"folder", folderId}, {"epoch", epoch},
                                    {"full", full && first}, {"entries", batch}}));
        first = false;
        batch = {};
        size  = 0;
    };
    for (const FileEntry& e : changed) {
        batch.append(e.toJson());
        size += 200 + qint64(e.blocks.size()) * 66 + e.path.size();
        if (size >= kIndexBatch) flush();
    }
    if (!batch.isEmpty() || (full && first)) flush();
    sent = f->state->seq;
}

void SyncService::onIndex(const uconnect::DevId& dev, const QJsonObject& msg) {
    const auto p = peers_.find(dev);
    if (p == peers_.end()) return;
    Folder* f = folder(msg.value("folder").toString());
    if (!f || !f->state->config.peers.value(p->second.key).contribute) return;
    QVector<FileEntry> entries;
    for (const auto& v : msg.value("entries").toArray())
        if (auto e = FileEntry::fromJson(v.toObject())) entries << *e;
    f->state->applyRemote(p->second.key, msg.value("epoch").toString().toULongLong(), msg.value("full").toBool(),
                          entries);
    f->dirty  = true;
    f->replan = true;
}

// --- sending --------------------------------------------------------------------

void SyncService::enqueue(const uconnect::DevId& dev, std::vector<uint8_t> message) {
    const auto it = peers_.find(dev);
    if (it == peers_.end()) return;
    it->second.queue.push_back(std::move(message));
    if (!pumpTimer_->isActive()) pumpTimer_->start(0);
}

void SyncService::pump() {
    qint64 budget  = kPumpBudget;
    bool   pending = false;
    for (auto& [dev, peer] : peers_) {
        while (!peer.queue.empty() && budget > 0) {
            if (!send_(dev, peer.queue.front())) break;  // its queue is full; later
            budget -= qint64(peer.queue.front().size());
            peer.queue.pop_front();
        }
        pending = pending || !peer.queue.empty();
    }
    if (pending) pumpTimer_->start(budget > 0 ? 10 : 0);
}

// --- the work loop --------------------------------------------------------------

void SyncService::work() {
    const auto now = Clock::now();
    std::vector<quint32> late;
    for (const auto& [id, r] : requests_)
        if (now > r.deadline) late.push_back(id);
    for (quint32 id : late) failRequest(id, true);

    for (auto& [id, f] : folders_) {
        if (f.state->config.paused) continue;
        // Planning need not wait for a scan: a scan's result leaves alone
        // whatever changed after it began.
        if (f.replan) plan(id);
        if (!f.scanning && (f.rescanSoon || now >= f.nextScan)) startScan(id);
        requestBlocks(id);
    }
    for (auto& [dev, peer] : peers_)
        for (const QString& id : peer.listening) sendIndex(dev, id, false);
    if (now - lastSave_ >= kSaveEvery) {
        lastSave_ = now;
        for (auto& [id, f] : folders_)
            if (f.dirty) {
                f.state->save();
                f.dirty = false;
            }
    }
    for (auto& [id, f] : folders_) emitStatus(id);
}

void SyncService::startScan(const QString& folderId) {
    Folder* f = folder(folderId);
    if (!f) return;
    f->root       = realPath(f->state->config.path);
    f->rescanSoon = false;
    f->nextScan   = Clock::now() + std::chrono::milliseconds(scanInterval_);
    if (f->root.isEmpty()) return;
    ScanInput in;
    in.root          = f->root;
    in.known         = f->state->local;
    in.stableAfterMs = stableAfter_;
    f->scanning      = true;
    const quint64 seqAtStart = f->state->seq;
    SyncFolder* expected = f->state.get();
    runOffThread<ScanResult>(
        this, [in] { return scanFolder(in); },
        [this, folderId, seqAtStart, expected](std::optional<ScanResult> result) {
            Folder* f = folder(folderId);
            if (!f || f->state.get() != expected) return;  // left meanwhile
            f->scanning = false;
            if (!result || !result->ok) return;
            if (f->state->applyScan(*result, seqAtStart) > 0) f->dirty = true;
            f->blocked.clear();  // what was in the way may be gone
            f->replan = true;
        });
}

void SyncService::plan(const QString& folderId) {
    Folder* f = folder(folderId);
    if (!f || f->root.isEmpty()) return;
    f->replan = false;
    // The rules as they are on disk now; a scan that began before they
    // changed would bring back the old ones.
    auto readRules = [&](const char* name) {
        QFile file(QDir(f->root).filePath(QLatin1String(name)));
        return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.read(1024 * 1024)) : QString();
    };
    if (const QString text = readRules(kIgnoreFile); text != f->state->ignore.text()) f->state->ignore = IgnoreRules(text);
    if (const QString text = readRules(kSkipFile); text != f->state->skip.text()) f->state->skip = IgnoreRules(text);
    const auto since = [this](const QString& key) { return sinceOf(key); };

    struct Planned {
        QString  path;
        Decision d;
    };
    QVector<Planned> deletes, dirs, files, adopts;
    QVariantList conflicts;
    for (const QString& path : f->state->allPaths()) {
        const Decision d = f->state->decide(path, since);
        if (d.kind == Decision::Conflict) {
            QVariantList candidates;
            for (const Candidate& c : d.candidates)
                candidates << QVariantMap{{"key", c.key}, {"mine", c.key.isEmpty()}, {"size", c.entry.size},
                                          {"mtime", c.entry.mtime}, {"deleted", c.entry.deleted}, {"dir", c.entry.dir}};
            conflicts << QVariantMap{{"path", path}, {"involved", d.involved}, {"following", d.following},
                                     {"waiting", d.waiting}, {"candidates", candidates}};
            // Tell the user once per set of competing versions.
            QString signature = path;
            for (const Candidate& c : d.candidates)
                for (auto v = c.entry.version.begin(); v != c.entry.version.end(); ++v)
                    signature += QStringLiteral("|%1:%2").arg(v.key()).arg(v.value());
            if (!f->logged.contains(signature)) {
                f->logged.insert(signature);
                emit event(folderId, path, d.involved ? QStringLiteral("conflict")
                                           : d.apply  ? QStringLiteral("following:") + d.following
                                           : d.waiting ? QStringLiteral("waiting")
                                                       : QStringLiteral("kept"));
            }
            if (!d.apply) continue;
        }
        if (d.kind == Decision::None) continue;
        if (f->blocked.contains(path)) continue;
        if (d.kind == Decision::Adopt) adopts << Planned{path, d};
        else if (d.target.deleted) deletes << Planned{path, d};
        else if (d.target.dir) dirs << Planned{path, d};
        else files << Planned{path, d};
    }
    f->conflicts = conflicts;

    // Fetches no longer wanted, or wanted with other contents.
    QSet<QString> wantedFiles;
    for (const Planned& p : files) wantedFiles.insert(p.path);
    for (auto it = f->pulls.begin(); it != f->pulls.end();) {
        const auto want = std::find_if(files.begin(), files.end(), [&](const Planned& p) { return p.path == it->first; });
        if (want == files.end() || !want->d.target.sameContent(it->second.target)) {
            const QString path = it->first;
            ++it;
            dropPull(*f, path);
        } else {
            it->second.target  = want->d.target;  // perhaps a newer version of the same content
            it->second.sources = want->d.sources;
            ++it;
        }
    }

    for (const Planned& p : adopts) {
        f->state->setLocal(p.d.target);
        f->state->forced.remove(p.path);
    }
    // Folders top-down.
    for (const Planned& p : dirs) apply(*f, folderId, p.d);

    // Fetches start before any delete: starting one copies the blocks it can
    // from files here, which may be the very files about to go (a rename).
    f->needFiles = 0;
    f->needBytes = 0;
    bool allStarted = true;
    bool newRules   = false;
    for (const Planned& p : files) {
        ++f->needFiles;
        f->needBytes += p.d.target.size;
        if (!f->pulls.contains(p.path) && !f->blocked.contains(p.path)) {
            if (qsizetype(f->pulls.size()) < kMaxPulls) startPull(*f, folderId, p.d.target, p.d.sources);
            else allStarted = false;
        }
        if (p.path == QLatin1String(kIgnoreFile) && f->pulls.contains(p.path)) newRules = true;
    }
    // Deletes bottom-up, once every fetch has taken what it needs from here,
    // and not while new ignore rules are on the way: what they ignore is not
    // to be deleted here.
    if (allStarted && !newRules) {
        std::sort(deletes.begin(), deletes.end(), [](const Planned& a, const Planned& b) { return a.path > b.path; });
        for (const Planned& p : deletes) apply(*f, folderId, p.d);
    } else if (!deletes.isEmpty()) {
        f->replan = true;
    }
    if (!adopts.isEmpty() || !dirs.isEmpty() || !deletes.isEmpty()) {
        f->dirty  = true;
        f->replan = true;  // settles in another round
    }
}

// --- applying decisions -----------------------------------------------------------

QString SyncService::absolute(const Folder& f, const QString& path) const {
    return QDir(f.root).filePath(path);
}

// The disk still holds what the index says, so changing it loses nothing
// the scanner has not seen.
bool SyncService::diskMatches(const Folder& f, const QString& path) const {
    const QFileInfo fi(absolute(f, path));
    if (fi.isSymLink() || fi.isJunction()) return false;
    const auto l = f.state->local.constFind(path);
    if (l == f.state->local.cend() || l->deleted) return !fi.exists();
    if (l->skipped) return false;
    if (l->dir) return fi.isDir();
    return fi.isFile() && fi.size() == l->size && fi.lastModified().toMSecsSinceEpoch() == l->mtime;
}

// The folder `path` goes in is inside the synced folder, links resolved.
bool SyncService::safeParent(const Folder& f, const QString& path, bool create) const {
    const QString parent = QFileInfo(absolute(f, path)).absolutePath();
    if (create) QDir().mkpath(parent);
    const QString real = realPath(parent);
    return !real.isEmpty() && isWithin(real, f.root);
}

void SyncService::apply(Folder& f, const QString& folderId, const Decision& d) {
    const QString path = d.target.path;
    const QString abs  = absolute(f, path);
    const QFileInfo fi(abs);
    auto block = [&](const QString& reason) {
        if (f.blocked.value(path) != reason) emit event(folderId, path, QStringLiteral("blocked:") + reason);
        f.blocked.insert(path, reason);
    };
    if (!safeParent(f, path, d.target.dir)) return block(QStringLiteral("outside"));

    if (d.target.dir) {
        if (fi.isDir() && !fi.isSymLink() && !fi.isJunction()) {
            f.state->setLocal(d.target);
        } else if (fi.exists()) {
            f.rescanSoon = true;
            return block(QStringLiteral("in_the_way"));
        } else if (QDir().mkdir(abs)) {
            f.state->setLocal(d.target);
        } else {
            return block(QStringLiteral("io"));
        }
        f.state->forced.remove(path);
        return;
    }

    // A delete.
    if (!diskMatches(f, path)) {
        f.rescanSoon = true;  // changed here meanwhile: the scan will make it a conflict
        return block(QStringLiteral("changed"));
    }
    if (fi.exists()) {
        if (fi.isDir() && !QDir(abs).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System))
            return block(QStringLiteral("not_empty"));  // holds files that are not synced
        const bool gone = (useTrash_ && QFile::moveToTrash(abs)) || removeEntry(fi);
        if (!gone) return block(QStringLiteral("io"));
    }
    f.state->setLocal(d.target);
    f.state->forced.remove(path);
}

void SyncService::startPull(Folder& f, const QString& folderId, const FileEntry& target, const QStringList& sources) {
    const QString path = target.path;
    auto block = [&](const QString& reason) {
        if (f.blocked.value(path) != reason) emit event(folderId, path, QStringLiteral("blocked:") + reason);
        f.blocked.insert(path, reason);
    };
    if (!safeParent(f, path, true)) return block(QStringLiteral("outside"));
    if (!diskMatches(f, path)) {
        f.rescanSoon = true;
        return block(QStringLiteral("changed"));
    }
    const QFileInfo fi(absolute(f, path));
    Pull p;
    p.target  = target;
    p.sources = sources;
    p.temp    = QDir(fi.absolutePath()).filePath(QLatin1String(kPartPrefix) +
                                                 QString::fromLatin1(target.hash.toHex().left(8)) + QLatin1Char('-') +
                                                 fi.fileName());
    p.file = std::make_unique<QFile>(p.temp);
    if (!p.file->open(QIODevice::ReadWrite)) return block(QStringLiteral("io"));
    const qint64 had = p.file->size();
    if (!p.file->resize(target.size)) return block(QStringLiteral("io"));

    // Where each block can come from without the network: the part file of an
    // earlier attempt, or any file here that has it (a rename, a copy, an
    // earlier version).
    QHash<QByteArray, std::pair<QString, qint64>> local;
    for (const FileEntry& e : f.state->local) {
        if (e.deleted || e.dir || e.skipped) continue;
        for (int i = 0; i < e.blocks.size(); ++i) local.insert(e.blocks[i], {e.path, qint64(i) * e.blockSize});
    }
    for (int i = 0; i < target.blocks.size(); ++i) {
        const qint64 offset = qint64(i) * target.blockSize;
        const qint64 length = target.blockLength(i);
        if (offset < had && p.file->seek(offset) && sha256(p.file->read(length)) == target.blocks[i]) continue;
        bool found = false;
        if (const auto it = local.constFind(target.blocks[i]); it != local.cend()) {
            QFile from(absolute(f, it->first));
            if (from.open(QIODevice::ReadOnly) && from.seek(it->second)) {
                const QByteArray data = from.read(length);
                if (sha256(data) == target.blocks[i] && p.file->seek(offset) && p.file->write(data) == length)
                    found = true;
            }
        }
        if (!found) p.missing << i;
    }
    p.lastProgress = Clock::now();
    f.pulls[path] = std::move(p);
    if (f.pulls[path].missing.isEmpty()) finishPull(folderId, path);
}

void SyncService::requestBlocks(const QString& folderId) {
    Folder* f = folder(folderId);
    if (!f) return;
    for (auto& [path, p] : f->pulls) {
        for (int i : p.missing) {
            if (p.requested.contains(i)) continue;
            // The connected holder with the fewest requests waiting.
            Peer*            best = nullptr;
            uconnect::DevId  bestDev{};
            for (const QString& key : p.sources) {
                const auto d = devOfKey_.constFind(key);
                if (d == devOfKey_.cend()) continue;
                Peer& peer = peers_.at(*d);
                if (peer.inflight >= kMaxInflight) continue;
                if (!best || peer.inflight < best->inflight) {
                    best    = &peer;
                    bestDev = *d;
                }
            }
            if (!best) break;
            const quint32 id = nextRequest_++;
            Request r;
            r.dev      = bestDev;
            r.folder   = folderId;
            r.path     = path;
            r.block    = i;
            r.hash     = p.target.blocks[i];
            r.expected = p.target.blockLength(i);
            r.deadline = Clock::now() + kRequestTimeout;
            requests_.emplace(id, std::move(r));
            p.requested.insert(i);
            ++best->inflight;
            enqueue(bestDev, encodeControl({{"t", "sync.get"},
                                            {"id", qint64(id)},
                                            {"folder", folderId},
                                            {"path", path},
                                            {"block", i},
                                            {"offset", qint64(i) * p.target.blockSize},
                                            {"size", p.target.blockLength(i)},
                                            {"hash", QString::fromLatin1(p.target.blocks[i].toHex())}}));
        }
    }
}

void SyncService::onData(const uconnect::DevId& dev, std::span<const uint8_t> message) {
    if (message.size() < 5) return;
    const quint32 id = (quint32{message[1]} << 24) | (quint32{message[2]} << 16) | (quint32{message[3]} << 8) |
                       quint32{message[4]};
    const auto it = requests_.find(id);
    if (it == requests_.end() || it->second.dev != dev) return;
    Request& r = it->second;
    r.data.append(reinterpret_cast<const char*>(message.data() + 5), qsizetype(message.size() - 5));
    r.deadline = Clock::now() + kRequestTimeout;
    if (r.data.size() < r.expected) return;

    Folder* f    = folder(r.folder);
    Pull*   want = nullptr;
    if (f)
        if (const auto p = f->pulls.find(r.path); p != f->pulls.end()) want = &p->second;
    if (!want || r.data.size() != r.expected || sha256(r.data) != r.hash) {
        failRequest(id, true);  // damaged or unwanted; another holder may do better
        return;
    }
    Pull& pull = *want;
    const qint64 offset = qint64(r.block) * pull.target.blockSize;
    if (!pull.file->seek(offset) || pull.file->write(r.data) != r.data.size()) {
        failRequest(id, false);
        return;
    }
    pull.missing.removeOne(r.block);
    pull.requested.remove(r.block);
    pull.lastProgress = Clock::now();
    if (auto peer = peers_.find(dev); peer != peers_.end()) --peer->second.inflight;
    const QString folderId = r.folder, path = r.path;
    requests_.erase(it);
    if (pull.missing.isEmpty()) finishPull(folderId, path);
}

void SyncService::onError(const uconnect::DevId& dev, const QJsonObject& msg) {
    const auto id = idOf(msg);
    if (!id) return;
    const auto it = requests_.find(*id);
    if (it != requests_.end() && it->second.dev == dev) failRequest(*id, true);
}

void SyncService::failRequest(quint32 id, bool dropSource) {
    const auto it = requests_.find(id);
    if (it == requests_.end()) return;
    const Request r = std::move(it->second);
    requests_.erase(it);
    QString key;
    if (auto peer = peers_.find(r.dev); peer != peers_.end()) {
        --peer->second.inflight;
        key = peer->second.key;
    }
    Folder* f = folder(r.folder);
    if (!f) return;
    const auto p = f->pulls.find(r.path);
    if (p == f->pulls.end()) return;
    p->second.requested.remove(r.block);
    if (dropSource && !key.isEmpty()) p->second.sources.removeAll(key);
}

void SyncService::finishPull(const QString& folderId, const QString& path) {
    Folder* f = folder(folderId);
    if (!f) return;
    const auto it = f->pulls.find(path);
    if (it == f->pulls.end()) return;
    Pull pull = std::move(it->second);
    f->pulls.erase(it);
    auto block = [&](const QString& reason) {
        if (f->blocked.value(path) != reason) emit event(folderId, path, QStringLiteral("blocked:") + reason);
        f->blocked.insert(path, reason);
    };

    pull.file->setFileTime(QDateTime::fromMSecsSinceEpoch(pull.target.mtime), QFileDevice::FileModificationTime);
    pull.file->close();
    const QString abs = absolute(*f, path);
    // Changed here while it was being fetched: keep the part file, rescan,
    // and let the result decide.
    if (!diskMatches(*f, path) || !safeParent(*f, path, false)) {
        f->rescanSoon = true;
        return block(QStringLiteral("changed"));
    }
    if (!replaceWith(pull.temp, abs)) return block(QStringLiteral("io"));
    FileEntry done = pull.target;
    done.mtime     = QFileInfo(abs).lastModified().toMSecsSinceEpoch();  // as the disk keeps it
    f->state->setLocal(done);

    f->state->forced.remove(path);
    f->dirty  = true;
    f->replan = true;
}

void SyncService::dropPull(Folder& f, const QString& path) {
    const auto it = f.pulls.find(path);
    if (it == f.pulls.end()) return;
    std::erase_if(requests_, [&](const auto& kv) {
        if (kv.second.path != path || folder(kv.second.folder) != &f) return false;
        if (auto peer = peers_.find(kv.second.dev); peer != peers_.end()) --peer->second.inflight;
        return true;
    });
    const QString temp = it->second.temp;
    it->second.file.reset();
    QFile::remove(temp);  // contents nobody wants any more
    f.pulls.erase(it);
}

// --- serving blocks ---------------------------------------------------------------

void SyncService::onGet(const uconnect::DevId& dev, const QJsonObject& msg) {
    const auto id = idOf(msg);
    const auto p  = peers_.find(dev);
    if (!id || p == peers_.end()) return;
    auto refuse = [&](const char* code) {
        enqueue(dev, encodeControl({{"t", "sync.err"}, {"id", qint64(*id)}, {"code", code}}));
    };
    Folder* f = folder(msg.value("folder").toString());
    if (!f || !f->state->config.peers.value(p->second.key).receive) return refuse("forbidden");
    const QString path = msg.value("path").toString();
    const auto    l    = f->state->local.constFind(path);
    if (!isValidSyncPath(path) || l == f->state->local.cend() || l->deleted || l->dir || l->skipped ||
        !f->state->participates(path) || f->root.isEmpty())
        return refuse("not_found");
    const int    block  = msg.value("block").toInt(-1);
    const QByteArray hash = QByteArray::fromHex(msg.value("hash").toString().toLatin1());
    if (block < 0 || block >= l->blocks.size() || l->blocks[block] != hash ||
        msg.value("offset").toInteger(-1) != qint64(block) * l->blockSize ||
        msg.value("size").toInteger(-1) != l->blockLength(block))
        return refuse("changed");

    const QString abs = absolute(*f, path);
    const QFileInfo fi(abs);
    const QString real = realPath(abs);
    if (fi.isSymLink() || fi.isJunction() || real.isEmpty() || !isWithin(real, f->root)) return refuse("not_found");
    QFile file(abs);
    if (!file.open(QIODevice::ReadOnly) || !file.seek(qint64(block) * l->blockSize)) return refuse("io");
    const QByteArray data = file.read(l->blockLength(block));
    if (sha256(data) != hash) {
        f->rescanSoon = true;  // the file changed since it was indexed
        return refuse("changed");
    }
    for (qsizetype at = 0; at < data.size(); at += kFrameSize)
        enqueue(dev, dataFrame(*id, data.constData() + at, std::min<qsizetype>(kFrameSize, data.size() - at)));
}

// --- messages ---------------------------------------------------------------------

bool SyncService::handleMessage(const uconnect::DevId& dev, std::span<const uint8_t> message) {
    if (message.empty()) return false;
    if (message[0] == static_cast<uint8_t>(MessageKind::SyncData)) {
        onData(dev, message);
        return true;
    }
    const auto msg = decodeControl(message);
    if (!msg) return false;
    const QString t = msg->value("t").toString();
    if (!t.startsWith(QLatin1String("sync."))) return false;
    if (!peers_.contains(dev)) return true;  // not a trusted, proven peer: nothing to say to it

    if (t == QLatin1String("sync.offers")) {
        QVariantList offers;
        for (const auto& v : msg->value("folders").toArray()) {
            const QJsonObject o = v.toObject();
            const QString     id = o.value("id").toString();
            if (id.size() != 32 || QByteArray::fromHex(id.toLatin1()).size() != 16) continue;
            offers << QVariantMap{{"id", id}, {"label", o.value("label").toString().left(100)}};
        }
        emit offersChanged(peers_.at(dev).key, offers);
    } else if (t == QLatin1String("sync.hello")) {
        onHello(dev, *msg);
    } else if (t == QLatin1String("sync.index")) {
        onIndex(dev, *msg);
    } else if (t == QLatin1String("sync.get")) {
        onGet(dev, *msg);
    } else if (t == QLatin1String("sync.err")) {
        onError(dev, *msg);
    }
    return true;
}

// --- user actions -----------------------------------------------------------------

void SyncService::resolve(const QString& folderId, const QString& path, const QString& choice, const QString& key) {
    Folder* f = folder(folderId);
    if (!f) return;
    if (choice == QLatin1String("mine")) {
        f->state->keepMine(path);
    } else if (choice == QLatin1String("theirs")) {
        f->state->take(path, key);
    } else if (choice == QLatin1String("skip")) {
        QFile skip(QDir(f->root).filePath(QLatin1String(kSkipFile)));
        if (skip.open(QIODevice::Append | QIODevice::Text)) skip.write((exactPattern(path) + QLatin1Char('\n')).toUtf8());
        f->rescanSoon = true;
    }
    f->blocked.remove(path);
    f->dirty  = true;
    f->replan = true;
}

void SyncService::rescan(const QString& folderId) {
    if (Folder* f = folder(folderId)) {
        f->rescanSoon = true;
        f->blocked.clear();
    }
}

// --- status -----------------------------------------------------------------------

QVariantMap SyncService::folderStatus(const QString& folderId, const Folder& f) const {
    qint64 files = 0, bytes = 0;
    for (const FileEntry& e : f.state->local) {
        if (e.deleted || e.dir) continue;
        ++files;
        bytes += e.size;
    }
    qint64 fetched = 0;
    for (const auto& [path, p] : f.pulls)
        fetched += qint64(p.target.blocks.size() - p.missing.size()) * p.target.blockSize;
    const QString state = f.state->config.paused ? QStringLiteral("paused")
                        : f.root.isEmpty()       ? QStringLiteral("missing")
                        : f.needFiles > 0        ? QStringLiteral("syncing")
                        : f.scanning             ? QStringLiteral("scanning")
                                                 : QStringLiteral("idle");
    QVariantMap blocked;
    for (auto it = f.blocked.cbegin(); it != f.blocked.cend(); ++it) blocked.insert(it.key(), it.value());
    return {
        {"id", folderId},
        {"state", state},
        {"files", files},
        {"bytes", bytes},
        {"needFiles", f.needFiles},
        {"needBytes", std::max<qint64>(0, f.needBytes - fetched)},
        {"conflicts", f.conflicts},
        {"blocked", blocked},
    };
}

void SyncService::emitStatus(const QString& folderId) {
    Folder* f = folder(folderId);
    if (!f) return;
    const QVariantMap status = folderStatus(folderId, *f);
    if (status == f->lastStatus) return;
    f->lastStatus = status;
    emit folderChanged(folderId, status);
}

}  // namespace ucd
