// Two StorageService instances joined back to back: one offers a folder, the
// other browses and downloads. Covers the protocol end to end and, above all,
// that nothing outside the shared folder can be reached.

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QRandomGenerator>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include <limits>
#include <memory>

#include "core/Permissions.hpp"
#include "core/Hello.hpp"
#include "core/StorageService.hpp"

using namespace ucd;

namespace {

uconnect::DevId devId(uint8_t b) {
    uconnect::DevId d{};
    d[0] = b;
    return d;
}
const uconnect::DevId kRequester = devId(1);
const uconnect::DevId kOfferer   = devId(2);

// Messages arrive on a later event-loop turn, as over a real connection, and
// like a real connection each direction holds only a few messages in flight:
// send() refuses beyond that, so a transfer spans many turns and exercises the
// sender's backpressure handling.
struct Link {
    static constexpr int kWindow = 4;

    std::unique_ptr<StorageService> requester;
    std::unique_ptr<StorageService> offerer;
    int                             toOfferer   = 0;
    int                             toRequester = 0;
    // What the offerer lets the requester do; everything unless a test narrows it.
    PermissionSet granted{perm::StorageObserve, perm::StorageRead, perm::StorageUpload};
    // Lets a test damage messages on the way, as a faulty network might.
    std::function<void(std::vector<uint8_t>&)> tamper;
    // Lets a test cut the line: messages to the requester are accepted and lost.
    bool cutToRequester = false;

    Link() {
        requester = std::make_unique<StorageService>(
            [this](const uconnect::DevId&, std::span<const uint8_t> bytes) {
                return deliver(offerer.get(), kRequester, bytes, toOfferer, tamper);
            },
            [](const uconnect::DevId&, const QString&) { return false; });
        offerer = std::make_unique<StorageService>(
            [this](const uconnect::DevId&, std::span<const uint8_t> bytes) {
                if (cutToRequester) return true;
                return deliver(requester.get(), kOfferer, bytes, toRequester, tamper);
            },
            [this](const uconnect::DevId& dev, const QString& permission) {
                return dev == kRequester && granted.contains(permission);
            });
    }
    Link(const Link&) = delete;

    static bool deliver(StorageService* to, uconnect::DevId from, std::span<const uint8_t> bytes, int& inFlight,
                        const std::function<void(std::vector<uint8_t>&)>& tamper) {
        if (inFlight >= kWindow) return false;
        ++inFlight;
        std::vector<uint8_t> copy(bytes.begin(), bytes.end());
        if (tamper) tamper(copy);
        QMetaObject::invokeMethod(to, [to, from, copy, &inFlight] {
            --inFlight;
            to->handleMessage(from, copy);
        }, Qt::QueuedConnection);
        return true;
    }
};

struct ListResult {
    QVariantList entries;
    QString      error;
};
struct DownloadResult {
    QString localPath;
    QString error;
};

quint32 nextId() {
    static quint32 id = 1;
    return id++;
}

QStringList names(const QVariantList& entries) {
    QStringList out;
    for (const QVariant& e : entries) out << e.toMap().value("name").toString();
    return out;
}

}  // namespace

class StorageServiceTest : public QObject {
    Q_OBJECT

    QTemporaryDir         dir_;
    QString               share_;
    QString               outside_;
    QString               downloads_;
    QByteArray            big_;
    bool                  haveEscapeLink_ = false;
    std::unique_ptr<Link> link_;

    ListResult list(const QString& path) {
        const quint32 id = nextId();
        QSignalSpy    spy(link_->requester.get(), &StorageService::listFinished);
        link_->requester->list(kOfferer, path, id);
        while (spy.wait(5000)) {
            for (const auto& args : spy)
                if (args.at(0).toUInt() == id) return {args.at(2).toList(), args.at(3).toString()};
        }
        return {{}, QStringLiteral("timed out")};
    }

    DownloadResult download(const QString& path) {
        const quint32 id = nextId();
        QSignalSpy    spy(link_->requester.get(), &StorageService::downloadFinished);
        link_->requester->download(kOfferer, path, downloads_, id);
        while (spy.wait(10000)) {
            for (const auto& args : spy)
                if (args.at(0).toUInt() == id) return {args.at(1).toString(), args.at(2).toString()};
        }
        return {{}, QStringLiteral("timed out")};
    }

    DownloadResult upload(const QString& localPath, const QString& folder = QStringLiteral("/share/sub"),
                          bool replace = false) {
        const quint32 id = nextId();
        QSignalSpy spy(link_->requester.get(), &StorageService::uploadFinished);
        link_->requester->upload(kOfferer, localPath, folder, id, replace);
        if (spy.isEmpty()) spy.wait(10000);
        for (const auto& args : spy)
            if (args.at(0).toUInt() == id) return {args.at(1).toString(), args.at(2).toString()};
        return {{}, QStringLiteral("timed out")};
    }

    // fs.rename, fs.delete or fs.mkdir: the resulting path, or an error.
    DownloadResult op(const QJsonObject& request) {
        const quint32 id = nextId();
        QSignalSpy    spy(link_->requester.get(), &StorageService::operationFinished);
        link_->requester->operation(kOfferer, request, id);
        if (spy.isEmpty()) spy.wait(5000);
        for (const auto& args : spy)
            if (args.at(0).toUInt() == id) return {args.at(1).toString(), args.at(2).toString()};
        return {{}, QStringLiteral("timed out")};
    }

    static bool makeLink(const QString& link, const QString& target) {
#ifdef Q_OS_WIN
        return QProcess::execute("cmd", {"/c", "mklink", "/J", QDir::toNativeSeparators(link),
                                         QDir::toNativeSeparators(target)}) == 0;
#else
        return QFile::link(target, link);
#endif
    }

    QString uploadedPath(const QString& remotePath) const {
        return share_ + remotePath.mid(QStringLiteral("/share").size());
    }

    static std::vector<uint8_t> uploadFrame(quint32 id, const QByteArray& bytes) {
        std::vector<uint8_t> frame{3, uint8_t(id >> 24), uint8_t(id >> 16), uint8_t(id >> 8), uint8_t(id)};
        frame.insert(frame.end(), bytes.begin(), bytes.end());
        return frame;
    }

    static QByteArray contents(const QString& path) {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }

