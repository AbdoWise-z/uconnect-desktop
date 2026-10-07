// Two StorageService instances joined back to back: one offers a folder, the
// other browses and downloads. Covers the protocol end to end and, above all,
// that nothing outside the shared folder can be reached.

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QRandomGenerator>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

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

    Link() {
        requester = std::make_unique<StorageService>(
            [this](const uconnect::DevId&, std::span<const uint8_t> bytes) {
                return deliver(offerer.get(), kRequester, bytes, toOfferer);
            },
            [](const uconnect::DevId&, const QString&) { return false; });
        offerer = std::make_unique<StorageService>(
            [this](const uconnect::DevId&, std::span<const uint8_t> bytes) {
                return deliver(requester.get(), kOfferer, bytes, toRequester);
            },
            [this](const uconnect::DevId& dev, const QString& permission) {
                return dev == kRequester && granted.contains(permission);
            });
    }
    Link(const Link&) = delete;

    static bool deliver(StorageService* to, uconnect::DevId from, std::span<const uint8_t> bytes, int& inFlight) {
        if (inFlight >= kWindow) return false;
        ++inFlight;
        std::vector<uint8_t> copy(bytes.begin(), bytes.end());
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

    DownloadResult upload(const QString& localPath, const QString& folder = QStringLiteral("/share/sub")) {
        const quint32 id = nextId();
        QSignalSpy spy(link_->requester.get(), &StorageService::uploadFinished);
        link_->requester->upload(kOfferer, localPath, folder, id);
        if (spy.isEmpty()) spy.wait(10000);
        for (const auto& args : spy)
            if (args.at(0).toUInt() == id) return {args.at(1).toString(), args.at(2).toString()};
        return {{}, QStringLiteral("timed out")};
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
        const QString link = share_ + "/escape";
#ifdef Q_OS_WIN
        haveEscapeLink_ = QProcess::execute("cmd", {"/c", "mklink", "/J", QDir::toNativeSeparators(link),
                                                    QDir::toNativeSeparators(outside_)}) == 0;
#else
        haveEscapeLink_ = QFile::link(outside_, link);
#endif
    }

    void init() {
        link_ = std::make_unique<Link>();
        link_->offerer->setOffered(true);
        link_->offerer->setSharedFolders({share_});
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
        QTRY_VERIFY(QDir(share_ + "/sub").entryList({"*.part"}, QDir::Files | QDir::Hidden).isEmpty());
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
                return Link::deliver(link_->offerer.get(), kRequester, bytes, link_->toOfferer);
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
