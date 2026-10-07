#include "SyncFolder.hpp"

#include <algorithm>

#include <QCborArray>
#include <QCborMap>
#include <QCborValue>
#include <QFile>
#include <QRandomGenerator>
#include <QSaveFile>

namespace ucd::sync {

SyncFolder::SyncFolder(FolderConfig cfg, QString meId, QString stateFile)
    : config(std::move(cfg)), me(std::move(meId)), stateFile_(std::move(stateFile)) {
    epoch = QRandomGenerator::global()->generate64() >> 1;
}

bool SyncFolder::participates(const QString& path, bool dir) const {
    return !ignore.matches(path, dir) && !skip.matches(path, dir);
}

void SyncFolder::setLocal(FileEntry e) {
    e.seq = ++seq;
    local.insert(e.path, std::move(e));
}

QVector<FileEntry> SyncFolder::changedSince(quint64 since) const {
    QVector<FileEntry> out;
    for (const FileEntry& e : local)
        if (e.seq > since) out << e;
    std::sort(out.begin(), out.end(), [](const FileEntry& a, const FileEntry& b) { return a.seq < b.seq; });
    return out;
}

namespace {

// Of names that clash ignoring case, the oldest is synced; ties go to the
// lower name. Folders have no time, so their names decide.
bool olderThan(qint64 aTime, const QString& a, qint64 bTime, const QString& b) {
    return aTime != bTime ? aTime < bTime : a < b;
}

// `held` plus everything inside a held folder.
void holdDescendants(QHash<QString, QString>& held, const QStringList& paths) {
    if (held.isEmpty()) return;
    for (const QString& p : paths) {
        for (qsizetype slash = p.indexOf(QLatin1Char('/')); slash >= 0; slash = p.indexOf(QLatin1Char('/'), slash + 1)) {
            const auto parent = held.constFind(p.left(slash));
            if (parent != held.cend()) {
                held.insert(p, *parent);
                break;
            }
        }
    }
}

}  // namespace

QString SyncFolder::heldReason(const QString& path) const {
    const auto s = scanHeld.constFind(path);
    return s != scanHeld.cend() ? *s : clashHeld.value(path);
}

QHash<QString, QString> SyncFolder::clashes(const QHash<QString, Decision>& decisions) const {
    struct Live {
        QString path;
        qint64  mtime = 0;
    };
    QHash<QString, QVector<Live>> byName;
    QSet<QString> paths(local.keyBegin(), local.keyEnd());
    for (auto it = decisions.keyBegin(); it != decisions.keyEnd(); ++it) paths.insert(*it);
    QStringList live;
    for (const QString& p : paths) {
        const auto d = decisions.constFind(p);
        if (d != decisions.cend() && (d->kind == Decision::Apply || (d->kind == Decision::Conflict && d->apply))) {
            if (d->target.deleted) continue;
            byName[foldName(p)] << Live{p, d->target.dir ? 0 : d->target.mtime};
            live << p;
            continue;
        }
        const auto l = local.constFind(p);
        if (l == local.cend() || l->deleted || scanHeld.contains(p)) continue;
        // Skipped for .syncskip takes no part; held back for a clash still competes.
        if (l->skipped && !clashHeld.contains(p)) continue;
        byName[foldName(p)] << Live{p, l->dir ? 0 : l->mtime};
        live << p;
    }
    QHash<QString, QString> held;
    for (const QVector<Live>& same : byName) {
        if (same.size() < 2) continue;
        const Live* oldest = &same.first();
        for (const Live& l : same)
            if (olderThan(l.mtime, l.path, oldest->mtime, oldest->path)) oldest = &l;
        for (const Live& l : same)
            if (l.path != oldest->path) held.insert(l.path, QStringLiteral("clash:") + oldest->path);
    }
    holdDescendants(held, live);
    return held;
}

int SyncFolder::applyScan(const ScanResult& result, quint64 seqAtStart) {
    if (!result.ok) return 0;
    // The rules this scan went by; the caller may hold newer ones.
    const IgnoreRules scanIgnore(result.ignoreText), scanSkip(result.skipText);
    const quint64 before = seq;

    // Names here that clash ignoring case (a case-sensitive disk can hold
    // both): the oldest syncs, the rest are held back. Names some member
    // could not hold are held back too.
    scanHeld.clear();
    QHash<QString, const FileEntry*> oldest;
    for (const FileEntry& obs : result.items) {
        const FileEntry*& o = oldest[foldName(obs.path)];
        if (!o) {
            o = &obs;
        } else if (olderThan(obs.dir ? 0 : obs.mtime, obs.path, o->dir ? 0 : o->mtime, o->path)) {
            scanHeld.insert(o->path, QStringLiteral("clash:") + obs.path);
            o = &obs;
        } else {
            scanHeld.insert(obs.path, QStringLiteral("clash:") + o->path);
        }
    }
    // A loser's own clash target may itself have lost to an older name later on.
    for (auto it = scanHeld.begin(); it != scanHeld.end(); ++it)
        it.value() = QStringLiteral("clash:") + oldest.value(foldName(it.key()))->path;
    QStringList observed;
    for (const FileEntry& obs : result.items) observed << obs.path;
    holdDescendants(scanHeld, observed);
    for (auto it = result.unsupported.cbegin(); it != result.unsupported.cend(); ++it)
        scanHeld.insert(it.key(), QStringLiteral("name:") + it.value());

    auto holdBack = [&](const QString& path) {
        const auto it = local.constFind(path);
        if (it != local.cend() && !it->deleted && !it->skipped) {
            FileEntry e = *it;
            e.skipped   = true;  // stays on disk; peers neither take nor delete it
            setLocal(e);
        }
    };

    QSet<QString> seen;
    for (const FileEntry& obs : result.items) {
        seen.insert(obs.path);
        const auto it = local.constFind(obs.path);
        if (it != local.cend() && it->seq > seqAtStart) continue;  // we changed it since the scan began
        if (scanHeld.contains(obs.path) || clashHeld.contains(obs.path)) {
            holdBack(obs.path);
            continue;
        }
        if (it != local.cend() && !it->deleted && it->dir == obs.dir && (obs.dir || it->hash == obs.hash)) {
            if (it->skipped) {  // taking part again, with the same content
                FileEntry e = *it;
                e.skipped   = false;
                setLocal(e);
            } else if (!obs.dir && (it->mtime != obs.mtime || it->size != obs.size)) {
                local[obs.path].mtime = obs.mtime;  // touched, not changed: nothing to tell anyone
            }
            continue;
        }
        FileEntry e = obs;
        e.version   = bumped(it != local.cend() ? it->version : VersionVector{}, me);
        setLocal(e);
    }

    const QStringList paths = local.keys();
    for (const QString& path : paths) {
        const FileEntry& l = local[path];
        if (l.deleted || seen.contains(path) || l.seq > seqAtStart || result.unstable.contains(path)) continue;
        if (scanHeld.contains(path) || clashHeld.contains(path)) {
            holdBack(path);
            continue;
        }
        if (scanSkip.matches(path, l.dir) && !scanIgnore.matches(path, l.dir)) {
            if (!l.skipped) {
                FileEntry e = l;
                e.skipped   = true;
                setLocal(e);
            }
            continue;
        }
        // Gone from disk, or newly ignored (which counts as deleted; the file
        // itself is left alone).
        FileEntry e;
        e.path    = path;
        e.deleted = true;
        e.version = bumped(l.version, me);
        setLocal(e);
    }
    return int(seq - before);
}

void SyncFolder::applyRemote(const QString& key, quint64 remoteEpoch, bool full, const QVector<FileEntry>& entries) {
    Remote& r = remotes[key];
    if (full || r.epoch != remoteEpoch) {
        r.entries.clear();
        r.seen = 0;
    }
    r.epoch = remoteEpoch;
    for (const FileEntry& e : entries) {
        r.entries.insert(e.path, e);
        r.seen = std::max(r.seen, e.seq);
    }
}

QVector<Candidate> SyncFolder::candidatesFor(const QString& path) const {
    QVector<Candidate> all;
    const auto l = local.constFind(path);
    if (l != local.cend() && !l->skipped) all << Candidate{{}, *l};
    for (auto r = remotes.cbegin(); r != remotes.cend(); ++r) {
        if (!config.peers.value(r.key()).contribute) continue;
        const auto e = r->entries.constFind(path);
        if (e != r->entries.cend() && !e->skipped) all << Candidate{r.key(), *e};
    }
    return all;
}

QStringList SyncFolder::holdersOf(const FileEntry& content) const {
    QStringList out;
    if (content.deleted || content.dir) return out;
    for (auto r = remotes.cbegin(); r != remotes.cend(); ++r) {
        if (!config.peers.value(r.key()).contribute) continue;
        const auto e = r->entries.constFind(content.path);
        if (e != r->entries.cend() && !e->skipped && e->sameContent(content)) out << r.key();
    }
    return out;
}

Decision SyncFolder::decide(const QString& path, const std::function<qint64(const QString&)>& connectedSince) const {
    Decision d;
    const auto l     = local.constFind(path);
    bool       isDir = l != local.cend() && l->dir;
    for (const Remote& r : remotes)
        if (const auto e = r.entries.constFind(path); e != r.entries.cend() && e->dir) isDir = true;
    if (!participates(path, isDir)) return d;

    // A conflict resolution waiting for its content.
    if (const auto f = forced.constFind(path); f != forced.cend()) {
        d.target = *f;
        if (l != local.cend() && l->sameContent(*f)) {
            d.kind           = Decision::Adopt;
            d.target         = *l;
            d.target.version = f->version;
        } else {
            d.kind    = Decision::Apply;
            d.sources = holdersOf(*f);
        }
        return d;
    }

    const QVector<Candidate> all = candidatesFor(path);
    if (all.isEmpty()) return d;

    // The newest versions: those no other candidate has seen past.
    QVector<int> maxima;
    for (int i = 0; i < all.size(); ++i) {
        bool dominated = false;
        for (int j = 0; j < all.size() && !dominated; ++j)
            dominated = i != j && compare(all[i].entry.version, all[j].entry.version) == Order::Older;
        if (!dominated) maxima << i;
    }
    // Grouped by what they hold: different versions of the same content are
    // no conflict.
    QVector<QVector<int>> groups;
    for (int i : maxima) {
        auto g = std::find_if(groups.begin(), groups.end(),
                              [&](const QVector<int>& group) { return all[group.first()].entry.sameContent(all[i].entry); });
        if (g == groups.end()) groups.append(QVector<int>{i});
        else g->append(i);
    }
    auto mergedOf = [&](const QVector<int>& group) {
        VersionVector v;
        for (int i : group) v = merged(v, all[i].entry.version);
        return v;
    };
    // Prefer our own entry as the representative: it has our size and time.
    auto representative = [&](const QVector<int>& group) {
        for (int i : group)
            if (all[i].key.isEmpty()) return all[i].entry;
        return all[group.first()].entry;
    };

    const bool haveLocal = l != local.cend() && !l->skipped;
    if (groups.size() == 1) {
        FileEntry target = representative(groups.first());
        target.version   = mergedOf(groups.first());
        if (haveLocal && l->sameContent(target)) {
            if (l->version == target.version) return d;
            d.kind   = Decision::Adopt;
            d.target = *l;
            d.target.version = target.version;
            return d;
        }
        d.kind    = Decision::Apply;
        d.target  = target;
        d.sources = holdersOf(target);
        return d;
    }

    // A conflict.
    d.kind = Decision::Conflict;
    int localGroup = -1;
    for (int g = 0; g < groups.size(); ++g) {
        FileEntry rep = representative(groups[g]);
        rep.version   = mergedOf(groups[g]);
        QString key;
        for (int i : groups[g])
            if (all[i].key.isEmpty()) localGroup = g;
        if (localGroup != g) key = all[groups[g].first()].key;
        d.candidates << Candidate{key, rep};
    }
    if (localGroup >= 0) {
        // We hold one of them. It is ours if it has a change of ours the
        // others never saw; otherwise we just stay on the chain we are on.
        const quint64 mine = l->version.value(me, 0);
        d.involved = mine > 0;
        for (int g = 0; g < groups.size(); ++g)
            if (g != localGroup && mergedOf(groups[g]).value(me, 0) >= mine) d.involved = false;
        return d;
    }
    // Behind all of them: follow the one held by the peer we have been
    // connected to longest.
    int     best = -1;
    qint64  bestSince = 0;
    QString bestKey;
    for (int g = 0; g < groups.size(); ++g) {
        for (int i : groups[g]) {
            const qint64 since = all[i].key.isEmpty() ? -1 : connectedSince(all[i].key);
            if (since < 0) continue;
            if (best < 0 || since < bestSince) {
                best      = g;
                bestSince = since;
                bestKey   = all[i].key;
            }
        }
    }
    if (best < 0) {
        d.waiting = true;
        return d;
    }
    d.following      = bestKey;
    d.apply          = true;
    d.target         = representative(groups[best]);
    d.target.version = mergedOf(groups[best]);
    d.sources        = holdersOf(d.target);
    return d;
}

QStringList SyncFolder::allPaths() const {
    QSet<QString> paths(local.keyBegin(), local.keyEnd());
    for (const Remote& r : remotes)
        for (auto it = r.entries.keyBegin(); it != r.entries.keyEnd(); ++it) paths.insert(*it);
    for (auto it = forced.keyBegin(); it != forced.keyEnd(); ++it) paths.insert(*it);
    QStringList out(paths.begin(), paths.end());
    // Folders before what is in them; deletes are handled deepest first by the caller.
    std::sort(out.begin(), out.end());
    return out;
}

bool SyncFolder::keepMine(const QString& path) {
    const auto l = local.constFind(path);
    if (l == local.cend()) return false;
    VersionVector v;
    for (const Candidate& c : candidatesFor(path)) v = merged(v, c.entry.version);
    FileEntry e = *l;
    e.skipped   = false;
    e.version   = bumped(v, me);
    forced.remove(path);
    setLocal(e);
    return true;
}

bool SyncFolder::take(const QString& path, const QString& key) {
    const auto r = remotes.constFind(key);
    if (r == remotes.cend()) return false;
    const auto e = r->entries.constFind(path);
    if (e == r->entries.cend()) return false;
    VersionVector v;
    for (const Candidate& c : candidatesFor(path)) v = merged(v, c.entry.version);
    FileEntry target = *e;
    target.skipped   = false;
    target.version   = bumped(v, me);
    forced.insert(path, target);
    return true;
}

bool SyncFolder::load() {
    QFile f(stateFile_);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QCborMap m = QCborValue::fromCbor(f.readAll()).toMap();
    if (m.isEmpty()) return false;
    epoch = quint64(m.value(QStringLiteral("epoch")).toInteger());
    seq   = quint64(m.value(QStringLiteral("seq")).toInteger());
    local.clear();
    for (const auto& v : m.value(QStringLiteral("local")).toArray()) {
        FileEntry e = FileEntry::fromCbor(v.toMap());
        local.insert(e.path, e);
    }
    remotes.clear();
    const QCborMap rs = m.value(QStringLiteral("remotes")).toMap();
    for (auto it = rs.begin(); it != rs.end(); ++it) {
        const QCborMap rm = it.value().toMap();
        Remote r;
        r.epoch = quint64(rm.value(QStringLiteral("epoch")).toInteger());
        r.seen  = quint64(rm.value(QStringLiteral("seen")).toInteger());
        for (const auto& v : rm.value(QStringLiteral("entries")).toArray()) {
            FileEntry e = FileEntry::fromCbor(v.toMap());
            r.entries.insert(e.path, e);
        }
        remotes.insert(it.key().toString(), r);
    }
    const QCborMap fs = m.value(QStringLiteral("forced")).toMap();
    for (auto it = fs.begin(); it != fs.end(); ++it)
        forced.insert(it.key().toString(), FileEntry::fromCbor(it.value().toMap()));
    return true;
}

bool SyncFolder::save() const {
    QCborMap m;
    m.insert(QStringLiteral("epoch"), qint64(epoch));
    m.insert(QStringLiteral("seq"), qint64(seq));
    QCborArray l;
    for (const FileEntry& e : local) l.append(e.toCbor());
    m.insert(QStringLiteral("local"), l);
    QCborMap rs;
    for (auto it = remotes.cbegin(); it != remotes.cend(); ++it) {
        QCborMap rm;
        rm.insert(QStringLiteral("epoch"), qint64(it->epoch));
        rm.insert(QStringLiteral("seen"), qint64(it->seen));
        QCborArray es;
        for (const FileEntry& e : it->entries) es.append(e.toCbor());
        rm.insert(QStringLiteral("entries"), es);
        rs.insert(it.key(), rm);
    }
    m.insert(QStringLiteral("remotes"), rs);
    QCborMap fs;
    for (auto it = forced.cbegin(); it != forced.cend(); ++it) fs.insert(it.key(), it->toCbor());
    m.insert(QStringLiteral("forced"), fs);

    QSaveFile f(stateFile_);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QCborValue(m).toCbor());
    return f.commit();
}

void SyncFolder::removeState() const {
    QFile::remove(stateFile_);
}

}  // namespace ucd::sync
