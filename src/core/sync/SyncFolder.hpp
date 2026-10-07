#pragma once
// One synced folder on this device: its index (what this device has), the
// indexes its peers sent, and the rules that decide, path by path, what to do.
// No network or disk writes happen here; SyncService carries out decisions.

#include <functional>

#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include "core/sync/SyncIgnore.hpp"
#include "core/sync/SyncModel.hpp"
#include "core/sync/SyncScanner.hpp"

namespace ucd::sync {

// What this device allows one peer, for one folder.
struct Grant {
    bool receive    = false;  // send it this folder and our changes
    bool contribute = false;  // accept its changes
    bool operator==(const Grant&) const = default;
};

struct FolderConfig {
    QString id;     // 32 hex, the same on every member
    QString label;
    QString path;   // where it is on this device
    bool    paused = false;
    QHash<QString, Grant> peers;  // by identity key (hex)
    bool operator==(const FolderConfig&) const = default;
};

// One of the versions competing in a conflict.
struct Candidate {
    QString   key;  // identity key of a device holding it; empty for this device
    FileEntry entry;
};

struct Decision {
    enum Kind {
        None,      // nothing to do
        Adopt,     // same content as we have, newer version: take the version only
        Apply,     // bring the disk to `target` (fetch, create a folder or delete)
        Conflict,  // versions that neither saw; see the fields below
    };
    Kind      kind = None;
    FileEntry target;
    QStringList sources;  // keys of peers holding the content of `target`

    // Conflict
    bool    involved = false;  // this device's own change is one of them
    QString following;         // the chain kept: a peer's key, or empty for ours
    bool    waiting  = false;  // behind every candidate, and none of their holders is online
    bool    apply    = false;  // following a peer: `target` and `sources` say what to fetch
    QVector<Candidate> candidates;
};

class SyncFolder {
public:
    SyncFolder(FolderConfig config, QString me, QString stateFile);

    FolderConfig config;
    const QString me;  // this device's member id

    quint64 epoch = 0;  // changes when this index starts over; peers then ask for all of it
    quint64 seq   = 0;  // the last change to the index
    QHash<QString, FileEntry> local;

    struct Remote {
        quint64 epoch = 0;
        quint64 seen  = 0;  // highest seq of theirs we have
        QHash<QString, FileEntry> entries;
    };
    QHash<QString, Remote> remotes;  // by identity key

    IgnoreRules ignore;  // .syncignore and .syncskip, as of the last scan
    IgnoreRules skip;
    // Resolutions waiting for their content: path -> what the entry becomes.
    QHash<QString, FileEntry> forced;

    bool participates(const QString& path, bool dir = false) const;
    void setLocal(FileEntry e);
    // Entries changed after `since`, oldest change first.
    QVector<FileEntry> changedSince(quint64 since) const;

    // Turns what a scan saw into index changes; entries changed after
    // `seqAtStart` (by applying a peer's change meanwhile) are left alone.
    // Returns how many entries changed.
    int applyScan(const ScanResult& result, quint64 seqAtStart);

    // A peer's index, or a part of it. `full`: everything it has, so what we
    // held of it before goes.
    void applyRemote(const QString& key, quint64 epoch, bool full, const QVector<FileEntry>& entries);

    // `connectedSince(key)`: when the connection to that peer began (ms since
    // epoch), or -1 if it is not connected.
    Decision decide(const QString& path, const std::function<qint64(const QString&)>& connectedSince) const;
    QStringList allPaths() const;

    // Conflict resolutions. Each gives the result a version newer than every
    // candidate, so it settles the conflict on every device it reaches.
    bool keepMine(const QString& path);
    bool take(const QString& path, const QString& key);

    bool load();
    bool save() const;
    void removeState() const;

private:
    QVector<Candidate> candidatesFor(const QString& path) const;
    QStringList        holdersOf(const FileEntry& content) const;

    QString stateFile_;
};

}  // namespace ucd::sync
