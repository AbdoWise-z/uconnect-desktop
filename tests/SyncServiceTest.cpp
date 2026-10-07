// Several SyncService instances joined by in-process links that can be cut
// and restored, each syncing its own temporary folder.
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QRandomGenerator>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <map>
#include <memory>
#include <set>

#include "core/Hello.hpp"
#include "core/sync/SyncService.hpp"

using namespace ucd;
using ucd::sync::FolderConfig;
using ucd::sync::Grant;

namespace {

const QString kFolder = QStringLiteral("0123456789abcdef0123456789abcdef");

struct Node {
    QString                      name;
    QString                      key;  // identity key, hex
    uconnect::DevId              dev{};
    QString                      root;
    QString                      state;
    std::unique_ptr<SyncService> svc;
    QVariantMap                  status;
    QStringList                  events;
};

class Net : public QObject {
public:
    static constexpr int kWindow = 8;

    QTemporaryDir                     dir;
    std::vector<std::unique_ptr<Node>> nodes;
    std::set<std::pair<int, int>>     links;
    std::map<std::pair<int, int>, qint64> since;
    std::map<std::pair<int, int>, int>    inFlight;
    qint64 dataBytes = 0;  // block data delivered, all links

    Node& add(const QString& name) {
        auto n  = std::make_unique<Node>();
        const int index = int(nodes.size());
        n->name = name;
        n->key  = QString::fromLatin1(QByteArray(32, char('a' + index)).toHex());
        n->dev[0] = uint8_t(index + 1);
        n->root   = dir.filePath(name);
        n->state  = dir.filePath(name + "-state");
        QDir().mkpath(n->root);
        n->svc = std::make_unique<SyncService>(
            [this, index](const uconnect::DevId& to, std::span<const uint8_t> bytes) { return deliver(index, to, bytes); },
            [this, index](const uconnect::DevId& d) {
                const int other = indexOf(d);
                return other >= 0 && linked(index, other) ? nodes[other]->key : QString();
            },
            [this, index](const uconnect::DevId& d) {
                const int other = indexOf(d);
                const auto it   = since.find({std::min(index, other), std::max(index, other)});
                return other >= 0 && it != since.end() ? it->second : qint64(-1);
            });
        n->svc->setIdentity(n->key, n->state);
        n->svc->setScanInterval(150);
        n->svc->setStableAfter(0);
        n->svc->setUseTrash(false);
        Node* raw = n.get();
        connect(n->svc.get(), &SyncService::folderChanged, this,
                [raw](const QString&, const QVariantMap& s) { raw->status = s; });
        connect(n->svc.get(), &SyncService::event, this,
                [raw](const QString&, const QString& path, const QString& kind) { raw->events << path + " " + kind; });
        nodes.push_back(std::move(n));
        return *nodes.back();
    }

    int indexOf(const uconnect::DevId& d) const {
        for (size_t i = 0; i < nodes.size(); ++i)
            if (nodes[i]->dev == d) return int(i);
        return -1;
    }
    bool linked(int a, int b) const { return links.contains({std::min(a, b), std::max(a, b)}); }

    bool deliver(int from, const uconnect::DevId& to, std::span<const uint8_t> bytes) {
        const int dest = indexOf(to);
        if (dest < 0 || !linked(from, dest)) return false;
        int& flying = inFlight[{from, dest}];
        if (flying >= kWindow) return false;
        ++flying;
        std::vector<uint8_t> copy(bytes.begin(), bytes.end());
        if (!copy.empty() && copy[0] == uint8_t(MessageKind::SyncData)) dataBytes += qint64(copy.size()) - 5;
        const uconnect::DevId fromDev = nodes[from]->dev;
        QMetaObject::invokeMethod(this, [this, from, dest, fromDev, copy] {
            --inFlight[{from, dest}];
            if (linked(from, dest)) nodes[dest]->svc->handleMessage(fromDev, copy);
        }, Qt::QueuedConnection);
        return true;
    }

