#include "SyncService.hpp"

#include <algorithm>
#include <cstring>
#include <functional>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStorageInfo>
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
constexpr auto   kCleanEvery     = 1h;
constexpr auto   kRateWindow     = 5s;    // rates are averaged over this long
constexpr auto   kEmitEvery      = 400ms; // progress reaches the UI this often
constexpr auto   kFreeEvery      = 10s;   // how often the disk's free space is read
constexpr int    kMaxListed      = 5000;  // rule-preview matches reported
// A kept file's name: <base>~yyyyMMdd-HHmmss[ (n)][.ext]
const QRegularExpression kKeptName(QStringLiteral("^(.*)~(\\d{8}-\\d{6})(?: \\(\\d+\\))?(\\.[^.]*)?$"));

QVariantMap versionMap(const VersionVector& v) {
    QVariantMap out;
    for (auto it = v.cbegin(); it != v.cend(); ++it) out.insert(it.key(), qint64(it.value()));
    return out;
}

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

void SyncService::Meter::add(qint64 bytes) {
    const auto now = Clock::now();
    samples.emplace_back(now, bytes);
    while (!samples.empty() && now - samples.front().first > kRateWindow) samples.pop_front();
}

qint64 SyncService::Meter::rate() const {
    const auto now = Clock::now();
    qint64 sum = 0;
    for (const auto& [when, bytes] : samples)
        if (now - when <= kRateWindow) sum += bytes;
    return sum / std::chrono::duration_cast<std::chrono::seconds>(kRateWindow).count();
}

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
        QHash<QString, Grant> before;
        if (Folder* f = folder(c.id)) {
            before           = f->state->config.peers;
            f->state->config = c;
            f->replan        = true;
        } else {
            openFolder(c);
        }
        // A peer that is no longer sent the folder stops getting its index
        // (the offers tell it to forget what it had); one whose grants
        // changed is greeted again, so both sides learn where they stand.
        for (auto& [dev, peer] : peers_) {
            const Grant g = c.peers.value(peer.key);
            if (!g.sends()) peer.listening.remove(c.id);
            if (g != before.value(peer.key)) peer.helloSent.remove(c.id);
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
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto& [id, f] : folders_) {
        f.replan = true;
        if (f.state->config.peers.contains(key)) {
            f.state->peerStats[key].lastSeen = now;
            f.dirty = true;
        }
    }
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
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto& [id, f] : folders_) {
        f.replan = true;
        if (f.state->config.peers.contains(key)) {
            f.state->peerStats[key].lastSeen = now;
            f.dirty = true;
        }
    }
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
        if (f.state->config.peers.value(it->second.key).sends())
            offers.append(QJsonObject{{"id", id}, {"label", f.state->config.label}});
    enqueue(dev, encodeControl({{"t", "sync.offers"}, {"folders", offers}}));
}

void SyncService::sendHellos(const uconnect::DevId& dev, bool reply) {
    auto it = peers_.find(dev);
    if (it == peers_.end()) return;
    Peer& peer = it->second;
    for (const auto& [id, f] : folders_) {
        const Grant g = f.state->config.peers.value(peer.key);
        if ((!g.sends() && !g.accepts()) || peer.helloSent.contains(id)) continue;
        peer.helloSent.insert(id);
        const auto r = f.state->remotes.constFind(peer.key);
        enqueue(dev, encodeControl({{"t", "sync.hello"},
                                    {"folder", id},
                                    {"epoch", QString::number(f.state->epoch)},
                                    {"peerEpoch", r == f.state->remotes.cend() ? QString() : QString::number(r->epoch)},
                                    {"seen", r == f.state->remotes.cend() ? 0 : qint64(r->seen)},
                                    {"reply", reply}}));
    }
}

void SyncService::onHello(const uconnect::DevId& dev, const QJsonObject& msg) {
    const auto p = peers_.find(dev);
    if (p == peers_.end()) return;
    const QString id = msg.value("folder").toString();
    Folder* f = folder(id);
    if (!f) return;
    const Grant g = f->state->config.peers.value(p->second.key);
    if (g.sends()) {
        // It has our index up to `seen`, unless our index started over since.
        const bool same = msg.value("peerEpoch").toString() == QString::number(f->state->epoch);
        p->second.sent[id] = same ? quint64(std::max<qint64>(0, msg.value("seen").toInteger())) : 0;
        p->second.listening.insert(id);
        sendIndex(dev, id, !same);
    }
    // Our hello may have arrived before it had this folder: until its index
    // comes, answer with ours so it knows to send it. Replies are not
    // answered, so two devices cannot keep this going.
    const bool reply = !msg.value("reply").toBool() && g.accepts() && !p->second.heard.contains(id);
    if (reply) p->second.helloSent.remove(id);
    if (g.sends() || g.accepts()) sendHellos(dev, reply);
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
    auto& stats = f->state->peerStats[p->second.key];
    if (stats.sentSeq != sent) {
        stats.sentSeq = sent;
        f->dirty  = true;
        f->replan = true;  // what the peer still lacks changed
    }
}