    static void write(const QString& path, const QByteArray& data) {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(data);
    }

private slots:
    void initTestCase() {
        QVERIFY(dir_.isValid());
        const QDir root(dir_.path());
        share_     = root.filePath("share");
        outside_   = root.filePath("outside");
        downloads_ = root.filePath("downloads");
        QVERIFY(QDir().mkpath(share_ + "/sub"));
        QVERIFY(QDir().mkpath(share_ + "/many"));
        QVERIFY(QDir().mkpath(outside_));

        write(share_ + "/hello.txt", "hello world");
        write(share_ + "/sub/nested.txt", "nested");
        for (int i = 0; i < 1200; ++i) write(share_ + QStringLiteral("/many/f%1.txt").arg(i, 4, 10, QLatin1Char('0')), "x");
        big_.resize(3 * 1024 * 1024 + 123);
        QRandomGenerator(42).fillRange(reinterpret_cast<quint32*>(big_.data()), big_.size() / 4);
        write(share_ + "/big.bin", big_);
        write(outside_ + "/secret.txt", "secret");

        // A link inside the share that leads out of it.
        haveEscapeLink_ = makeLink(share_ + "/escape", outside_);
    }

    void init() {
        link_ = std::make_unique<Link>();
        link_->offerer->setOffered(true);
        link_->offerer->setSharedFolders({share_});
        link_->offerer->setUseTrash(false);  // keep deletes out of the user's Recycle Bin
    }

    void renamesOnlyWithWriteAndWithinItsFolder() {
        write(share_ + "/sub/rename-me.txt", "r");
        const QJsonObject rename{{"t", "fs.rename"}, {"path", "/share/sub/rename-me.txt"}, {"name", "renamed.txt"}};
        QVERIFY(!op(rename).error.isEmpty());  // no storage.write yet
        QVERIFY(QFile::exists(share_ + "/sub/rename-me.txt"));

        link_->granted.insert(perm::StorageWrite);
        const auto r = op(rename);
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        QCOMPARE(r.localPath, QStringLiteral("/share/sub/renamed.txt"));
        QVERIFY(QFile::exists(share_ + "/sub/renamed.txt"));
        QVERIFY(!QFile::exists(share_ + "/sub/rename-me.txt"));

        // Names that would leave the folder, are not plain, or clash are refused.
        for (const QString& name : {QStringLiteral("../escaped.txt"), QStringLiteral("a/b.txt"), QStringLiteral(".."),
                                    QStringLiteral("CON"), QStringLiteral("nested.txt")}) {
            const auto bad = op({{"t", "fs.rename"}, {"path", "/share/sub/renamed.txt"}, {"name", name}});
            QVERIFY2(!bad.error.isEmpty(), qPrintable(name));
        }
        QVERIFY(QFile::exists(share_ + "/sub/renamed.txt"));
        QCOMPARE(contents(share_ + "/sub/nested.txt"), QByteArray("nested"));
        QVERIFY(!QFile::exists(share_ + "/escaped.txt"));

        // A shared folder itself is not renamed.
        QVERIFY(!op({{"t", "fs.rename"}, {"path", "/share"}, {"name", "other"}}).error.isEmpty());
        QVERIFY(QFileInfo(share_).isDir());
        QVERIFY(QFile::remove(share_ + "/sub/renamed.txt"));
    }

    void deletesFilesAndFoldersOnlyWithWrite() {
        QVERIFY(QDir().mkpath(share_ + "/doomed/inner"));
        write(share_ + "/doomed/inner/a.txt", "a");
        write(share_ + "/doomed/b.txt", "b");
        const QJsonObject doomed{{"t", "fs.delete"}, {"path", "/share/doomed"}};
        QVERIFY(!op(doomed).error.isEmpty());  // no storage.write yet
        QVERIFY(QFileInfo(share_ + "/doomed").isDir());

        link_->granted.insert(perm::StorageWrite);
        QVERIFY(op({{"t", "fs.delete"}, {"path", "/share/doomed/b.txt"}}).error.isEmpty());
        QVERIFY(!QFile::exists(share_ + "/doomed/b.txt"));
        const auto folder = op(doomed);
        QVERIFY2(folder.error.isEmpty(), qPrintable(folder.error));
        QVERIFY(!QFileInfo::exists(share_ + "/doomed"));

        // Not the shared folder itself, nothing outside it, and nothing that is not there.
        for (const QString& path : {QStringLiteral("/share"), QStringLiteral("/share/../outside/secret.txt"),
                                    QStringLiteral("/share/missing.txt"), QStringLiteral("/unknown/x")}) {
            QVERIFY2(!op({{"t", "fs.delete"}, {"path", path}}).error.isEmpty(), qPrintable(path));
        }
        QVERIFY(QFileInfo(share_).isDir());
        QCOMPARE(contents(outside_ + "/secret.txt"), QByteArray("secret"));
    }

    // A link that leads out of the share: deleting through it is refused, and
    // deleting it (or a folder holding it) removes the link, not its target.
    void deletesLinksNotWhatTheyPointTo() {
        if (!haveEscapeLink_) QSKIP("could not create a link on this system");
        link_->granted.insert(perm::StorageWrite);
        QVERIFY(!op({{"t", "fs.delete"}, {"path", "/share/escape/secret.txt"}}).error.isEmpty());
        QCOMPARE(contents(outside_ + "/secret.txt"), QByteArray("secret"));

        QVERIFY(QDir().mkpath(share_ + "/holder"));
        QVERIFY(makeLink(share_ + "/holder/link", outside_));
        const auto holder = op({{"t", "fs.delete"}, {"path", "/share/holder"}});
        QVERIFY2(holder.error.isEmpty(), qPrintable(holder.error));
        QVERIFY(!QFileInfo::exists(share_ + "/holder"));
        QCOMPARE(contents(outside_ + "/secret.txt"), QByteArray("secret"));

        const auto link = op({{"t", "fs.delete"}, {"path", "/share/escape"}});
        QVERIFY2(link.error.isEmpty(), qPrintable(link.error));
        QVERIFY(!QFileInfo(share_ + "/escape").exists() && !QFileInfo(share_ + "/escape").isSymLink());
        QCOMPARE(contents(outside_ + "/secret.txt"), QByteArray("secret"));
        haveEscapeLink_ = makeLink(share_ + "/escape", outside_);  // for the tests after this one
    }

