#include "SyncScanner.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include "core/sync/SyncIgnore.hpp"

namespace ucd::sync {
namespace {

QString readSmallFile(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.read(1024 * 1024));
}

struct Walker {
    const ScanInput&   in;
    const IgnoreRules& ignore;
    const IgnoreRules& skip;
    ScanResult&        out;
    qint64             now = QDateTime::currentMSecsSinceEpoch();

    void walk(const QString& dirPath, const QString& relDir) {
        if (in.cancel && *in.cancel) return;
        const QFileInfoList children = QDir(dirPath).entryInfoList(
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name);
        for (const QFileInfo& fi : children) {
#ifdef Q_OS_MACOS
            // macOS hands names out in another Unicode form and finds files by
            // either; the index keeps the form every other system uses.
            const QString name = fi.fileName().normalized(QString::NormalizationForm_C);
#else
            const QString name = fi.fileName();
#endif
            const QString rel  = relDir.isEmpty() ? name : relDir + QLatin1Char('/') + name;
            // Links are not synced: following one could lead out of the folder.
            if (fi.isSymLink() || fi.isJunction()) continue;
            if (name.startsWith(QLatin1String(kPartPrefix))) continue;
            if (relDir.isEmpty() && (name == QLatin1String(kSkipFile) || name == QLatin1String(kKeptDir))) continue;
            const bool dir = fi.isDir();
            if (ignore.matches(rel, dir) || skip.matches(rel, dir)) continue;
            if (const QString problem = nameProblem(name); !problem.isEmpty()) {
                out.unsupported.insert(rel, problem);  // and nothing inside it, if a folder
                continue;
            }
            if (!isValidSyncPath(rel)) continue;

            FileEntry e;
            e.path = rel;
            e.dir  = dir;
            if (dir) {
                out.items << e;
                walk(fi.absoluteFilePath(), rel);
                continue;
            }
            e.size  = fi.size();
            e.mtime = fi.lastModified().toMSecsSinceEpoch();
            const qint64 age = now - e.mtime;  // a time in the future is not "still changing"
            if (age >= 0 && age < in.stableAfterMs) {
                out.unstable.insert(rel);
                continue;
            }
            const auto known = in.known.constFind(rel);
            if (known != in.known.cend() && !known->deleted && !known->dir && known->size == e.size &&
                known->mtime == e.mtime && known->hash.size() == 32) {
                e.hash      = known->hash;
                e.blockSize = known->blockSize;
                e.blocks    = known->blocks;
            } else if (!hashFile(fi.absoluteFilePath(), &e, in.cancel.get())) {
                out.unstable.insert(rel);  // changed while being read, or unreadable for now
                continue;
            }
            out.items << e;
        }
    }
};

}  // namespace

bool hashFile(const QString& path, FileEntry* e, const std::atomic<bool>* cancel) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QFileInfo before(path);
    e->size      = file.size();
    e->blockSize = blockSizeFor(e->size);
    e->blocks.clear();
    QCryptographicHash whole(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        if (cancel && *cancel) return false;
        const QByteArray block = file.read(e->blockSize);
        if (block.isEmpty()) break;
        whole.addData(block);
        e->blocks << QCryptographicHash::hash(block, QCryptographicHash::Sha256);
    }
    if (file.error() != QFileDevice::NoError) return false;
    e->hash = whole.result();
    // Written to while being read: the hashes describe no single version.
    const QFileInfo after(path);
    return after.size() == e->size && after.lastModified() == before.lastModified();
}

ScanResult scanFolder(const ScanInput& in) {
    ScanResult out;
    if (!QFileInfo(in.root).isDir()) {
        out.ok = false;
        return out;
    }
    out.ignoreText = readSmallFile(QDir(in.root).filePath(QLatin1String(kIgnoreFile)));
    out.skipText   = readSmallFile(QDir(in.root).filePath(QLatin1String(kSkipFile)));
    const IgnoreRules ignore(out.ignoreText), skip(out.skipText);
    Walker{in, ignore, skip, out}.walk(in.root, {});
    return out;
}

}  // namespace ucd::sync