void SyncService::onIndex(const uconnect::DevId& dev, const QJsonObject& msg) {
    const auto p = peers_.find(dev);
    if (p == peers_.end()) return;
    Folder* f = folder(msg.value("folder").toString());
    if (!f || !f->state->config.peers.value(p->second.key).accepts()) return;
    p->second.heard.insert(msg.value("folder").toString());
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
        if (now >= f.nextCleanup) cleanKept(id);
        if (!f.scanning && (f.rescanSoon || now >= f.nextScan)) startScan(id);
        requestBlocks(id);
        if (now >= f.nextFree) {
            f.nextFree = now + kFreeEvery;
            f.free     = f.root.isEmpty() ? -1 : QStorageInfo(f.root).bytesAvailable();
        }
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
            f->leftOut = result->leftOut;
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

    QHash<QString, Decision> decisions;
    for (const QString& path : f->state->allPaths()) {
        Decision d = f->state->decide(path, since);
        if (d.kind != Decision::None) decisions.insert(path, std::move(d));
    }

    // Names that clash, ignoring case, with an older one are held back here:
    // not fetched, and if they are files of ours, marked skipped. When the
    // older one goes, they come back at the next scan.
    const QHash<QString, QString> clashes = f->state->clashes(decisions);
    for (auto it = f->state->clashHeld.cbegin(); it != f->state->clashHeld.cend(); ++it)
        if (!clashes.contains(it.key())) f->rescanSoon = true;  // released
    f->state->clashHeld = clashes;
    for (auto it = clashes.cbegin(); it != clashes.cend(); ++it) {
        decisions.remove(it.key());
        const auto l = f->state->local.constFind(it.key());
        if (l != f->state->local.cend() && !l->deleted && !l->skipped) {
            FileEntry e = *l;
            e.skipped   = true;
            f->state->setLocal(e);
            f->dirty  = true;
        }
    }
    // Tell the user once about each thing held back, and why.
    QVariantList held;
    QSet<QString> heldPaths(f->state->scanHeld.keyBegin(), f->state->scanHeld.keyEnd());
    for (auto it = clashes.keyBegin(); it != clashes.keyEnd(); ++it) heldPaths.insert(*it);
    QStringList heldSorted(heldPaths.begin(), heldPaths.end());
    std::sort(heldSorted.begin(), heldSorted.end());
    for (const QString& path : heldSorted) {
        const QString reason = f->state->heldReason(path);
        held << QVariantMap{{"path", path}, {"reason", reason}};
        if (!f->logged.contains(path + QLatin1Char('|') + reason)) {
            f->logged.insert(path + QLatin1Char('|') + reason);
            emit event(folderId, path, QStringLiteral("held:") + reason);
        }
    }
    f->held = held;

    struct Planned {
        QString  path;
        Decision d;
    };
    QVector<Planned> deletes, dirs, files, adopts;
    QVariantList conflicts;
    QStringList  paths = decisions.keys();
    std::sort(paths.begin(), paths.end());
    for (const QString& path : paths) {
        const Decision& d = decisions[path];
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
        if (f->blocked.contains(path)) continue;
        if (d.kind == Decision::Adopt) adopts << Planned{path, d};
        else if (d.target.deleted) deletes << Planned{path, d};
        else if (d.target.dir) dirs << Planned{path, d};
        else files << Planned{path, d};
    }
    f->conflicts = conflicts;

    for (const Planned& p : adopts) {
        f->state->setLocal(p.d.target);
        f->state->forced.remove(p.path);
    }
    // Folders top-down.
    for (const Planned& p : dirs) apply(*f, folderId, p.d);

    // Renames: a new name whose content is that of a file about to be
    // deleted here is the same file, moved. Rename it in place rather than
    // assembling a copy and setting the original aside. A change of case
    // only is one too.
    for (auto file = files.begin(); file != files.end();) {
        const auto had = f->state->local.constFind(file->path);
        const bool fresh = had == f->state->local.cend() || had->deleted;
        auto from = deletes.end();
        if (fresh && !f->pulls.contains(file->path)) {
            for (auto del = deletes.begin(); del != deletes.end(); ++del) {
                const auto l = f->state->local.constFind(del->path);
                if (l == f->state->local.cend() || l->deleted || l->dir || !l->sameContent(file->d.target)) continue;
                // Prefer the same name in another case: that is surely the one.
                if (from == deletes.end() || foldName(del->path) == foldName(file->path)) from = del;
            }
        }
        if (from != deletes.end() && renameLocal(*f, from->path, file->d.target)) {
            f->state->setLocal(from->d.target);
            f->state->forced.remove(from->path);
            f->state->forced.remove(file->path);
            deletes.erase(from);
            file = files.erase(file);
            f->dirty  = true;
            f->replan = true;
        } else {
            ++file;
        }
    }

    // Fetches no longer wanted, or wanted with other contents.
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

    // Fetches start before any delete: starting one copies the blocks it can
    // from files here, which may be the very files about to go.
    f->needFiles = 0;
    f->needBytes = 0;
    f->queued.clear();
    bool allStarted = true;
    bool newRules   = false;
    for (const Planned& p : files) {
        ++f->needFiles;
        f->needBytes += p.d.target.size;
        if (!f->pulls.contains(p.path) && !f->blocked.contains(p.path)) {
            if (qsizetype(f->pulls.size()) < kMaxPulls) startPull(*f, folderId, p.d.target, p.d.sources);
            else allStarted = false;
        }
        if (!f->pulls.contains(p.path)) f->queued << p.path;
        if (p.path == QLatin1String(kIgnoreFile) && f->pulls.contains(p.path)) newRules = true;
    }
    // Deletes bottom-up, once every fetch has taken what it needs from here,
    // and not while new ignore rules are on the way: what they ignore is not
    // to be deleted here. A round that would remove much of the folder at
    // once (a device wiped its copy, say) waits for the user instead.
    QStringList pending;
    int applied = 0;
    if (allStarted && !newRules) {
        std::sort(deletes.begin(), deletes.end(), [](const Planned& a, const Planned& b) { return a.path > b.path; });
        qint64 live = 0;
        for (const FileEntry& e : f->state->local)
            if (!e.deleted) ++live;
        const int  n       = int(deletes.size());
        const bool guarded = guardMin_ > 0 && n >= guardMin_ &&
                             (n >= guardMany_ || double(n) >= guardShare_ * double(std::max<qint64>(live, 1)));
        for (const Planned& p : deletes) {
            if (guarded && !f->approvedDeletes.contains(p.path)) {
                pending << p.path;
                continue;
            }
            apply(*f, folderId, p.d);
            f->approvedDeletes.remove(p.path);
            ++applied;
        }
        if (pending.isEmpty()) f->approvedDeletes.clear();
    } else if (!deletes.isEmpty()) {
        f->replan = true;
    }
    std::sort(pending.begin(), pending.end());
    if (!pending.isEmpty()) {
        const QString signature = QStringLiteral("deletes|") + pending.join(QLatin1Char('|'));
        if (!f->logged.contains(signature)) {
            f->logged.insert(signature);
            emit event(folderId, {}, QStringLiteral("deletes:%1").arg(pending.size()));
        }
    }
    f->pendingDeletes = pending;
    if (!adopts.isEmpty() || !dirs.isEmpty() || applied > 0) {
        f->dirty  = true;
        f->replan = true;  // settles in another round
    }

    // What each member still lacks from us, or we from it; and when there
    // was nothing either way while it was connected.
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    for (auto g = f->state->config.peers.cbegin(); g != f->state->config.peers.cend(); ++g) {
        const QString& key = g.key();
        Folder::Need need;
        if (g->sends())
            for (const FileEntry& e : f->state->local) {
                if (e.skipped || !pendingFor(*f, e, key)) continue;
                if (e.deleted || e.dir) {
                    ++need.sendChanges;
                } else {
                    ++need.sendFiles;
                    need.sendBytes += e.size;
                }
            }
        if (g->accepts()) {
            for (const Planned& p : files)
                if (p.d.sources.contains(key)) {
                    ++need.receiveFiles;
                    need.receiveBytes += p.d.target.size;
                }
            for (const Planned& p : deletes)
                if (p.d.sources.contains(key) || p.d.following == key) ++need.receiveChanges;
            for (const Planned& p : dirs)
                if (p.d.following == key) ++need.receiveChanges;
        }
        const auto r = f->state->remotes.constFind(key);
        if (r != f->state->remotes.cend())
            for (const FileEntry& e : r->entries)
                if (e.skipped) ++need.skipped;
        f->needs.insert(key, need);
        if (need.sendFiles + need.sendChanges + need.receiveFiles + need.receiveChanges == 0 && devOfKey_.contains(key) &&
            !f->scanning) {
            auto& stats = f->state->peerStats[key];
            if (nowMs / 60000 != stats.lastInSync / 60000) f->dirty = true;  // saved once a minute
            stats.lastInSync = nowMs;
        }
    }
    for (auto it = f->needs.begin(); it != f->needs.end();)
        it = f->state->config.peers.contains(it.key()) ? std::next(it) : f->needs.erase(it);
}