    void createsFoldersWithUploadPermission() {
        const QJsonObject mkdir{{"t", "fs.mkdir"}, {"path", "/share/sub"}, {"name", "New folder"}};
        link_->granted = {perm::StorageObserve};
        QVERIFY(!op(mkdir).error.isEmpty());

        link_->granted.insert(perm::StorageUpload);
        const auto first  = op(mkdir);
        const auto second = op(mkdir);
        QVERIFY2(first.error.isEmpty() && second.error.isEmpty(), qPrintable(first.error + second.error));
        QCOMPARE(first.localPath, QStringLiteral("/share/sub/New folder"));
        QCOMPARE(second.localPath, QStringLiteral("/share/sub/New folder (1)"));
        QVERIFY(QFileInfo(share_ + "/sub/New folder").isDir());

        // A folder upload merging: the existing folder is answered, not a new one.
        QJsonObject merge = mkdir;
        merge.insert("existing", true);
        QCOMPARE(op(merge).localPath, QStringLiteral("/share/sub/New folder"));
        QVERIFY(!QFileInfo::exists(share_ + "/sub/New folder (2)"));
        // ...but never a link out of the share; that gets a folder of its own.
        if (haveEscapeLink_) {
            const auto out = op({{"t", "fs.mkdir"}, {"path", "/share"}, {"name", "escape"}, {"existing", true}});
            QCOMPARE(out.localPath, QStringLiteral("/share/escape (1)"));
            QVERIFY(QDir().rmdir(share_ + "/escape (1)"));
        }

        QVERIFY(!op({{"t", "fs.mkdir"}, {"path", "/share/sub"}, {"name", "../x"}}).error.isEmpty());
        QVERIFY(!op({{"t", "fs.mkdir"}, {"path", "/share/../outside"}, {"name", "x"}}).error.isEmpty());
        QVERIFY(!QFileInfo::exists(outside_ + "/x"));
        QVERIFY(QDir().rmdir(share_ + "/sub/New folder"));
        QVERIFY(QDir().rmdir(share_ + "/sub/New folder (1)"));
    }

    void movesAndCopiesWithinTheShares() {
        QVERIFY(QDir().mkpath(share_ + "/tree/inner"));
        QVERIFY(QDir().mkpath(share_ + "/dest"));
        write(share_ + "/tree/a.txt", "a");
        write(share_ + "/tree/inner/b.txt", "b");
        const bool innerLink = makeLink(share_ + "/tree/out", outside_);
        auto move = [](const QString& path, const QString& to) {
            return QJsonObject{{"t", "fs.move"}, {"path", path}, {"to", to}};
        };
        auto copy = [](const QString& path, const QString& to) {
            return QJsonObject{{"t", "fs.copy"}, {"path", path}, {"to", to}};
        };

        // Moving needs write; copying needs upload.
        QVERIFY(!op(move("/share/tree/a.txt", "/share/dest")).error.isEmpty());
        link_->granted.remove(perm::StorageUpload);
        QVERIFY(!op(copy("/share/tree/a.txt", "/share/dest")).error.isEmpty());
        QVERIFY(!QFile::exists(share_ + "/dest/a.txt"));
        link_->granted.insert(perm::StorageUpload);
        link_->granted.insert(perm::StorageWrite);

        const auto moved = op(move("/share/tree/a.txt", "/share/dest"));
        QVERIFY2(moved.error.isEmpty(), qPrintable(moved.error));
        QCOMPARE(moved.localPath, QStringLiteral("/share/dest/a.txt"));
        QVERIFY(QFile::exists(share_ + "/dest/a.txt") && !QFile::exists(share_ + "/tree/a.txt"));

        // A copy of a folder: everything but the link inside it.
        const auto copied = op(copy("/share/tree", "/share/dest"));
        QVERIFY2(copied.error.isEmpty(), qPrintable(copied.error));
        QCOMPARE(copied.localPath, QStringLiteral("/share/dest/tree"));
        QCOMPARE(contents(share_ + "/dest/tree/inner/b.txt"), QByteArray("b"));
        QVERIFY(QFile::exists(share_ + "/tree/inner/b.txt"));
        if (innerLink) {
            QVERIFY(!QFileInfo(share_ + "/dest/tree/out").exists());
            QVERIFY(!QFileInfo(share_ + "/dest/tree/out").isSymLink());
        }
        QCOMPARE(op(copy("/share/tree", "/share/dest")).localPath, QStringLiteral("/share/dest/tree (1)"));

        // Between drives a move is a copy and a delete.
        link_->offerer->setAlwaysCopyMoves(true);
        const auto across = op(move("/share/dest/tree (1)", "/share/sub"));
        QVERIFY2(across.error.isEmpty(), qPrintable(across.error));
        QCOMPARE(across.localPath, QStringLiteral("/share/sub/tree (1)"));
        QVERIFY(!QFileInfo::exists(share_ + "/dest/tree (1)"));
        QCOMPARE(contents(share_ + "/sub/tree (1)/inner/b.txt"), QByteArray("b"));
        link_->offerer->setAlwaysCopyMoves(false);

        // Into itself, out of the share, the share itself, or a link's target.
        QVERIFY(!op(move("/share/tree", "/share/tree/inner")).error.isEmpty());
        QVERIFY(!op(copy("/share/tree", "/share/tree/inner")).error.isEmpty());
        QVERIFY(!QFileInfo::exists(share_ + "/tree/inner/tree"));
        QVERIFY(!op(move("/share", "/share/dest")).error.isEmpty());
        QVERIFY(!op(move("/share/dest/a.txt", "/share/../outside")).error.isEmpty());
        if (haveEscapeLink_) {
            QVERIFY(!op(move("/share/dest/a.txt", "/share/escape")).error.isEmpty());
            QVERIFY(!op(copy("/share/escape/secret.txt", "/share/dest")).error.isEmpty());
            QVERIFY(!op(copy("/share/escape", "/share/dest")).error.isEmpty());
            QVERIFY(!QFileInfo::exists(share_ + "/dest/secret.txt") && !QFileInfo::exists(share_ + "/dest/escape"));
            QVERIFY(QFile::exists(outside_ + "/secret.txt"));
        }

        if (innerLink) QVERIFY(QDir().rmdir(share_ + "/tree/out"));
        QVERIFY(QDir(share_ + "/tree").removeRecursively());
        QVERIFY(QDir(share_ + "/dest").removeRecursively());
        QVERIFY(QDir(share_ + "/sub/tree (1)").removeRecursively());
    }