    // `when`: the connection's start time, to order "connected longest".
    void link(int a, int b, qint64 when = -1) {
        const std::pair<int, int> k{std::min(a, b), std::max(a, b)};
        links.insert(k);
        since[k] = when >= 0 ? when : QDateTime::currentMSecsSinceEpoch();
        nodes[a]->svc->peerUp(nodes[b]->dev);
        nodes[b]->svc->peerUp(nodes[a]->dev);
    }
    void unlink(int a, int b) {
        links.erase({std::min(a, b), std::max(a, b)});
        nodes[a]->svc->peerGone(nodes[b]->dev);
        nodes[b]->svc->peerGone(nodes[a]->dev);
    }

    // Everyone in the folder, everyone allowed both ways unless changed.
    FolderConfig config(int i, std::map<int, Grant> grants = {}) {
        FolderConfig c;
        c.id    = kFolder;
        c.label = QStringLiteral("Shared");
        c.path  = nodes[i]->root;
        for (size_t j = 0; j < nodes.size(); ++j) {
            if (int(j) == i) continue;
            c.peers.insert(nodes[j]->key, grants.contains(int(j)) ? grants[int(j)] : Grant{true, true});
        }
        return c;
    }
    void join(int i, std::map<int, Grant> grants = {}) { nodes[i]->svc->setFolders({config(i, std::move(grants))}); }
};

void write(const QString& path, const QByteArray& data) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    QCOMPARE(f.write(data), data.size());
}

QByteArray contents(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray("<missing>");
}

QByteArray randomBytes(qsizetype n, quint32 seed) {
    QByteArray b(n, Qt::Uninitialized);
    QRandomGenerator(seed).fillRange(reinterpret_cast<quint32*>(b.data()), n / 4);
    return b;
}

int conflictsOf(const Node& n) {
    return int(n.status.value("conflicts").toList().size());
}

}  // namespace

class SyncServiceTest : public QObject {
    Q_OBJECT
    std::unique_ptr<Net> net_;

    QString at(int node, const QString& rel) const { return QDir(net_->nodes[node]->root).filePath(rel); }

private slots:
    void init() { net_ = std::make_unique<Net>(); }
    void cleanup() { net_.reset(); }

    void syncsANewMemberAndThenChanges() {
        Net& n = *net_;
        n.add("alice");
        n.add("bob");
        const QByteArray big = randomBytes(3 * 1024 * 1024 + 7, 1);
        write(at(0, "a.txt"), "alpha");
        write(at(0, "sub/b.txt"), "bravo");
        write(at(0, "big.bin"), big);
        QDir().mkpath(at(0, "empty"));
        n.join(0);
        n.join(1);
        n.link(0, 1);

        QTRY_COMPARE_WITH_TIMEOUT(contents(at(1, "big.bin")), big, 15000);
        QTRY_COMPARE(contents(at(1, "a.txt")), QByteArray("alpha"));
        QTRY_COMPARE(contents(at(1, "sub/b.txt")), QByteArray("bravo"));
        QTRY_VERIFY(QFileInfo(at(1, "empty")).isDir());
        QVERIFY(QDir(at(1, "")).entryList({".syncpart-*"}, QDir::Files | QDir::Hidden).isEmpty());

        // Back the other way: an edit, a delete and a new file.
        write(at(1, "a.txt"), "alpha, edited by bob");
        QFile::remove(at(1, "sub/b.txt"));
        write(at(1, "new.txt"), "from bob");
        QTRY_COMPARE_WITH_TIMEOUT(contents(at(0, "a.txt")), QByteArray("alpha, edited by bob"), 10000);
        QTRY_VERIFY(!QFile::exists(at(0, "sub/b.txt")));
        QTRY_COMPARE(contents(at(0, "new.txt")), QByteArray("from bob"));
        QCOMPARE(conflictsOf(*n.nodes[0]), 0);
    }