bool SyncService::pendingFor(const Folder& f, const FileEntry& e, const QString& key) const {
    // The peer's own index says best what it has. Only a peer that never
    // sends one is judged by how much of our index it was sent.
    const auto r = f.state->remotes.constFind(key);
    if (r != f.state->remotes.cend()) {
        const auto theirs = r->entries.constFind(e.path);
        if (theirs == r->entries.cend()) return !e.deleted;
        return !theirs->skipped && compare(theirs->version, e.version) == Order::Older;
    }
    quint64 sentSeq = 0;
    if (const auto st = f.state->peerStats.constFind(key); st != f.state->peerStats.cend()) sentSeq = st->sentSeq;
    if (const Peer* p = peerOf(key)) sentSeq = std::max(sentSeq, p->sent.value(f.state->config.id));
    return e.seq > sentSeq;
}

const SyncService::Peer* SyncService::peerOf(const QString& key) const {
    const auto d = devOfKey_.constFind(key);
    if (d == devOfKey_.cend()) return nullptr;
    const auto p = peers_.find(*d);
    return p == peers_.end() ? nullptr : &p->second;
}

// Moves the file at `from` to `to.path`, if the disk still holds what the
// index says and nothing else is at the new name (or it is the same file
// under another case).
bool SyncService::renameLocal(Folder& f, const QString& from, const FileEntry& to) {
    const bool caseOnly = foldName(from) == foldName(to.path);
    const QString parent = to.path.contains(QLatin1Char('/')) ? to.path.section(QLatin1Char('/'), 0, -2) : QString();
    if (!caseMismatch(f, caseOnly ? parent : to.path).isEmpty()) return false;
    if (!diskMatches(f, from) || !safeParent(f, to.path, true)) return false;
    const QString absFrom = absolute(f, from), absTo = absolute(f, to.path);
    if (QFileInfo::exists(absTo) && !caseOnly) return false;
    if (caseOnly) {
        // A case-insensitive disk sees the new name as taken by this very
        // file; going through another name works everywhere.
        const QString step = QDir(QFileInfo(absFrom).absolutePath())
                                 .filePath(QLatin1String(kPartPrefix) + QStringLiteral("rename-") + QFileInfo(absFrom).fileName());
        if (!QFile::rename(absFrom, step)) return false;
        if (!QFile::rename(step, absTo)) {
            QFile::rename(step, absFrom);
            return false;
        }
    } else if (!QFile::rename(absFrom, absTo)) {
        return false;
    }
    QFile moved(absTo);
    if (moved.open(QIODevice::ReadWrite)) {
        moved.setFileTime(QDateTime::fromMSecsSinceEpoch(to.mtime), QFileDevice::FileModificationTime);
        moved.close();
    }
    FileEntry done = to;
    done.mtime     = QFileInfo(absTo).lastModified().toMSecsSinceEpoch();
    f.state->setLocal(done);
    return true;
}

// --- applying decisions -----------------------------------------------------------

QString SyncService::absolute(const Folder& f, const QString& path) const {
    return QDir(f.root).filePath(path);
}

