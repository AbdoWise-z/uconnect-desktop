#pragma once
// Reads a synced folder as it is on disk. Runs on a pool thread: it touches
// nothing but the folder and what it is given.

#include <atomic>
#include <memory>

#include <QHash>
#include <QSet>
#include <QString>
#include <QVector>

#include "core/sync/SyncModel.hpp"

namespace ucd::sync {

struct ScanInput {
    QString root;  // real path of the synced folder
    // What the index says, so unchanged files (same size and time) are not
    // read again.
    QHash<QString, FileEntry> known;
    // Files changed more recently than this are still being written; they
    // are left for a later scan.
    qint64 stableAfterMs = 2000;
    std::shared_ptr<std::atomic<bool>> cancel;
};

struct ScanResult {
    bool    ok = true;  // false: the folder itself is missing or unreadable
    QString ignoreText;  // .syncignore and .syncskip as read for this scan
    QString skipText;
    QVector<FileEntry> items;  // everything present and taking part; no versions yet
    QSet<QString>      unstable;  // present but still changing
};

ScanResult scanFolder(const ScanInput& input);

// Hashes one file into blocks; false if it cannot be read or changed meanwhile.
bool hashFile(const QString& path, FileEntry* entry, const std::atomic<bool>* cancel = nullptr);

}  // namespace ucd::sync