    void reportsChangesToWatchedFolders() {
        QVERIFY(QDir().mkpath(share_ + "/watched"));
        QSignalSpy changed(link_->requester.get(), &StorageService::folderChanged);
        const quint32 id = nextId();
        link_->requester->watch(kOfferer, "/share/watched", id);
        QTest::qWait(2500);
        QVERIFY(changed.isEmpty());  // nothing changed yet

        write(share_ + "/watched/new.txt", "n");
        QTRY_VERIFY_WITH_TIMEOUT(!changed.isEmpty(), 5000);
        QCOMPARE(changed.first().at(0).toUInt(), id);
        QCOMPARE(changed.first().at(1).toString(), QStringLiteral("/share/watched"));

        // Without observe permission the watch ends.
        changed.clear();
        link_->granted.remove(perm::StorageObserve);
        QTest::qWait(2500);
        link_->granted.insert(perm::StorageObserve);
        write(share_ + "/watched/later.txt", "l");
        QTest::qWait(2500);
        QVERIFY(changed.isEmpty());

        // Unwatched: no more reports.
        const quint32 again = nextId();
        link_->requester->watch(kOfferer, "/share/watched", again);
        QTest::qWait(100);
        link_->requester->unwatch(again);
        write(share_ + "/watched/after.txt", "a");
        QTest::qWait(2500);
        QVERIFY(changed.isEmpty());

        // Outside the share, or through a link out of it: never watched.
        if (haveEscapeLink_) {
            link_->requester->watch(kOfferer, "/share/escape", nextId());
            write(outside_ + "/poke.txt", "p");
            QTest::qWait(2500);
            QVERIFY(changed.isEmpty());
            QFile::remove(outside_ + "/poke.txt");
        }
        QVERIFY(QDir(share_ + "/watched").removeRecursively());
    }

    // A file damaged on the way is caught by its SHA-256 and not kept.
    void discardsFilesDamagedInTransit() {
        auto flipFirstDataByte = [](uint8_t kind) {
            return [kind, done = false](std::vector<uint8_t>& m) mutable {
                if (done || m.size() < 6 || m[0] != kind) return;
                m[5] ^= 0xFF;
                done = true;
            };
        };
        link_->tamper = flipFirstDataByte(2);  // a download frame
        const auto down = download("/share/hello.txt");
        QVERIFY(down.localPath.isEmpty());
        QVERIFY2(down.error.contains("damaged"), qPrintable(down.error));
        QVERIFY(!QFile::exists(downloads_ + "/hello.txt"));
        QVERIFY(QDir(downloads_).entryList({"*.part"}, QDir::Files).isEmpty());

        write(outside_ + "/fragile.txt", "fragile contents");
        link_->tamper = flipFirstDataByte(3);  // an upload frame
        const auto up = upload(outside_ + "/fragile.txt");
        QVERIFY(up.localPath.isEmpty());
        QVERIFY2(up.error.contains("damaged"), qPrintable(up.error));
        QVERIFY(!QFile::exists(share_ + "/sub/fragile.txt"));
        QVERIFY(QDir(share_ + "/sub").entryList({".uconnect-*"}, QDir::Files | QDir::Hidden).isEmpty());

        // Intact, both still work.
        link_->tamper = nullptr;
        QVERIFY(download("/share/hello.txt").error.isEmpty());
        const auto ok = upload(outside_ + "/fragile.txt");
        QVERIFY2(ok.error.isEmpty(), qPrintable(ok.error));
        QVERIFY(QFile::remove(uploadedPath(ok.localPath)));
        QFile::remove(downloads_ + "/hello.txt");
    }

    // A download cut off by a lost connection resumes from what arrived.
    void resumesDownloadsAfterTheConnectionDrops() {
        const QString part = downloads_ + "/big.bin." +
            QString::fromLatin1(QCryptographicHash::hash("/share/big.bin", QCryptographicHash::Sha256).toHex().left(8)) +
            ".part";
        QFile::remove(part);
        qint64 sentData = 0;
        int    frames   = 0;
        link_->tamper = [&](std::vector<uint8_t>& m) {
            if (m.empty() || m[0] != 2) return;
            sentData += static_cast<qint64>(m.size()) - 5;
            if (++frames == 5) link_->cutToRequester = true;  // this frame arrives; nothing after it
        };

        // The line goes dead part-way through, then the device is gone.
        const quint32 id = nextId();
        QSignalSpy progress(link_->requester.get(), &StorageService::downloadProgress);
        QSignalSpy finished(link_->requester.get(), &StorageService::downloadFinished);
        link_->requester->download(kOfferer, "/share/big.bin", downloads_, id);
        QTRY_VERIFY_WITH_TIMEOUT(link_->cutToRequester, 10000);
        QTest::qWait(100);  // let what got through arrive
        link_->requester->peerGone(kOfferer);
        link_->offerer->peerGone(kRequester);
        link_->cutToRequester = false;
        QCOMPARE(finished.size(), 1);
        QVERIFY(!finished.first().at(2).toString().isEmpty());
        const qint64 kept = QFileInfo(part).size();
        QVERIFY2(kept > 1024 * 1024 && kept < big_.size(), qPrintable(QString::number(kept)));
        QTest::qWait(50);  // let frames already on the way arrive (and be ignored)

        // The same download again picks up where it stopped.
        sentData = 0;
        const auto again = download("/share/big.bin");
        QVERIFY2(again.error.isEmpty(), qPrintable(again.error));
        QCOMPARE(contents(again.localPath), big_);
        QCOMPARE(sentData, big_.size() - kept);
        QVERIFY(!QFile::exists(part));
        QVERIFY(QFile::remove(again.localPath));

        // A part that does not match the file is caught, discarded, and the
        // next attempt starts over.
        write(part, QByteArray(1000, 'z'));
        const auto bad = download("/share/big.bin");
        QVERIFY2(bad.error.contains("damaged"), qPrintable(bad.error));
        QVERIFY(!QFile::exists(part));
        sentData = 0;
        const auto fresh = download("/share/big.bin");
        QVERIFY2(fresh.error.isEmpty(), qPrintable(fresh.error));
        QCOMPARE(contents(fresh.localPath), big_);
        QCOMPARE(sentData, big_.size());
        QVERIFY(QFile::remove(fresh.localPath));

        // Canceling does not keep a part.
        link_->tamper = nullptr;
        const quint32 canceled = nextId();
        progress.clear();
        link_->requester->download(kOfferer, "/share/big.bin", downloads_, canceled);
        QTRY_VERIFY_WITH_TIMEOUT(!progress.isEmpty() && progress.last().at(1).toLongLong() > 0, 10000);
        link_->requester->cancel(canceled);
        QVERIFY(!QFile::exists(part));
    }