// The disk still holds what the index says, so changing it loses nothing
// the scanner has not seen.
QString SyncService::caseMismatch(const Folder& f, const QString& path) const {
    QString dir = f.root;
    QString rel;
    for (const QString& part : path.split(QLatin1Char('/'))) {
        const QString next = QDir(dir).filePath(part);
        const QFileInfo fi(next);
        if (!fi.exists() && !fi.isSymLink()) return {};  // the rest is not there yet
        rel = rel.isEmpty() ? part : rel + QLatin1Char('/') + part;
        QStringList names = QDir(dir).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
#ifdef Q_OS_MACOS
        for (QString& n : names) n = n.normalized(QString::NormalizationForm_C);
#endif
        if (!names.contains(part)) return rel;  // found only by ignoring case
        dir = next;
    }
    return {};
}

bool SyncService::diskMatches(const Folder& f, const QString& path) const {
    const QFileInfo fi(absolute(f, path));
    if (fi.isSymLink() || fi.isJunction()) return false;
    const auto l = f.state->local.constFind(path);
    // Not ours: nothing there, or only something under another case, which
    // the callers keep clear of.
    if (l == f.state->local.cend() || l->deleted) return !fi.exists() || !caseMismatch(f, path).isEmpty();
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
        if (!f.logged.contains(path + QStringLiteral("|blocked:") + reason)) {  // told once, though retried
            f.logged.insert(path + QStringLiteral("|blocked:") + reason);
            emit event(folderId, path, QStringLiteral("blocked:") + reason);
        }
        f.blocked.insert(path, reason);
    };
    const bool otherCase = !caseMismatch(f, path).isEmpty();
    if (otherCase && !d.target.deleted) return block(QStringLiteral("clash"));
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
    if (fi.exists() && !otherCase) {  // under another case it is a different file of ours
        if (fi.isDir() && !QDir(abs).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System))
            return block(QStringLiteral("not_empty"));  // holds files that are not synced
        // Files are kept aside for a while; an empty folder just goes.
        const bool gone = fi.isDir() ? QDir().rmdir(abs) : (keepAside(f, path) || removeEntry(fi));
        if (!gone) return block(QStringLiteral("io"));
    }
    f.state->setLocal(d.target);
    f.state->forced.remove(path);
}

void SyncService::startPull(Folder& f, const QString& folderId, const FileEntry& target, const QStringList& sources) {
    const QString path = target.path;
    auto block = [&](const QString& reason) {
        if (!f.logged.contains(path + QStringLiteral("|blocked:") + reason)) {  // told once, though retried
            f.logged.insert(path + QStringLiteral("|blocked:") + reason);
            emit event(folderId, path, QStringLiteral("blocked:") + reason);
        }
        f.blocked.insert(path, reason);
    };
    // A file or folder of ours under another case holds this name on a disk
    // that does not tell case apart: it is held back, and this cannot be
    // written over or into it. Nothing to retry until one of them goes.
    if (!caseMismatch(f, path).isEmpty()) return block(QStringLiteral("clash"));
    if (!safeParent(f, path, true)) return block(QStringLiteral("outside"));
    if (!diskMatches(f, path)) {
        f.rescanSoon = true;
        return block(QStringLiteral("changed"));
    }
    const QFileInfo fi(absolute(f, path));
    Pull p;
    p.target  = target;
    p.sources = sources;
    p.keepOld = f.state->forced.contains(path);  // this device's own version, given up in a conflict
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
    if (auto peer = peers_.find(dev); peer != peers_.end()) {
        --peer->second.inflight;
        peer->second.received.add(r.data.size());
    }
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
        if (!f->logged.contains(path + QStringLiteral("|blocked:") + reason)) {
            f->logged.insert(path + QStringLiteral("|blocked:") + reason);
            emit event(folderId, path, QStringLiteral("blocked:") + reason);
        }
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
    if (!caseMismatch(*f, path).isEmpty()) return block(QStringLiteral("clash"));
    if (pull.keepOld && QFileInfo(abs).isFile() && !keepAside(*f, path)) return block(QStringLiteral("io"));
    if (!replaceWith(pull.temp, abs)) return block(QStringLiteral("io"));
    FileEntry done = pull.target;
    done.mtime     = QFileInfo(abs).lastModified().toMSecsSinceEpoch();  // as the disk keeps it
    f->state->setLocal(done);

    f->state->forced.remove(path);
    f->dirty  = true;
    f->replan = true;
}

bool SyncService::keepAside(const Folder& f, const QString& path) const {
    const QFileInfo name(path);
    const QString   stamp  = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    const QString   suffix = name.suffix();
    const QString   kept   = name.completeBaseName() + QLatin1Char('~') + stamp +
                         (suffix.isEmpty() ? QString() : QLatin1Char('.') + suffix);
    const QString dir = QDir(QDir(f.root).filePath(QLatin1String(kKeptDir))).filePath(
        path.contains(QLatin1Char('/')) ? path.section(QLatin1Char('/'), 0, -2) : QString());
    if (!QDir().mkpath(dir)) return false;
    return QFile::rename(absolute(f, path), uniquePath(dir, kept));
}

