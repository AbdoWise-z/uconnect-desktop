// The pure parts of folder sync: version vectors, ignore patterns, and the
// decisions a folder makes from its own index and its peers'.
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include "core/sync/SyncFolder.hpp"
#include "core/sync/SyncIgnore.hpp"
#include "core/sync/SyncModel.hpp"

using namespace ucd::sync;

namespace {

const QString kBob   = QStringLiteral("bbbb");  // identity keys of peers
const QString kCarol = QStringLiteral("cccc");

FileEntry file(const QString& path, const QByteArray& content, VersionVector v) {
    FileEntry e;
    e.path      = path;
    e.size      = content.size();
    e.hash      = QCryptographicHash::hash(content, QCryptographicHash::Sha256);
    e.blockSize = blockSizeFor(e.size);
    e.blocks << e.hash;
    e.version = std::move(v);
    return e;
}

FolderConfig config() {
    FolderConfig c;
    c.id   = QStringLiteral("f");
    c.path = QStringLiteral(".");
    c.peers.insert(kBob, {true, true});
    c.peers.insert(kCarol, {true, true});
    return c;
}

qint64 neverConnected(const QString&) { return -1; }

}  // namespace

class SyncLogicTest : public QObject {
    Q_OBJECT

private slots:
    void comparesVersionVectors() {
        QCOMPARE(compare({{"B", 1}}, {{"B", 1}}), Order::Equal);
        QCOMPARE(compare({{"A", 1}, {"B", 1}}, {{"B", 1}}), Order::Newer);
        QCOMPARE(compare({{"B", 1}}, {{"A", 1}, {"B", 1}}), Order::Older);
        // Carol's offline edit against Bob's newer one.
        QCOMPARE(compare({{"A", 1}, {"B", 1}, {"C", 1}}, {{"A", 1}, {"B", 2}}), Order::Concurrent);
        QCOMPARE(compare({}, {{"A", 1}}), Order::Older);
        const VersionVector keep = bumped(merged({{"A", 1}, {"B", 1}, {"C", 1}}, {{"A", 1}, {"B", 2}}), "C");
        QCOMPARE(keep, (VersionVector{{"A", 1}, {"B", 2}, {"C", 2}}));
        QCOMPARE(compare(keep, {{"A", 1}, {"B", 2}}), Order::Newer);
    }

    void matchesIgnorePatterns() {
        const IgnoreRules r(QStringLiteral("# comment\n*.tmp\n!keep.tmp\n/build\ndocs/*.pdf\ncache/\n**/logs\n"
                                           "[ab]?.bin\n"));
        QVERIFY(r.matches("a.tmp", false));
        QVERIFY(r.matches("deep/down/a.tmp", false));
        QVERIFY(!r.matches("keep.tmp", false));
        QVERIFY(r.matches("build", true));
        QVERIFY(r.matches("build/out/x.o", false));  // inside an ignored folder
        QVERIFY(!r.matches("src/build", true));      // anchored at the top
        QVERIFY(r.matches("docs/a.pdf", false));
        QVERIFY(!r.matches("docs/sub/a.pdf", false));
        QVERIFY(r.matches("x/cache", true));
        QVERIFY(!r.matches("x/cache", false));  // folders only
        QVERIFY(r.matches("a/b/logs/today.txt", false));
        QVERIFY(r.matches("a1.bin", false) && !r.matches("c1.bin", false));
        QVERIFY(!r.matches("readme.md", false));

        const QString odd = QStringLiteral("dir/[draft] *notes?.txt");
        const IgnoreRules exact(exactPattern(odd));
        QVERIFY(exact.matches(odd, false));
        QVERIFY(!exact.matches("dir/d *notesX.txt", false));
        QVERIFY(!exact.matches("other/dir/[draft] *notes?.txt", false));
    }