    void readOnlySharesRefuseEveryChange() {
        QVERIFY(QDir().mkpath(share_ + "/sub/keep"));
        link_->granted.insert(perm::StorageWrite);
        link_->offerer->setSharedFolders({share_}, {share_});
        QVariantList root;
        {
            QSignalSpy spy(link_->requester.get(), &StorageService::listFinished);
            const quint32 id = nextId();
            link_->requester->list(kOfferer, "/share/sub", id);
            QVERIFY(spy.wait(5000));
            QCOMPARE(spy.first().at(4).toBool(), true);
            link_->requester->list(kOfferer, "/", nextId());
            QVERIFY(spy.wait(5000));
            root = spy.last().at(2).toList();
        }
        QCOMPARE(root.first().toMap().value("readOnly").toBool(), true);

        write(outside_ + "/ro.txt", "ro");
        QVERIFY(upload(outside_ + "/ro.txt").error.contains("read-only"));
        auto refused = [&](const QJsonObject& request) { return op(request).error.contains("read-only"); };
        QVERIFY(refused({{"t", "fs.mkdir"}, {"path", "/share/sub"}, {"name", "x"}}));
        QVERIFY(refused({{"t", "fs.rename"}, {"path", "/share/sub/keep"}, {"name", "y"}}));
        QVERIFY(refused({{"t", "fs.delete"}, {"path", "/share/sub/keep"}}));
        QVERIFY(refused({{"t", "fs.move"}, {"path", "/share/sub/keep"}, {"to", "/share"}}));
        QVERIFY(refused({{"t", "fs.copy"}, {"path", "/share/hello.txt"}, {"to", "/share/sub"}}));
        QVERIFY(QFileInfo(share_ + "/sub/keep").isDir());
        QVERIFY(!QFileInfo::exists(share_ + "/sub/x") && !QFileInfo::exists(share_ + "/sub/hello.txt"));
        QVERIFY(download("/share/hello.txt").error.isEmpty());  // reading is unaffected
        QFile::remove(downloads_ + "/hello.txt");
        QVERIFY(QDir().rmdir(share_ + "/sub/keep"));
    }

    // The offerer reports what was done in its folders, and by whom.
    void reportsActivityInTheShares() {
        link_->granted.insert(perm::StorageWrite);
        QSignalSpy activity(link_->offerer.get(), &StorageService::activity);
        auto last = [&] {
            const auto args = activity.last();
            return QStringList{args.at(1).toString(), args.at(2).toString(), args.at(3).toString()};
        };
        QVERIFY(download("/share/hello.txt").error.isEmpty());
        QCOMPARE(last(), (QStringList{"download", "/share/hello.txt", ""}));
        QCOMPARE(activity.last().at(0).value<uconnect::DevId>(), kRequester);
        QFile::remove(downloads_ + "/hello.txt");

        QCOMPARE(op({{"t", "fs.mkdir"}, {"path", "/share/sub"}, {"name", "act"}}).error, QString());
        QCOMPARE(last(), (QStringList{"mkdir", "/share/sub/act", ""}));
        write(outside_ + "/act.txt", "a");
        QVERIFY(upload(outside_ + "/act.txt", "/share/sub/act").error.isEmpty());
        QCOMPARE(last(), (QStringList{"upload", "/share/sub/act/act.txt", ""}));
        QVERIFY(op({{"t", "fs.rename"}, {"path", "/share/sub/act/act.txt"}, {"name", "b.txt"}}).error.isEmpty());
        QCOMPARE(last(), (QStringList{"rename", "/share/sub/act/act.txt", "b.txt"}));
        QVERIFY(op({{"t", "fs.copy"}, {"path", "/share/sub/act/b.txt"}, {"to", "/share/sub"}}).error.isEmpty());
        QCOMPARE(last(), (QStringList{"copy", "/share/sub/act/b.txt", "/share/sub/b.txt"}));
        QVERIFY(op({{"t", "fs.move"}, {"path", "/share/sub/b.txt"}, {"to", "/share"}}).error.isEmpty());
        QCOMPARE(last(), (QStringList{"move", "/share/sub/b.txt", "/share/b.txt"}));
        QVERIFY(op({{"t", "fs.delete"}, {"path", "/share/b.txt"}}).error.isEmpty());
        QCOMPARE(last(), (QStringList{"delete", "/share/b.txt", ""}));

        const qsizetype before = activity.size();
        QVERIFY(!op({{"t", "fs.delete"}, {"path", "/share/missing.txt"}}).error.isEmpty());
        QCOMPARE(activity.size(), before);  // only what happened
        QVERIFY(QDir(share_ + "/sub/act").removeRecursively());
    }

    void refusesUploadsOverTheLimitOrTheFreeSpace() {
        write(outside_ + "/eleven.txt", "eleven byte");
        link_->offerer->setUploadLimit(10);
        const auto big = upload(outside_ + "/eleven.txt");
        QVERIFY2(big.error.contains("larger"), qPrintable(big.error));
        link_->offerer->setUploadLimit(11);
        const auto fits = upload(outside_ + "/eleven.txt");
        QVERIFY2(fits.error.isEmpty(), qPrintable(fits.error));
        QVERIFY(QFile::remove(uploadedPath(fits.localPath)));

        // As if the disk were nearly full.
        link_->offerer->setMinimumFreeSpace(std::numeric_limits<qint64>::max() / 2);
        const auto full = upload(outside_ + "/eleven.txt");
        QVERIFY2(full.error.contains("space"), qPrintable(full.error));
        const auto copy = op({{"t", "fs.copy"}, {"path", "/share/hello.txt"}, {"to", "/share/sub"}});
        QVERIFY2(copy.error.contains("space"), qPrintable(copy.error));
        QVERIFY(!QFile::exists(share_ + "/sub/hello.txt"));
        QVERIFY(QDir(share_ + "/sub").entryList({"*.part"}, QDir::Files | QDir::Hidden).isEmpty());
    }