// Files kept longer than keepDays_ go on to the trash; empty folders left
// behind go too. Runs off the worker thread.
void SyncService::cleanKept(const QString& folderId) {
    Folder* f = folder(folderId);
    if (!f) return;
    f->nextCleanup = Clock::now() + kCleanEvery;
    if (f->root.isEmpty()) return;
    const QString root = QDir(f->root).filePath(QLatin1String(kKeptDir));
    if (!QFileInfo(root).isDir()) return;
    const QDateTime cutoff = QDateTime::currentDateTime().addDays(-keepDays_);
    runOffThread<int>(
        this,
        [root, cutoff, trash = useTrash_] {
            static const QRegularExpression stamp(QStringLiteral("~(\\d{8}-\\d{6})(?:\\.[^.]*)?$"));
            int gone = 0;
            std::function<void(const QString&)> clean = [&](const QString& dir) {
                for (const QFileInfo& fi : QDir(dir).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden)) {
                    if (fi.isSymLink() || fi.isJunction()) continue;
                    if (fi.isDir()) {
                        clean(fi.absoluteFilePath());
                        QDir().rmdir(fi.absoluteFilePath());  // only if it is empty now
                        continue;
                    }
                    const auto m = stamp.match(fi.fileName());
                    const QDateTime when = m.hasMatch()
                                               ? QDateTime::fromString(m.captured(1), QStringLiteral("yyyyMMdd-HHmmss"))
                                               : fi.lastModified();
                    if (!when.isValid() || when > cutoff) continue;
                    if ((trash && QFile::moveToTrash(fi.absoluteFilePath())) || QFile::remove(fi.absoluteFilePath())) ++gone;
                }
            };
            clean(root);
            return gone;
        },
        [](std::optional<int>) {});
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
    if (!f || !f->state->config.peers.value(p->second.key).sends()) return refuse("forbidden");
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
    Peer& peer = p->second;
    peer.served.add(data.size());
    peer.serving       = path;
    peer.servingFolder = msg.value("folder").toString();
    peer.servedAt      = Clock::now();
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
        QVariantList  offers;
        QSet<QString> offered;
        for (const auto& v : msg->value("folders").toArray()) {
            const QJsonObject o = v.toObject();
            const QString     id = o.value("id").toString();
            if (id.size() != 32 || QByteArray::fromHex(id.toLatin1()).size() != 16) continue;
            offered.insert(id);
            offers << QVariantMap{{"id", id}, {"label", o.value("label").toString().left(100)}};
        }
        // A folder it no longer sends us: what we held of its index is no
        // longer ours to act on. Its next hello brings a full index.
        Peer& peer = peers_.at(dev);
        for (auto& [id, f] : folders_) {
            if (offered.contains(id) || !f.state->remotes.contains(peer.key)) continue;
            f.state->remotes.remove(peer.key);
            peer.heard.remove(id);
            f.dirty  = true;
            f.replan = true;
        }
        emit offersChanged(peer.key, offers);
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

namespace {

qint64 doneOf(const sync::FileEntry& target, int missing) {
    return std::min<qint64>(target.size, qint64(target.blocks.size() - missing) * target.blockSize);
}

}  // namespace

QVariantMap SyncService::folderStatus(const QString& folderId, const Folder& f) const {
    qint64 files = 0, bytes = 0, dirs = 0;
    for (const FileEntry& e : f.state->local) {
        if (e.deleted) continue;
        if (e.dir) {
            ++dirs;
            continue;
        }
        ++files;
        bytes += e.size;
    }
    qint64 fetched = 0;
    QVariantList transfers;
    for (const auto& [path, p] : f.pulls) {
        fetched += doneOf(p.target, int(p.missing.size()));
        QString key;
        for (auto r = requests_.crbegin(); r != requests_.crend() && key.isEmpty(); ++r)
            if (r->second.folder == folderId && r->second.path == path)
                if (auto peer = peers_.find(r->second.dev); peer != peers_.end()) key = peer->second.key;
        if (key.isEmpty())
            for (const QString& source : p.sources)
                if (devOfKey_.contains(source)) {
                    key = source;
                    break;
                }
        transfers << QVariantMap{{"path", path}, {"size", p.target.size},
                                 {"done", doneOf(p.target, int(p.missing.size()))}, {"key", key}};
    }
    QVariantList pending;
    for (const QString& path : f.pendingDeletes.mid(0, 200)) pending << path;
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
        {"dirs", dirs},
        {"free", f.free},
        {"needFiles", f.needFiles},
        {"needBytes", std::max<qint64>(0, f.needBytes - fetched)},
        {"conflicts", f.conflicts},
        {"blocked", blocked},
        {"held", f.held},
        {"members", memberStatus(folderId, f)},
        {"pendingDeletes", pending},
        {"pendingDeleteCount", f.pendingDeletes.size()},
        {"transfers", transfers},
    };
}

QVariantList SyncService::memberStatus(const QString& folderId, const Folder& f) const {
    QVariantList out;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const auto   now   = Clock::now();
    QStringList keys = f.state->config.peers.keys();
    std::sort(keys.begin(), keys.end());
    for (const QString& key : keys) {
        const Grant     g    = f.state->config.peers.value(key);
        const Peer*     peer = peerOf(key);
        const Folder::Need need  = f.needs.value(key);
        const auto         stats = f.state->peerStats.value(key);
        // What is on its way from it does not count as left.
        qint64 receiveBytes = need.receiveBytes;
        for (const auto& [path, p] : f.pulls)
            if (p.sources.contains(key)) receiveBytes -= doneOf(p.target, int(p.missing.size()));
        receiveBytes = std::max<qint64>(0, receiveBytes);
        QString current;
        bool    sending = false;
        if (peer) {
            const auto d = devOfKey_.constFind(key);
            for (auto r = requests_.crbegin(); r != requests_.crend(); ++r)
                if (r->second.dev == *d && r->second.folder == folderId) {
                    current = r->second.path;
                    break;
                }
            if (current.isEmpty() && peer->servingFolder == folderId && now - peer->servedAt < 3s) {
                current = peer->serving;
                sending = true;
            }
        }
        const qint64 rate     = peer ? peer->received.rate() : 0;
        const qint64 sendRate = peer ? peer->served.rate() : 0;
        out << QVariantMap{
            {"key", key},
            {"online", peer != nullptr},
            {"paused", g.paused},
            {"toSendFiles", need.sendFiles},
            {"toSendBytes", need.sendBytes},
            {"toSendChanges", need.sendChanges},
            {"toReceiveFiles", need.receiveFiles},
            {"toReceiveBytes", receiveBytes},
            {"toReceiveChanges", need.receiveChanges},
            {"current", current},
            {"sending", sending},
            {"rate", rate},
            {"sendRate", sendRate},
            {"eta", rate > 0 && receiveBytes > 0 ? qint64(receiveBytes / rate) : qint64(-1)},
            {"skipped", need.skipped},
            {"lastSeen", peer ? nowMs : stats.lastSeen},
            {"lastInSync", stats.lastInSync},
        };
    }
    return out;
}