    void reusesBlocksItAlreadyHas() {
        Net& n = *net_;
        n.add("alice");
        n.add("bob");
        const QByteArray big = randomBytes(2 * 1024 * 1024, 2);
        write(at(0, "big.bin"), big);
        n.join(0);
        n.join(1);
        n.link(0, 1);
        QTRY_COMPARE_WITH_TIMEOUT(contents(at(1, "big.bin")), big, 15000);

        // A rename, and a copy with one block changed, send (almost) nothing.
        n.dataBytes = 0;
        QVERIFY(QFile::rename(at(0, "big.bin"), at(0, "moved.bin")));
        QByteArray edited = big;
        edited[100] = char(edited[100] ^ 0x5a);
        write(at(0, "copy.bin"), edited);
        QTRY_COMPARE_WITH_TIMEOUT(contents(at(1, "moved.bin")), big, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(contents(at(1, "copy.bin")), edited, 10000);
        QTRY_VERIFY(!QFile::exists(at(1, "big.bin")));
        QVERIFY2(n.dataBytes <= 128 * 1024, qPrintable(QString::number(n.dataBytes)));  // one block
    }

    void receiveOnlyMembersDoNotChangeOthers() {
        Net& n = *net_;
        n.add("alice");
        n.add("bob");
        write(at(0, "doc.txt"), "original");
        n.join(0, {{1, Grant{true, false}}});  // alice sends to bob, takes nothing from him
        n.join(1);
        n.link(0, 1);
        QTRY_COMPARE_WITH_TIMEOUT(contents(at(1, "doc.txt")), QByteArray("original"), 10000);
        write(at(1, "doc.txt"), "bob's change");
        write(at(1, "bobs.txt"), "bob");
        QTest::qWait(1500);
        QCOMPARE(contents(at(0, "doc.txt")), QByteArray("original"));
        QVERIFY(!QFile::exists(at(0, "bobs.txt")));
        // Alice's own changes still reach him (his edit becomes a conflict there).
        write(at(0, "more.txt"), "more");
        QTRY_COMPARE_WITH_TIMEOUT(contents(at(1, "more.txt")), QByteArray("more"), 10000);
    }

    void conflictsWaitForTheirOwnersAndSettleEverywhere() {
        Net& n = *net_;
        n.add("alice");
        n.add("bob");
        write(at(0, "plan.md"), "v1");
        n.join(0);
        n.join(1);
        n.link(0, 1);
        QTRY_COMPARE_WITH_TIMEOUT(contents(at(1, "plan.md")), QByteArray("v1"), 10000);

        n.unlink(0, 1);
        write(at(0, "plan.md"), "alice's version");
        write(at(1, "plan.md"), "bob's longer version");
        QTest::qWait(600);  // both scanned
        n.link(0, 1);
        QTRY_COMPARE_WITH_TIMEOUT(conflictsOf(*n.nodes[0]), 1, 10000);
        QTRY_COMPARE(conflictsOf(*n.nodes[1]), 1);
        const QVariantMap c = n.nodes[0]->status.value("conflicts").toList().first().toMap();
        QVERIFY(c.value("involved").toBool());
        QCOMPARE(contents(at(0, "plan.md")), QByteArray("alice's version"));  // nothing overwritten
        QCOMPARE(contents(at(1, "plan.md")), QByteArray("bob's longer version"));

        n.nodes[0]->svc->resolve(kFolder, "plan.md", "mine", {});
        QTRY_COMPARE_WITH_TIMEOUT(contents(at(1, "plan.md")), QByteArray("alice's version"), 10000);
        QTRY_COMPARE(conflictsOf(*n.nodes[0]), 0);
        QTRY_COMPARE(conflictsOf(*n.nodes[1]), 0);
    }

    void takingTheirsFetchesIt() {
        Net& n = *net_;
        n.add("alice");
        n.add("bob");
        write(at(0, "plan.md"), "v1");
        n.join(0);
        n.join(1);
        n.link(0, 1);
        QTRY_COMPARE_WITH_TIMEOUT(contents(at(1, "plan.md")), QByteArray("v1"), 10000);
        n.unlink(0, 1);
        write(at(0, "plan.md"), "alice's version");
        write(at(1, "plan.md"), "bob's longer version");
        QTest::qWait(600);
        n.link(0, 1);
        QTRY_COMPARE_WITH_TIMEOUT(conflictsOf(*n.nodes[0]), 1, 10000);
        n.nodes[0]->svc->resolve(kFolder, "plan.md", "theirs", n.nodes[1]->key);
        QTRY_COMPARE_WITH_TIMEOUT(contents(at(0, "plan.md")), QByteArray("bob's longer version"), 10000);
        QTRY_COMPARE(conflictsOf(*n.nodes[0]), 0);
        QTRY_COMPARE(conflictsOf(*n.nodes[1]), 0);
        QCOMPARE(contents(at(1, "plan.md")), QByteArray("bob's longer version"));
    }

    // Dave did not edit and is behind both sides: he follows the one he has
    // been connected to longest, and says so.
    void bystandersFollowTheLongestConnection() {
        Net& n = *net_;
        n.add("bob");
        n.add("carol");
        n.add("dave");
        write(at(0, "plan.md"), "v1");
        n.join(0);
        n.join(1);
        n.join(2);
        n.link(0, 2);
        n.link(1, 2);
        QTRY_COMPARE_WITH_TIMEOUT(contents(at(1, "plan.md")), QByteArray("v1"), 10000);
        QTRY_COMPARE(contents(at(2, "plan.md")), QByteArray("v1"));

        n.unlink(0, 2);
        n.unlink(1, 2);
        write(at(0, "plan.md"), "bob's");
        write(at(1, "plan.md"), "carol's version");
        QTest::qWait(600);
        n.link(1, 2, 1000);  // carol: connected since earlier
        n.link(0, 2, 5000);
        QTRY_COMPARE_WITH_TIMEOUT(contents(at(2, "plan.md")), QByteArray("carol's version"), 10000);
        QTRY_VERIFY(n.nodes[2]->events.join(";").contains("plan.md following:" + n.nodes[1]->key) ||
                    n.nodes[2]->events.join(";").contains("plan.md kept"));
        QTRY_COMPARE(conflictsOf(*n.nodes[0]), 1);  // still bob's to decide
        QCOMPARE(contents(at(0, "plan.md")), QByteArray("bob's"));
    }

    void followsIgnoreAndSkipRules() {
        Net& n = *net_;
        n.add("alice");
        n.add("bob");
        write(at(0, ".syncignore"), "*.log\n");
        write(at(0, "a.log"), "log");
        write(at(0, "notes.txt"), "notes");
        write(at(0, "local.txt"), "shared at first");
        n.join(0);
        n.join(1);
        n.link(0, 1);
        QTRY_COMPARE_WITH_TIMEOUT(contents(at(1, "notes.txt")), QByteArray("notes"), 10000);
        QTRY_COMPARE(contents(at(1, ".syncignore")), QByteArray("*.log\n"));
        QTRY_COMPARE(contents(at(1, "local.txt")), QByteArray("shared at first"));
        QVERIFY(!QFile::exists(at(1, "a.log")));

        // Newly ignored: deleted as far as sync goes, but every member ignores
        // it too, so nobody's copy is removed.
        write(at(0, ".syncignore"), "*.log\nnotes.txt\n");
        QTRY_COMPARE_WITH_TIMEOUT(contents(at(1, ".syncignore")), QByteArray("*.log\nnotes.txt\n"), 10000);
        QTest::qWait(800);
        QVERIFY(QFile::exists(at(0, "notes.txt")));
        QVERIFY(QFile::exists(at(1, "notes.txt")));

        // Skipped on bob: his copy goes its own way, and .syncskip stays his.
        write(at(1, ".syncskip"), "local.txt\n");
        QTest::qWait(400);
        write(at(1, "local.txt"), "bob keeps this");
        write(at(0, "local.txt"), "alice changed it");
        QTest::qWait(1500);
        QCOMPARE(contents(at(0, "local.txt")), QByteArray("alice changed it"));
        QCOMPARE(contents(at(1, "local.txt")), QByteArray("bob keeps this"));
        QVERIFY(!QFile::exists(at(0, ".syncskip")));
    }

    // A member joining while already connected (as when accepting an offer).
    void joinsWhileConnected() {
        Net& n = *net_;
        n.add("alice");
        n.add("bob");
        write(at(0, "a.txt"), "alpha");
        n.join(0);
        n.link(0, 1);
        QTest::qWait(500);
        n.join(1);
        QTRY_COMPARE_WITH_TIMEOUT(contents(at(1, "a.txt")), QByteArray("alpha"), 10000);
        write(at(1, "b.txt"), "from bob");
        QTRY_COMPARE_WITH_TIMEOUT(contents(at(0, "b.txt")), QByteArray("from bob"), 10000);
    }

    void relaysThroughAMember() {
        Net& n = *net_;
        n.add("alice");
        n.add("bob");
        n.add("carol");
        n.join(0);
        n.join(1);
        n.join(2);
        n.link(0, 1);
        n.link(1, 2);  // alice and carol never meet
        write(at(0, "hello.txt"), "from alice");
        QTRY_COMPARE_WITH_TIMEOUT(contents(at(2, "hello.txt")), QByteArray("from alice"), 15000);
        write(at(2, "reply.txt"), "from carol");
        QTRY_COMPARE_WITH_TIMEOUT(contents(at(0, "reply.txt")), QByteArray("from carol"), 15000);
    }

    void refusesWhatWouldLeaveTheFolder() {
        Net& n = *net_;
        n.add("alice");
        n.add("bob");
        const QString outside = n.dir.filePath("outside");
        write(outside + "/secret.txt", "secret");
        n.join(0);
        n.join(1);
        n.link(0, 1);
#ifdef Q_OS_WIN
        const bool linked = QProcess::execute("cmd", {"/c", "mklink", "/J", QDir::toNativeSeparators(at(1, "escape")),
                                                      QDir::toNativeSeparators(outside)}) == 0;
#else
        const bool linked = QFile::link(outside, at(1, "escape"));
#endif
        // A crafted index from bob: paths out of the folder are dropped, and a
        // file whose blocks bob will not serve is never written.
        sync::FileEntry fake;
        fake.path      = "escape/planted.txt";
        fake.size      = 5;
        fake.hash      = QByteArray(32, 'x');
        fake.blockSize = 128 * 1024;
        fake.blocks << QByteArray(32, 'x');
        fake.version.insert("zz", 1);
        QJsonObject evil = fake.toJson();
        evil["p"]        = "../planted.txt";
        n.nodes[0]->svc->handleMessage(n.nodes[1]->dev,
                                       encodeControl({{"t", "sync.index"}, {"folder", kFolder}, {"epoch", "1"},
                                                      {"full", false}, {"entries", QJsonArray{evil, fake.toJson()}}}));
        write(at(0, "fine.txt"), "fine");
        QTRY_COMPARE_WITH_TIMEOUT(contents(at(1, "fine.txt")), QByteArray("fine"), 10000);
        QTest::qWait(800);
        QVERIFY(!QFile::exists(n.dir.filePath("planted.txt")));
        QVERIFY(!QFile::exists(outside + "/planted.txt"));
        if (linked) QVERIFY(!QFile::exists(at(0, "escape/secret.txt")));  // bob's link is not synced
    }
};

QTEST_GUILESS_MAIN(SyncServiceTest)
#include "SyncServiceTest.moc"