    void validatesPaths() {
        QVERIFY(isValidSyncPath("a/b.txt"));
        QVERIFY(isValidSyncPath(".syncignore"));
        QVERIFY(!isValidSyncPath(".syncskip"));
        QVERIFY(!isValidSyncPath("../x"));
        QVERIFY(!isValidSyncPath("a/../../x"));
        QVERIFY(!isValidSyncPath("/abs"));
        QVERIFY(!isValidSyncPath("a//b"));
        QVERIFY(!isValidSyncPath("a/.syncpart-x"));
        QVERIFY(!isValidSyncPath(".synctrash/a.txt"));
        QVERIFY(isValidSyncPath("sub/.synctrash"));  // reserved at the top only
        QVERIFY(!FileEntry::fromJson({{"p", "../x"}, {"v", QJsonObject{{"A", 1}}}, {"del", true}}));
        // Block hashes must agree with the size.
        FileEntry e = file("a.txt", "hello", {{"A", 1}});
        QVERIFY(FileEntry::fromJson(e.toJson()));
        QJsonObject bad = e.toJson();
        bad["s"] = 10'000'000;
        QVERIFY(!FileEntry::fromJson(bad));
    }

    void followsNewerVersions() {
        SyncFolder f(config(), "A", {});
        f.setLocal(file("a.txt", "old", {{"B", 1}}));
        f.applyRemote(kBob, 1, true, {file("a.txt", "new", {{"B", 2}})});
        Decision d = f.decide("a.txt", neverConnected);
        QCOMPARE(d.kind, Decision::Apply);
        QCOMPARE(d.sources, QStringList{kBob});
        QCOMPARE(d.target.version, (VersionVector{{"B", 2}}));

        f.setLocal(file("a.txt", "mine", {{"A", 1}, {"B", 2}}));  // we are ahead
        QCOMPARE(f.decide("a.txt", neverConnected).kind, Decision::None);

        // The same content under concurrent versions is no conflict.
        f.applyRemote(kCarol, 1, true, {file("a.txt", "mine", {{"B", 2}, {"C", 1}})});
        d = f.decide("a.txt", neverConnected);
        QCOMPARE(d.kind, Decision::Adopt);
        QCOMPARE(d.target.version, (VersionVector{{"A", 1}, {"B", 2}, {"C", 1}}));
    }

    void ignoresPeersWithoutContribute() {
        FolderConfig c = config();
        c.peers[kBob].contribute = false;
        SyncFolder f(c, "A", {});
        f.setLocal(file("a.txt", "old", {{"A", 1}}));
        f.applyRemote(kBob, 1, true, {file("a.txt", "new", {{"A", 1}, {"B", 1}})});
        QCOMPARE(f.decide("a.txt", neverConnected).kind, Decision::None);
    }

    void tellsOwnConflictsFromOthers() {
        // Carol edited offline while Bob edited: for Carol, it is her conflict.
        SyncFolder carol(config(), "C", {});
        carol.setLocal(file("plan.md", "carol", {{"A", 1}, {"B", 1}, {"C", 1}}));
        carol.applyRemote(kBob, 1, true, {file("plan.md", "bob", {{"A", 1}, {"B", 2}})});
        Decision d = carol.decide("plan.md", neverConnected);
        QCOMPARE(d.kind, Decision::Conflict);
        QVERIFY(d.involved);
        QCOMPARE(d.candidates.size(), 2);

        // Alice holds Bob's version and did not edit: she stays on Bob's chain.
        SyncFolder alice(config(), "A", {});
        alice.setLocal(file("plan.md", "bob", {{"A", 1}, {"B", 2}}));
        alice.applyRemote(kCarol, 1, true, {file("plan.md", "carol", {{"A", 1}, {"B", 1}, {"C", 1}})});
        d = alice.decide("plan.md", neverConnected);
        QCOMPARE(d.kind, Decision::Conflict);
        QVERIFY(!d.involved);
        QVERIFY(!d.apply);
        QCOMPARE(d.following, QString());
    }

    void followsTheLongestConnectionWhenBehind() {
        SyncFolder dave(config(), "D", {});
        dave.setLocal(file("plan.md", "old", {{"A", 1}, {"B", 1}}));
        dave.applyRemote(kBob, 1, true, {file("plan.md", "bob", {{"A", 1}, {"B", 2}})});
        dave.applyRemote(kCarol, 1, true, {file("plan.md", "carol", {{"A", 1}, {"B", 1}, {"C", 1}})});
        const auto since = [](const QString& key) -> qint64 { return key == kCarol ? 1000 : 5000; };
        Decision d = dave.decide("plan.md", since);
        QCOMPARE(d.kind, Decision::Conflict);
        QVERIFY(d.apply);
        QCOMPARE(d.following, kCarol);  // connected since 1000, before Bob
        QCOMPARE(d.sources, QStringList{kCarol});
        QCOMPARE(d.target.hash, file("x", "carol", {}).hash);
        QVERIFY(dave.decide("plan.md", neverConnected).waiting);
    }