void SyncService::emitStatus(const QString& folderId) {
    Folder* f = folder(folderId);
    if (!f) return;
    const QVariantMap status = folderStatus(folderId, *f);
    if (status == f->lastStatus) return;
    // Progress alone need not reach the UI with every block.
    const auto now = Clock::now();
    if (status.value("state") == f->lastStatus.value("state") && now - f->lastEmit < kEmitEvery) return;
    f->lastEmit   = now;
    f->lastStatus = status;
    emit folderChanged(folderId, status);
}

// --- the folder as the user sees it -------------------------------------------------

QVariantList SyncService::files(const QString& folderId) const {
    const auto it = folders_.find(folderId);
    if (it == folders_.end()) return {};
    const Folder&     f = it->second;
    const SyncFolder& s = *f.state;
    const auto since = [this](const QString& key) { return sinceOf(key); };
    QSet<QString> conflicts;
    for (const QVariant& c : f.conflicts) conflicts.insert(c.toMap().value("path").toString());
    QStringList sendKeys;
    for (auto g = s.config.peers.cbegin(); g != s.config.peers.cend(); ++g)
        if (g->sends()) sendKeys << g.key();

    QSet<QString> paths(f.leftOut.keyBegin(), f.leftOut.keyEnd());
    for (const QString& p : s.allPaths()) paths.insert(p);
    QStringList sorted(paths.begin(), paths.end());
    std::sort(sorted.begin(), sorted.end());

    QVariantList out;
    for (const QString& path : sorted) {
        QVariantMap row{{"path", path}, {"name", path.section(QLatin1Char('/'), -1)}};
        auto fill = [&](const FileEntry& e) {
            row.insert("dir", e.dir);
            row.insert("size", e.size);
            row.insert("mtime", e.mtime);
            row.insert("version", versionMap(e.version));
        };
        QString state, detail;
        double  progress = -1;
        const auto local = s.local.constFind(path);
        const bool mine  = local != s.local.cend() && !local->deleted;
        if (mine) fill(*local);

        if (const auto left = f.leftOut.constFind(path); left != f.leftOut.cend()) {
            row.insert("dir", left->dir);
            row.insert("size", left->size);
            row.insert("mtime", 0);
            state  = left->skip ? QStringLiteral("skipped") : QStringLiteral("ignored");
            detail = (left->skip ? s.skip : s.ignore).explain(path, left->dir).pattern;
        } else {
            bool isDir = mine && local->dir;
            for (const auto& r : s.remotes)
                if (const auto e = r.entries.constFind(path); e != r.entries.cend() && e->dir) isDir = true;
            const Decision d = s.decide(path, since);
            if (!s.participates(path, isDir)) {
                if (!mine) {  // only other devices have it; here the rules leave it out
                    for (const auto& r : s.remotes)
                        if (const auto e = r.entries.constFind(path); e != r.entries.cend() && !e->deleted) {
                            fill(*e);
                            break;
                        }
                    if (!row.contains("dir")) continue;
                }
                const bool ignored = s.ignore.matches(path, isDir);
                state  = ignored ? QStringLiteral("ignored") : QStringLiteral("skipped");
                detail = (ignored ? s.ignore : s.skip).explain(path, isDir).pattern;
            } else if (conflicts.contains(path) || (d.kind == Decision::Conflict && !d.apply)) {
                if (!mine)
                    for (const Candidate& c : d.candidates)
                        if (!c.entry.deleted) {
                            fill(c.entry);
                            break;
                        }
                if (!row.contains("dir")) fill(d.candidates.isEmpty() ? FileEntry{} : d.candidates.first().entry);
                state = QStringLiteral("conflict");
            } else if (const auto pull = f.pulls.find(path); pull != f.pulls.end()) {
                fill(pull->second.target);
                bool online = false;
                for (const QString& source : pull->second.sources) online = online || devOfKey_.contains(source);
                if (online) {
                    state    = QStringLiteral("transferring");
                    progress = pull->second.target.size > 0
                                   ? double(doneOf(pull->second.target, int(pull->second.missing.size()))) /
                                         double(pull->second.target.size)
                                   : 1.0;
                } else {
                    state  = QStringLiteral("receive");
                    detail = QStringLiteral("waiting");
                }
            } else if (const auto b = f.blocked.constFind(path); b != f.blocked.cend()) {
                if (!mine) fill(d.target);
                state  = QStringLiteral("blocked");
                detail = *b;
            } else if (mine && local->skipped) {
                state  = QStringLiteral("held");
                detail = s.heldReason(path);
                if (detail.isEmpty()) {
                    state  = QStringLiteral("skipped");
                    detail = s.skip.explain(path, local->dir).pattern;
                }
            } else if (d.kind == Decision::Apply || (d.kind == Decision::Conflict && d.apply)) {
                if (d.target.deleted) {
                    if (!mine) continue;
                    state  = QStringLiteral("receive");
                    detail = QStringLiteral("delete");
                } else {
                    if (!mine) fill(d.target);
                    state = QStringLiteral("receive");
                    bool online = false;
                    for (const QString& source : d.sources) online = online || devOfKey_.contains(source);
                    if (!online) detail = QStringLiteral("waiting");
                }
            } else if (mine) {
                int lacking = 0;
                for (const QString& key : sendKeys)
                    if (pendingFor(f, *local, key)) ++lacking;
                state = lacking > 0 ? QStringLiteral("send") : QStringLiteral("synced");
                if (lacking > 0) detail = QString::number(lacking);
            } else {
                continue;  // a tombstone, or nothing to do
            }
        }
        row.insert("state", state);
        row.insert("detail", detail);
        row.insert("progress", progress);
        out << row;
    }
    return out;
}