    void replacesExistingFilesOnlyWithWrite() {
        write(share_ + "/sub/target.txt", "old");
        write(outside_ + "/target.txt", "new contents");
        QVERIFY(!upload(outside_ + "/target.txt", "/share/sub", true).error.isEmpty());  // upload only
        QCOMPARE(contents(share_ + "/sub/target.txt"), QByteArray("old"));

        link_->granted.insert(perm::StorageWrite);
        const auto r = upload(outside_ + "/target.txt", "/share/sub", true);
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        QCOMPARE(r.localPath, QStringLiteral("/share/sub/target.txt"));
        QCOMPARE(contents(share_ + "/sub/target.txt"), QByteArray("new contents"));
        QVERIFY(!QFile::exists(share_ + "/sub/target (1).txt"));

        // Never replaces a folder.
        QVERIFY(QDir().mkpath(share_ + "/sub/target-dir"));
        write(outside_ + "/target-dir", "x");
        QVERIFY(!upload(outside_ + "/target-dir", "/share/sub", true).error.isEmpty());
        QVERIFY(QFileInfo(share_ + "/sub/target-dir").isDir());

        QTest::qWait(50);
        QVERIFY(QDir(share_ + "/sub").entryList({"*.part"}, QDir::Files | QDir::Hidden).isEmpty());
        QVERIFY(QFile::remove(share_ + "/sub/target.txt"));
        QVERIFY(QDir().rmdir(share_ + "/sub/target-dir"));
    }

    void listsSharedFoldersByNameOnly() {
        const auto r = list("/");
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        QCOMPARE(names(r.entries), QStringList{"share"});
        QVERIFY(r.entries.first().toMap().value("dir").toBool());
    }

    void listsFolders() {
        const auto r = list("/share");
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        const QStringList n = names(r.entries);
        QVERIFY(n.contains("hello.txt"));
        QVERIFY(n.contains("sub"));
        QVERIFY(n.indexOf("sub") < n.indexOf("hello.txt"));  // folders first

        const auto hello = r.entries.at(n.indexOf("hello.txt")).toMap();
        QCOMPARE(hello.value("size").toLongLong(), 11);
        QVERIFY(!hello.value("dir").toBool());

        QCOMPARE(names(list("/share/sub").entries), QStringList{"nested.txt"});
    }

    void pagesLargeFolders() {
        const auto r = list("/share/many");
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        QCOMPARE(r.entries.size(), 1200);
        QCOMPARE(names(r.entries).first(), QStringLiteral("f0000.txt"));
        QCOMPARE(names(r.entries).last(), QStringLiteral("f1199.txt"));
    }

    void rejectsEscapes_data() {
        QTest::addColumn<QString>("path");
        QTest::newRow("dot-dot") << "/share/../outside/secret.txt";
        QTest::newRow("dot-dot nested") << "/share/sub/../../outside/secret.txt";
        QTest::newRow("dot-dot at root") << "/..";
        QTest::newRow("backslashes") << "/share/..\\outside\\secret.txt";
        QTest::newRow("relative") << "share/hello.txt";
        QTest::newRow("real path") << QDir(outside_).absoluteFilePath("secret.txt");
        QTest::newRow("unknown folder") << "/outside/secret.txt";
        QTest::newRow("alternate data stream") << "/share/hello.txt:secret";
        QTest::newRow("device name") << "/share/CON";
        QTest::newRow("nul byte") << QStringLiteral("/share/hello.txt") + QChar(0) + "x";
    }

    void rejectsEscapes() {
        QFETCH(QString, path);
        const auto d = download(path);
        QVERIFY2(!d.error.isEmpty(), "download should have been refused");
        QVERIFY(d.localPath.isEmpty());
        QVERIFY(!list(path).error.isEmpty());
        QVERIFY(!QFileInfo::exists(QDir(downloads_).filePath("secret.txt")));
    }

    void hidesLinksLeadingOutside() {
        if (!haveEscapeLink_) QSKIP("could not create a link on this system");
        const auto read  = download("/share/escape/secret.txt");
        const auto inner = list("/share/escape");
        const auto outer = names(list("/share").entries);
        QVERIFY2(!read.error.isEmpty(), "a file outside the share was readable through the link");
        QVERIFY2(!inner.error.isEmpty(), "the link's target was listable");
        QVERIFY2(!outer.contains("escape"), "the link was listed");
    }

    void enforcesPermissions() {
        link_->granted = {};
        QVERIFY(!list("/").error.isEmpty());
        QVERIFY(!download("/share/hello.txt").error.isEmpty());

        // Observe lets it browse, but reading files needs its own permission.
        link_->granted = {perm::StorageObserve};
        QVERIFY(list("/share").error.isEmpty());
        QVERIFY(!download("/share/hello.txt").error.isEmpty());

        // And the reverse: reading a known path without browsing rights.
        link_->granted = {perm::StorageRead};
        QVERIFY(!list("/share").error.isEmpty());
        const auto d = download("/share/hello.txt");
        QVERIFY2(d.error.isEmpty(), qPrintable(d.error));
    }

    void refusesWhenNotOffered() {
        link_->offerer->setOffered(false);
        QVERIFY(!list("/").error.isEmpty());
        QVERIFY(!download("/share/hello.txt").error.isEmpty());
    }

    void downloadsExactBytes() {
        const auto big = download("/share/big.bin");
        QVERIFY2(big.error.isEmpty(), qPrintable(big.error));
        QCOMPARE(contents(big.localPath), big_);

        const auto first  = download("/share/hello.txt");
        const auto second = download("/share/hello.txt");
        QVERIFY2(first.error.isEmpty() && second.error.isEmpty(), qPrintable(first.error + second.error));
        QVERIFY(first.localPath != second.localPath);  // never overwrites
        QCOMPARE(contents(first.localPath), QByteArray("hello world"));
        QCOMPARE(contents(second.localPath), QByteArray("hello world"));
    }