    void resolvesConflictsForEveryone() {
        SyncFolder carol(config(), "C", {});
        carol.setLocal(file("plan.md", "carol", {{"A", 1}, {"B", 1}, {"C", 1}}));
        carol.applyRemote(kBob, 1, true, {file("plan.md", "bob", {{"A", 1}, {"B", 2}})});
        QVERIFY(carol.keepMine("plan.md"));
        QCOMPARE(carol.local["plan.md"].version, (VersionVector{{"A", 1}, {"B", 2}, {"C", 2}}));
        QCOMPARE(carol.decide("plan.md", neverConnected).kind, Decision::None);

        SyncFolder carol2(config(), "C", {});
        carol2.setLocal(file("plan.md", "carol", {{"A", 1}, {"B", 1}, {"C", 1}}));
        carol2.applyRemote(kBob, 1, true, {file("plan.md", "bob", {{"A", 1}, {"B", 2}})});
        QVERIFY(carol2.take("plan.md", kBob));
        const Decision d = carol2.decide("plan.md", neverConnected);
        QCOMPARE(d.kind, Decision::Apply);
        QCOMPARE(d.sources, QStringList{kBob});
        QCOMPARE(d.target.version, (VersionVector{{"A", 1}, {"B", 2}, {"C", 2}}));
    }

    void turnsScansIntoVersions() {
        SyncFolder f(config(), "A", {});
        ScanResult scan;
        scan.items << file("a.txt", "one", {}) << file("b.txt", "two", {});
        f.applyScan(scan, f.seq);
        QCOMPARE(f.local["a.txt"].version, (VersionVector{{"A", 1}}));

        // Unchanged: nothing; changed: a new version; gone: a tombstone.
        scan.items = {file("a.txt", "one", {}), file("b.txt", "TWO", {})};
        QCOMPARE(f.applyScan(scan, f.seq), 1);
        QCOMPARE(f.local["b.txt"].version, (VersionVector{{"A", 2}}));
        scan.items = {file("b.txt", "TWO", {})};
        f.applyScan(scan, f.seq);
        QVERIFY(f.local["a.txt"].deleted);
        QCOMPARE(f.local["a.txt"].version, (VersionVector{{"A", 2}}));

        // Newly ignored counts as deleted; skipped is marked, not deleted.
        scan.items.clear();
        scan.ignoreText = QStringLiteral("b.txt");
        f.applyScan(scan, f.seq);
        QVERIFY(f.local["b.txt"].deleted);
        f.setLocal(file("c.txt", "three", {{"A", 1}}));
        scan.ignoreText.clear();
        scan.skipText = QStringLiteral("c.txt");
        f.applyScan(scan, f.seq);
        QVERIFY(f.local["c.txt"].skipped && !f.local["c.txt"].deleted);
        // A peer's newer version of a skipped file is not applied.
        f.skip = IgnoreRules(QStringLiteral("c.txt"));
        f.applyRemote(kBob, 1, true, {file("c.txt", "bob", {{"A", 1}, {"B", 1}})});
        QCOMPARE(f.decide("c.txt", neverConnected).kind, Decision::None);
    }

    void keepsStateAcrossRestarts() {
        QTemporaryDir dir;
        const QString state = dir.filePath("f.cbor");
        SyncFolder f(config(), "A", state);
        f.setLocal(file("a.txt", "one", {{"A", 1}}));
        f.applyRemote(kBob, 7, true, {file("a.txt", "one", {{"A", 1}})});
        QVERIFY(f.save());
        SyncFolder g(config(), "A", state);
        QVERIFY(g.load());
        QCOMPARE(g.epoch, f.epoch);
        QCOMPARE(g.seq, f.seq);
        QCOMPARE(g.local["a.txt"].hash, f.local["a.txt"].hash);
        QCOMPARE(g.remotes[kBob].epoch, quint64(7));
        QCOMPARE(g.remotes[kBob].entries.size(), 1);
    }
};

QTEST_GUILESS_MAIN(SyncLogicTest)
#include "SyncLogicTest.moc"