QVariantMap SyncService::fileInfo(const QString& folderId, const QString& path) const {
    const auto it = folders_.find(folderId);
    if (it == folders_.end()) return {};
    const Folder&     f = it->second;
    const SyncFolder& s = *f.state;
    QVariantMap out{{"path", path}};
    if (const auto l = s.local.constFind(path); l != s.local.cend()) {
        out.insert("dir", l->dir);
        out.insert("deleted", l->deleted);
        out.insert("size", l->size);
        out.insert("mtime", l->mtime);
        out.insert("hash", QString::fromLatin1(l->hash.toHex()));
        out.insert("version", versionMap(l->version));
        out.insert("skipped", l->skipped);
    }
    QVariantList holders;
    QStringList  keys = s.remotes.keys();
    std::sort(keys.begin(), keys.end());
    for (const QString& key : keys) {
        const auto e = s.remotes[key].entries.constFind(path);
        if (e == s.remotes[key].entries.cend()) continue;
        holders << QVariantMap{{"key", key},         {"size", e->size},       {"mtime", e->mtime},
                               {"version", versionMap(e->version)}, {"deleted", e->deleted}, {"dir", e->dir},
                               {"skipped", e->skipped}, {"hash", QString::fromLatin1(e->hash.toHex())}};
    }
    out.insert("holders", holders);
    QVariantList kept;
    for (const QVariant& v : keptFiles(folderId))
        if (v.toMap().value("path").toString() == path) kept << v;
    out.insert("kept", kept);
    for (const QVariant& v : files(folderId)) {
        const QVariantMap row = v.toMap();
        if (row.value("path").toString() != path) continue;
        out.insert("state", row.value("state"));
        out.insert("detail", row.value("detail"));
        out.insert("progress", row.value("progress"));
        if (!out.contains("dir")) {
            out.insert("dir", row.value("dir"));
            out.insert("size", row.value("size"));
            out.insert("mtime", row.value("mtime"));
            out.insert("version", row.value("version"));
        }
        break;
    }
    return out;
}

// --- rules ------------------------------------------------------------------------

QString SyncService::rulesPath(const Folder& f, bool skipFile) const {
    return QDir(f.root).filePath(QLatin1String(skipFile ? kSkipFile : kIgnoreFile));
}

QString SyncService::rulesText(const QString& folderId, bool skipFile) const {
    const auto it = folders_.find(folderId);
    if (it == folders_.end() || it->second.root.isEmpty()) return {};
    QFile file(rulesPath(it->second, skipFile));
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.read(1024 * 1024)) : QString();
}

QVariantMap SyncService::previewRules(const QString& folderId, bool skipFile, const QString& text,
                                      const QString& probe) const {
    const auto it = folders_.find(folderId);
    if (it == folders_.end()) return {};
    const Folder&     f     = it->second;
    const SyncFolder& s     = *f.state;
    const IgnoreRules rules(text);
    const IgnoreRules& other = skipFile ? s.ignore : s.skip;  // the file not being edited, as it is

    QVariantList problems;
    for (const IgnoreRules::Problem& p : rules.problems()) problems << QVariantMap{{"line", p.line}, {"pattern", p.pattern}};

    QSet<QString> paths(f.leftOut.keyBegin(), f.leftOut.keyEnd());
    QHash<QString, bool> dirs;
    for (const FileEntry& e : s.local)
        if (!e.deleted) {
            paths.insert(e.path);
            dirs.insert(e.path, e.dir);
        }
    for (const auto& r : s.remotes)
        for (const FileEntry& e : r.entries)
            if (!e.deleted) {
                paths.insert(e.path);
                if (!dirs.contains(e.path)) dirs.insert(e.path, e.dir);
            }
    for (auto l = f.leftOut.cbegin(); l != f.leftOut.cend(); ++l)
        if (!dirs.contains(l.key())) dirs.insert(l.key(), l->dir);
    QStringList sorted(paths.begin(), paths.end());
    std::sort(sorted.begin(), sorted.end());

    QVariantList matches;
    qint64 stop = 0, resume = 0;
    for (const QString& path : sorted) {
        const bool dir = dirs.value(path);
        const IgnoreRules::Match m = rules.explain(path, dir);
        if (m.line > 0 && matches.size() < kMaxListed)
            matches << QVariantMap{{"path", path}, {"dir", dir}, {"line", m.line}, {"pattern", m.pattern},
                                   {"negate", m.negate}, {"via", m.via}};
        if (dir) continue;
        const bool wouldBeOut = rules.matches(path, false) || other.matches(path, false);
        const auto local      = s.local.constFind(path);
        const bool syncedNow  = local != s.local.cend() && !local->deleted && !local->skipped && s.participates(path);
        if (syncedNow && wouldBeOut) ++stop;
        const auto left = f.leftOut.constFind(path);
        if (left != f.leftOut.cend() && left->skip == skipFile && !wouldBeOut) ++resume;
    }
    QVariantMap out{{"problems", problems}, {"matches", matches}, {"stop", stop}, {"resume", resume}};
    if (!probe.isEmpty()) {
        QString p = probe.trimmed();
        p.replace(QLatin1Char('\\'), QLatin1Char('/'));
        while (p.startsWith(QLatin1Char('/'))) p.remove(0, 1);
        const bool dir = p.endsWith(QLatin1Char('/')) || dirs.value(p);
        while (p.endsWith(QLatin1Char('/'))) p.chop(1);
        const IgnoreRules::Match m = rules.explain(p, dir);
        out.insert("probe", QVariantMap{{"path", p}, {"dir", dir}, {"line", m.line}, {"pattern", m.pattern},
                                        {"negate", m.negate}, {"via", m.via},
                                        {"excluded", m.line > 0 && !m.negate}});
    }
    return out;
}