    // The offerer serves four downloads per peer at a time and refuses more as
    // "busy"; the requester queues the rest instead of failing them.
    void queuesDownloadsBeyondTheOfferersLimit() {
        QSignalSpy finished(link_->requester.get(), &StorageService::downloadFinished);
        QSet<quint32> ids;
        for (int i = 0; i < 10; ++i) {
            const quint32 id = nextId();
            ids.insert(id);
            link_->requester->download(kOfferer, i % 2 ? "/share/hello.txt" : "/share/big.bin", downloads_, id);
        }
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() == 10, 30000);
        for (const auto& args : finished) {
            QVERIFY(ids.contains(args.at(0).toUInt()));
            QVERIFY2(args.at(2).toString().isEmpty(), qPrintable(args.at(2).toString()));
            QVERIFY(QFile::remove(args.at(1).toString()));
        }
    }

    void cancelsMidTransfer() {
        const quint32 id = nextId();
        QSignalSpy    progress(link_->requester.get(), &StorageService::downloadProgress);
        QSignalSpy    finished(link_->requester.get(), &StorageService::downloadFinished);
        link_->requester->download(kOfferer, "/share/big.bin", downloads_, id);
        QVERIFY(progress.wait(5000));
        link_->requester->cancel(id);
        QVERIFY(finished.count() > 0 || finished.wait(5000));
        QVERIFY(finished.last().at(1).toString().isEmpty());
        QVERIFY(!finished.last().at(2).toString().isEmpty());
        QTest::qWait(200);  // let stray frames arrive; they must be ignored
        QVERIFY(QDir(downloads_).entryList({"*.part"}, QDir::Files).isEmpty());
    }

    void uploadsExactBytesAndRefreshesListing() {
        const auto before = list("/share/sub");  // populates the listing cache
        const auto r = upload(share_ + "/big.bin");
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        QCOMPARE(contents(uploadedPath(r.localPath)), big_);
        QVERIFY(names(list("/share/sub").entries).contains(QFileInfo(r.localPath).fileName()));
        QCOMPARE(names(before.entries).size() + 1, names(list("/share/sub").entries).size());
        QVERIFY(QFile::remove(uploadedPath(r.localPath)));
    }

    void uploadsEmptyFilesAndPreservesExistingFiles() {
        const QString source = outside_ + "/empty.txt";
        write(source, {});
        const auto empty = upload(source);
        QVERIFY2(empty.error.isEmpty(), qPrintable(empty.error));
        QVERIFY(QFileInfo::exists(uploadedPath(empty.localPath)));
        QCOMPARE(QFileInfo(uploadedPath(empty.localPath)).size(), 0);
        const auto first = upload(share_ + "/hello.txt", "/share");
        const auto second = upload(share_ + "/hello.txt", "/share");
        QVERIFY(first.error.isEmpty() && second.error.isEmpty());
        QVERIFY(first.localPath != second.localPath);
        QVERIFY(first.localPath != "/share/hello.txt");
        QCOMPARE(contents(share_ + "/hello.txt"), QByteArray("hello world"));
        QVERIFY(QFile::remove(uploadedPath(first.localPath)));
        QVERIFY(QFile::remove(uploadedPath(second.localPath)));
        QVERIFY(QFile::remove(uploadedPath(empty.localPath)));
    }

    void enforcesUploadPermissionsAndSharedFolders() {
        link_->granted = {perm::StorageObserve, perm::StorageRead, perm::StorageWrite};
        QVERIFY(!upload(share_ + "/hello.txt").error.isEmpty());
        link_->granted.insert(perm::StorageUpload);
        link_->offerer->setOffered(false);
        QVERIFY(!upload(share_ + "/hello.txt").error.isEmpty());
        link_->offerer->setOffered(true);
        for (const QString& folder : {QStringLiteral("/"), QStringLiteral("/share/../outside"),
                                     QStringLiteral("/share/hello.txt"), QStringLiteral("/unknown"),
                                     QStringLiteral("/share/missing")}) {
            QVERIFY2(!upload(share_ + "/hello.txt", folder).error.isEmpty(), qPrintable(folder));
        }
        if (haveEscapeLink_) QVERIFY(!upload(share_ + "/hello.txt", "/share/escape").error.isEmpty());
        QVERIFY(!QFileInfo::exists(outside_ + "/hello.txt"));
        QVERIFY(!upload(share_).error.isEmpty());
        QVERIFY(!upload(outside_ + "/missing.txt").error.isEmpty());
    }

    void cancelsUploadsAndRemovesPartialFiles() {
        const quint32 id = nextId();
        QSignalSpy progress(link_->requester.get(), &StorageService::uploadProgress);
        QSignalSpy finished(link_->requester.get(), &StorageService::uploadFinished);
        link_->requester->upload(kOfferer, share_ + "/big.bin", "/share/sub", id);
        QVERIFY(progress.count() > 0);
        // Wait for receiver acceptance, then cancel before the pump sends data.
        QTRY_VERIFY(!QDir(share_ + "/sub").entryList({"*.part"}, QDir::Files | QDir::Hidden).isEmpty());
        link_->requester->cancel(id);
        QCOMPARE(finished.count(), 1);
        QVERIFY(!finished.first().at(2).toString().isEmpty());
        QTRY_VERIFY2(QDir(share_ + "/sub").entryList({"*.part"}, QDir::Files | QDir::Hidden).isEmpty(),
                     qPrintable(QDir(share_ + "/sub").entryList({"*.part"}, QDir::Files | QDir::Hidden).join(", ")));
        QVERIFY(!QFileInfo::exists(share_ + "/sub/big.bin"));
    }

    void queuesMultipleUploadsUnderBackpressure() {
        QSignalSpy finished(link_->requester.get(), &StorageService::uploadFinished);
        for (int i = 0; i < 9; ++i)
            link_->requester->upload(kOfferer, share_ + "/hello.txt", "/share/sub", nextId());
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 9, 10000);
        QSet<QString> saved;
        for (const auto& args : finished) {
            QVERIFY2(args.at(2).toString().isEmpty(), qPrintable(args.at(2).toString()));
            const QString path = uploadedPath(args.at(1).toString());
            QCOMPARE(contents(path), QByteArray("hello world"));
            saved.insert(path);
        }
        QCOMPARE(saved.size(), 9);
        for (const auto& path : saved) QVERIFY(QFile::remove(path));
    }

    void uploadAndDownloadIdsCanOverlap() {
        QSignalSpy downloaded(link_->offerer.get(), &StorageService::downloadFinished);
        // For this test both peers grant access to each other.
        link_->requester.reset(new StorageService(
            [this](const uconnect::DevId&, std::span<const uint8_t> bytes) {
                return Link::deliver(link_->offerer.get(), kRequester, bytes, link_->toOfferer, link_->tamper);
            }, [](const uconnect::DevId&, const QString&) { return true; }));
        link_->requester->setOffered(true);
        link_->requester->setSharedFolders({share_});
        QSignalSpy uploads(link_->requester.get(), &StorageService::uploadFinished);
        const quint32 id = nextId();
        link_->requester->upload(kOfferer, share_ + "/big.bin", "/share/sub", id);
        link_->offerer->download(kRequester, "/share/big.bin", downloads_, id);
        QTRY_COMPARE_WITH_TIMEOUT(uploads.count(), 1, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(downloaded.count(), 1, 10000);
        QVERIFY(uploads.first().at(2).toString().isEmpty());
        QVERIFY(downloaded.first().at(2).toString().isEmpty());
        QCOMPARE(contents(uploadedPath(uploads.first().at(1).toString())), big_);
        QCOMPARE(contents(downloaded.first().at(1).toString()), big_);
        QVERIFY(QFile::remove(uploadedPath(uploads.first().at(1).toString())));
    }

    void rejectsMalformedUploadsAndCleansUpOnDisconnect() {
        const quint32 id = nextId();
        const auto request = [&](const QString& name, qint64 size) {
            link_->offerer->handleMessage(kRequester, encodeControl({{"t", "fs.write"}, {"id", qint64(id)},
                {"path", "/share/sub"}, {"name", name}, {"size", size}}));
        };
        for (const QString& name : {QStringLiteral("../evil.txt"), QStringLiteral("CON"),
                                   QStringLiteral("file:stream"), QStringLiteral("a\\b"), QStringLiteral("..")}) {
            request(name, 1);
            QVERIFY(QDir(share_ + "/sub").entryList({"*.part"}, QDir::Files | QDir::Hidden).isEmpty());
            QCoreApplication::processEvents();
        }
        request("evil.txt", -1);
        request("short.txt", 2);
        link_->offerer->handleMessage(kRequester, uploadFrame(id, "abc"));
        QVERIFY(!QFileInfo::exists(share_ + "/sub/short.txt"));
        QVERIFY(QDir(share_ + "/sub").entryList({"*.part"}, QDir::Files | QDir::Hidden).isEmpty());
        request("short.txt", 2);
        link_->offerer->handleMessage(kRequester, uploadFrame(id, "a"));
        link_->offerer->handleMessage(kRequester, encodeControl({{"t", "fs.write.end"}, {"id", qint64(id)}, {"sent", 1}}));
        QVERIFY(!QFileInfo::exists(share_ + "/sub/short.txt"));
        request("lost.txt", 3);
        link_->offerer->handleMessage(kRequester, uploadFrame(id, "a"));
        link_->offerer->peerGone(kRequester);
        QVERIFY(QDir(share_ + "/sub").entryList({"*.part"}, QDir::Files | QDir::Hidden).isEmpty());
        QVERIFY(!QFileInfo::exists(share_ + "/sub/lost.txt"));
    }

    void revokingUploadPermissionRemovesPartialFile() {
        const quint32 id = nextId();
        link_->offerer->handleMessage(kRequester, encodeControl({{"t", "fs.write"}, {"id", qint64(id)},
            {"path", "/share/sub"}, {"name", "revoked.txt"}, {"size", 3}}));
        link_->granted.remove(perm::StorageUpload);
        link_->offerer->handleMessage(kRequester, uploadFrame(id, "abc"));
        QVERIFY(!QFileInfo::exists(share_ + "/sub/revoked.txt"));
        QVERIFY(QDir(share_ + "/sub").entryList({"*.part"}, QDir::Files | QDir::Hidden).isEmpty());
    }

    void retriesUploadAcknowledgementsWhenTheQueueIsFull() {
        // Acceptance and completion must survive temporary backpressure too.
        link_->toRequester = Link::kWindow;
        QTimer::singleShot(100, link_->requester.get(), [this] { link_->toRequester = 0; });
        bool blockedCompletion = false;
        connect(link_->requester.get(), &StorageService::uploadProgress, this,
                [this, &blockedCompletion](quint32, qint64 sent, qint64 total) {
                    if (sent != total || blockedCompletion) return;
                    blockedCompletion = true;
                    link_->toRequester = Link::kWindow;
                    QTimer::singleShot(100, link_->requester.get(), [this] { link_->toRequester = 0; });
                });
        const auto r = upload(share_ + "/big.bin");
        QVERIFY2(r.error.isEmpty(), qPrintable(r.error));
        QVERIFY(blockedCompletion);
        QCOMPARE(contents(uploadedPath(r.localPath)), big_);
        QVERIFY(QFile::remove(uploadedPath(r.localPath)));
    }

    void hidesPartialUploadsAndCleansUpChangedShares() {
        const quint32 id = nextId();
        link_->offerer->handleMessage(kRequester, encodeControl({{"t", "fs.write"}, {"id", qint64(id)},
            {"path", "/share/sub"}, {"name", "pending.txt"}, {"size", 3}}));
        QVERIFY(!QDir(share_ + "/sub").entryList({"*.part"}, QDir::Files | QDir::Hidden).isEmpty());
        for (const QString& name : names(list("/share/sub").entries))
            QVERIFY(!name.startsWith(".uconnect-upload-"));
        link_->offerer->setSharedFolders({outside_});
        QVERIFY(QDir(share_ + "/sub").entryList({"*.part"}, QDir::Files | QDir::Hidden).isEmpty());
        link_->offerer->handleMessage(kRequester, uploadFrame(id, "abc"));
        QVERIFY(!QFileInfo::exists(share_ + "/sub/pending.txt"));
        QVERIFY(!QFileInfo::exists(outside_ + "/pending.txt"));
    }

    void sanitizesFileNames_data() {
        QTest::addColumn<QString>("name");
        QTest::newRow("parent") << "../evil.exe";
        QTest::newRow("windows parent") << "..\\..\\evil.exe";
        QTest::newRow("drive") << "C:evil";
        QTest::newRow("dot-dot") << "..";
        QTest::newRow("dot") << ".";
        QTest::newRow("empty") << "";
        QTest::newRow("device") << "CON";
        QTest::newRow("device with extension") << "nul.txt";
        QTest::newRow("trailing dot and space") << "name. ";
        QTest::newRow("control character") << QStringLiteral("a") + QChar(7) + "b";
    }

    void sanitizesFileNames() {
        QFETCH(QString, name);
        const QString safe = safeFileName(name);
        QVERIFY(!safe.isEmpty());
        QVERIFY(safe != "." && safe != "..");
        for (QChar c : QStringLiteral("/\\:*?\"<>|")) QVERIFY2(!safe.contains(c), qPrintable(safe));
        QVERIFY(!safe.endsWith('.') && !safe.endsWith(' '));
        const QString stem = safe.section('.', 0, 0).toUpper();
        QVERIFY2(stem != "CON" && stem != "NUL", qPrintable(safe));
        QCOMPARE(safeFileName("report.pdf"), QStringLiteral("report.pdf"));
    }
};

QTEST_GUILESS_MAIN(StorageServiceTest)
#include "StorageServiceTest.moc"
