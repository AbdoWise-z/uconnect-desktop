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

    void usesTheStrictestNames() {
        QCOMPARE(nameProblem("report.txt"), QString());
        QCOMPARE(nameProblem("notes: draft.txt"), QStringLiteral("char::"));
        QCOMPARE(nameProblem("what?.txt"), QStringLiteral("char:?"));
        QCOMPARE(nameProblem("back\\slash"), QStringLiteral("char:\\"));
        QCOMPARE(nameProblem("tab\tname"), QStringLiteral("control"));
        QCOMPARE(nameProblem("ends with dot."), QStringLiteral("trailing"));
        QCOMPARE(nameProblem("ends with space "), QStringLiteral("trailing"));
        QCOMPARE(nameProblem("CON"), QStringLiteral("reserved"));
        QCOMPARE(nameProblem("lpt1.log"), QStringLiteral("reserved"));
        QCOMPARE(nameProblem(QString(256, QLatin1Char('a'))), QStringLiteral("long"));
        QCOMPARE(nameProblem(QStringLiteral("cafe\u0301")), QStringLiteral("unicode"));  // e + accent
        QCOMPARE(nameProblem(QStringLiteral("caf\u00e9")), QString());
        QCOMPARE(foldName("Docs/Report.TXT"), foldName("docs/report.txt"));
        QVERIFY(!isValidSyncPath("a/what?.txt"));
    }

    // On a disk that tells case apart, both can exist: the older one syncs.
    void holdsBackNewerNamesThatClash() {
        SyncFolder f(config(), "A", {});
        ScanResult scan;
        FileEntry older = file("Report.txt", "first", {});
        older.mtime     = 100;
        FileEntry newer = file("report.txt", "second", {});
        newer.mtime     = 200;
        FileEntry docs, docsLower, inside;
        docs.path = "Docs";
        docs.dir  = true;
        docsLower = docs;
        docsLower.path = "docs";
        inside = file("docs/x.txt", "x", {});
        scan.items << newer << older << docs << docsLower << inside;
        scan.unsupported.insert("bad:name.txt", "char::");
        f.applyScan(scan, f.seq);
        QCOMPARE(f.scanHeld.value("report.txt"), QStringLiteral("clash:Report.txt"));
        QCOMPARE(f.scanHeld.value("docs"), QStringLiteral("clash:Docs"));  // folders: by name
        QCOMPARE(f.scanHeld.value("docs/x.txt"), QStringLiteral("clash:Docs"));
        QCOMPARE(f.scanHeld.value("bad:name.txt"), QStringLiteral("name:char::"));
        QVERIFY(f.local.contains("Report.txt") && !f.local.contains("report.txt"));
        QVERIFY(!f.local.contains("docs/x.txt"));

        // The older one goes: the other syncs.
        scan.items = {newer};
        scan.unsupported.clear();
        f.applyScan(scan, f.seq);
        QVERIFY(f.scanHeld.isEmpty());
        QVERIFY(f.local["Report.txt"].deleted);
        QVERIFY(f.local.contains("report.txt") && !f.local["report.txt"].skipped);
    }

    // Across devices: a newer name here loses to an older one from a peer.
    void holdsBackNamesThatClashWithPeers() {
        SyncFolder f(config(), "A", {});
        FileEntry mine = file("report.txt", "mine", {{"A", 1}});
        mine.mtime     = 200;
        f.setLocal(mine);
        FileEntry theirs = file("Report.txt", "theirs", {{"B", 1}});
        theirs.mtime     = 100;
        f.applyRemote(kBob, 1, true, {theirs});
        QHash<QString, Decision> decisions{{"Report.txt", f.decide("Report.txt", neverConnected)}};
        QCOMPARE(decisions["Report.txt"].kind, Decision::Apply);
        auto held = f.clashes(decisions);
        QCOMPARE(held.value("report.txt"), QStringLiteral("clash:Report.txt"));
        QVERIFY(!held.contains("Report.txt"));

        // Ours is the older one: theirs waits.
        f.local["report.txt"].mtime = 50;
        held = f.clashes(decisions);
        QCOMPARE(held.value("Report.txt"), QStringLiteral("clash:report.txt"));
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
        f.peerStats[kBob] = {1000, 2000, 3};
        QVERIFY(f.save());
        SyncFolder g(config(), "A", state);
        QVERIFY(g.load());
        QCOMPARE(g.epoch, f.epoch);
        QCOMPARE(g.seq, f.seq);
        QCOMPARE(g.local["a.txt"].hash, f.local["a.txt"].hash);
        QCOMPARE(g.remotes[kBob].epoch, quint64(7));
        QCOMPARE(g.remotes[kBob].entries.size(), 1);
        QCOMPARE(g.peerStats[kBob].lastSeen, qint64(1000));
        QCOMPARE(g.peerStats[kBob].lastInSync, qint64(2000));
        QCOMPARE(g.peerStats[kBob].sentSeq, quint64(3));
    }

    // The editor shows which line decided a path, and which lines are no use.
    void explainsRules() {
        const IgnoreRules r(QStringLiteral("# comment\n*.tmp\n!keep.tmp\n/build\n\n!\ncache/\n"));
        QCOMPARE(r.problems().size(), 1);
        QCOMPARE(r.problems().first().line, 6);
        QCOMPARE(r.problems().first().pattern, QStringLiteral("!"));

        IgnoreRules::Match m = r.explain("a.tmp", false);
        QCOMPARE(m.line, 2);
        QCOMPARE(m.pattern, QStringLiteral("*.tmp"));
        QVERIFY(!m.negate);
        QVERIFY(m.via.isEmpty());

        m = r.explain("keep.tmp", false);  // brought back by the later line
        QCOMPARE(m.line, 3);
        QVERIFY(m.negate);

        m = r.explain("build/out/keep.tmp", false);  // the folder above decides
        QCOMPARE(m.line, 4);
        QCOMPARE(m.via, QStringLiteral("build"));
        QVERIFY(!m.negate);

        m = r.explain("x/cache", false);  // folders only: a file of that name is free
        QCOMPARE(m.line, 0);
        QCOMPARE(r.explain("x/cache", true).line, 7);
        QCOMPARE(r.explain("readme.md", false).line, 0);
        QCOMPARE(IgnoreRules().explain("a.tmp", false).line, 0);
    }

    // A paused member is sent nothing and nothing is taken from it, but its
    // grants stay for when it resumes.
    void pausedGrantsGrantNothing() {
        Grant g{true, true, true};
        QVERIFY(g.receive && g.contribute);
        QVERIFY(!g.sends() && !g.accepts());
        g.paused = false;
        QVERIFY(g.sends() && g.accepts());

        FolderConfig c = config();
        c.peers[kBob].paused = true;
        QTemporaryDir dir;
        SyncFolder f(c, "A", dir.filePath("p.cbor"));
        f.applyRemote(kBob, 1, true, {file("new.txt", "theirs", {{"B", 1}})});
        QCOMPARE(f.decide("new.txt", neverConnected).kind, Decision::None);
        f.config.peers[kBob].paused = false;
        QCOMPARE(f.decide("new.txt", neverConnected).kind, Decision::Apply);
    }
};

QTEST_GUILESS_MAIN(SyncLogicTest)
#include "SyncLogicTest.moc"