bool SyncService::saveRules(const QString& folderId, bool skipFile, const QString& text) {
    Folder* f = folder(folderId);
    if (!f || f->root.isEmpty()) return false;
    QSaveFile file(rulesPath(*f, skipFile));
    if (!file.open(QIODevice::WriteOnly)) return false;
    file.write(text.toUtf8());
    if (!file.commit()) return false;
    f->rescanSoon = true;
    f->replan     = true;
    return true;
}

bool SyncService::addRule(const QString& folderId, bool skipFile, const QString& path) {
    Folder* f = folder(folderId);
    if (!f || f->root.isEmpty() || !isValidSyncPath(path)) return false;
    QString text = rulesText(folderId, skipFile);
    if (!text.isEmpty() && !text.endsWith(QLatin1Char('\n'))) text += QLatin1Char('\n');
    text += exactPattern(path) + QLatin1Char('\n');
    return saveRules(folderId, skipFile, text);
}

// --- kept files -------------------------------------------------------------------

QVariantList SyncService::keptFiles(const QString& folderId) const {
    const auto it = folders_.find(folderId);
    if (it == folders_.end() || it->second.root.isEmpty()) return {};
    const QString root = QDir(it->second.root).filePath(QLatin1String(kKeptDir));
    if (!QFileInfo(root).isDir()) return {};
    QVariantList out;
    std::function<void(const QString&, const QString&)> walk = [&](const QString& dir, const QString& rel) {
        for (const QFileInfo& fi : QDir(dir).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden)) {
            if (fi.isSymLink() || fi.isJunction()) continue;
            const QString child = rel.isEmpty() ? fi.fileName() : rel + QLatin1Char('/') + fi.fileName();
            if (fi.isDir()) {
                walk(fi.absoluteFilePath(), child);
                continue;
            }
            const auto m = kKeptName.match(fi.fileName());
            QString   original = fi.fileName();
            qint64    when     = fi.lastModified().toMSecsSinceEpoch();
            if (m.hasMatch()) {
                original = m.captured(1) + m.captured(3);
                const QDateTime stamp = QDateTime::fromString(m.captured(2), QStringLiteral("yyyyMMdd-HHmmss"));
                if (stamp.isValid()) when = stamp.toMSecsSinceEpoch();
            }
            out << QVariantMap{{"kept", child},
                               {"path", rel.isEmpty() ? original : rel + QLatin1Char('/') + original},
                               {"time", when},
                               {"size", fi.size()}};
        }
    };
    walk(root, {});
    std::sort(out.begin(), out.end(), [](const QVariant& a, const QVariant& b) {
        return a.toMap().value("time").toLongLong() > b.toMap().value("time").toLongLong();
    });
    return out;
}

bool SyncService::restoreKept(const QString& folderId, const QString& kept) {
    Folder* f = folder(folderId);
    if (!f || f->root.isEmpty()) return false;
    const QString keptRoot = QDir(f->root).filePath(QLatin1String(kKeptDir));
    const QString source   = QDir(keptRoot).filePath(kept);
    const QString real     = realPath(source);
    if (real.isEmpty() || !isWithin(real, keptRoot) || !QFileInfo(source).isFile()) return false;
    const QFileInfo fi(source);
    const auto      m        = kKeptName.match(fi.fileName());
    const QString   original = m.hasMatch() ? m.captured(1) + m.captured(3) : fi.fileName();
    const QString   relDir   = kept.contains(QLatin1Char('/')) ? kept.section(QLatin1Char('/'), 0, -2) : QString();
    const QString   relPath  = relDir.isEmpty() ? original : relDir + QLatin1Char('/') + original;
    if (!isValidSyncPath(relPath) || !safeParent(*f, relPath, true)) return false;
    const QString target = uniquePath(QFileInfo(absolute(*f, relPath)).absolutePath(), original);
    if (!QFile::rename(source, target)) return false;
    f->rescanSoon = true;  // it comes back as a change of ours
    return true;
}

bool SyncService::discardKept(const QString& folderId, const QString& kept) {
    Folder* f = folder(folderId);
    if (!f || f->root.isEmpty()) return false;
    const QString keptRoot = QDir(f->root).filePath(QLatin1String(kKeptDir));
    const QString source   = QDir(keptRoot).filePath(kept);
    const QString real     = realPath(source);
    if (real.isEmpty() || !isWithin(real, keptRoot) || !QFileInfo(source).isFile()) return false;
    return (useTrash_ && QFile::moveToTrash(source)) || QFile::remove(source);
}

// --- held deletes, transfers --------------------------------------------------------

void SyncService::confirmDeletes(const QString& folderId, bool apply) {
    Folder* f = folder(folderId);
    if (!f || f->pendingDeletes.isEmpty()) return;
    if (apply) {
        for (const QString& path : f->pendingDeletes) f->approvedDeletes.insert(path);
    } else {
        // Ours again, newer than the deletes: the other devices take them back.
        for (const QString& path : f->pendingDeletes) f->state->keepMine(path);
    }
    f->pendingDeletes.clear();
    f->dirty  = true;
    f->replan = true;
}

QVariantList SyncService::transfers() const {
    QVariantList out, queued;
    for (const auto& [id, f] : folders_) {
        const QVariantMap status = folderStatus(id, f);
        for (const QVariant& v : status.value("transfers").toList()) {
            QVariantMap t = v.toMap();
            t.insert("folder", id);
            t.insert("active", true);
            out << t;
        }
        for (const QString& path : f.queued) {
            qint64 size = 0;
            for (const auto& r : f.state->remotes)
                if (const auto e = r.entries.constFind(path); e != r.entries.cend() && !e->deleted) size = e->size;
            queued << QVariantMap{{"folder", id}, {"path", path}, {"size", size}, {"done", 0}, {"key", QString()},
                                  {"active", false}};
        }
    }
    out += queued;
    return out;
}

qint64 SyncService::receiveRate() const {
    qint64 rate = 0;
    for (const auto& [dev, peer] : peers_) rate += peer.received.rate();
    return rate;
}

}  // namespace ucd
