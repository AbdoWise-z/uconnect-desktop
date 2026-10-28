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
    QHash<QString, int>          offered;  // peer key -> folders in its last offers
};

class Net : public QObject {
public:
    static constexpr int kWindow = 8;

    QTemporaryDir                     dir;
    std::vector<std::unique_ptr<Node>> nodes;
    std::set<std::pair<int, int>>     links;
    std::map<std::pair<int, int>, qint64> since;
    std::map<std::pair<int, int>, int>    inFlight;
    std::set<std::pair<int, int>>     noOffers;  // (from, to): `from` offers nothing to `to`
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
            },
            nullptr,
            [this, index](const uconnect::DevId& d) { return !noOffers.count({index, indexOf(d)}); });
        n->svc->setIdentity(n->key, n->state);
        n->svc->setScanInterval(150);
        n->svc->setStableAfter(0);
        n->svc->setUseTrash(false);
        Node* raw = n.get();
        connect(n->svc.get(), &SyncService::folderChanged, this,
                [raw](const QString&, const QVariantMap& s) { raw->status = s; });
        connect(n->svc.get(), &SyncService::event, this,
                [raw](const QString&, const QString& path, const QString& kind) { raw->events << path + " " + kind; });
        connect(n->svc.get(), &SyncService::offersChanged, this,
                [raw](const QString& key, const QVariantList& offers) { raw->offered[key] = int(offers.size()); });
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

// path -> state, as the user would see it.
QHash<QString, QString> statesOf(const Node& n) {
    QHash<QString, QString> out;
    for (const QVariant& v : n.svc->files(kFolder))
        out.insert(v.toMap().value("path").toString(), v.toMap().value("state").toString());
    return out;
}

QVariantMap rowOf(const Node& n, const QString& path) {
    for (const QVariant& v : n.svc->files(kFolder))
        if (v.toMap().value("path").toString() == path) return v.toMap();
    return {};
}

QVariantMap memberOf(const Node& n, const QString& key) {
    for (const QVariant& v : n.status.value("members").toList())
        if (v.toMap().value("key").toString() == key) return v.toMap();
    return {};
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
        // Kept aside, not gone.
        const QStringList kept = QDir(at(0, ".synctrash/sub")).entryList({"b~*.txt"}, QDir::Files);
        QCOMPARE(kept.size(), 1);
        QCOMPARE(contents(at(0, ".synctrash/sub/" + kept.first())), QByteArray("bravo"));
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

    // A rename moves the file in place: nothing fetched, nothing kept aside.
    // A change of case only is one too.
    void renamesInPlace() {
        Net& n = *net_;
        n.add("alice");
        n.add("bob");
        const QByteArray big = randomBytes(1024 * 1024, 3);
        write(at(0, "big.bin"), big);
        write(at(0, "report.txt"), "report");
        n.join(0);
        n.join(1);
        n.link(0, 1);
        QTRY_COMPARE_WITH_TIMEOUT(contents(at(1, "big.bin")), big, 10000);
        QTRY_COMPARE(contents(at(1, "report.txt")), QByteArray("report"));

        n.dataBytes = 0;
        QVERIFY(QFile::rename(at(0, "big.bin"), at(0, "moved.bin")));
        QVERIFY(QFile::rename(at(0, "report.txt"), at(0, "step.tmp")));
        QVERIFY(QFile::rename(at(0, "step.tmp"), at(0, "Report.txt")));
        QTRY_COMPARE_WITH_TIMEOUT(contents(at(1, "moved.bin")), big, 10000);
        QTRY_VERIFY(QDir(at(1, "")).entryList(QDir::Files).contains("Report.txt"));
        QVERIFY(!QDir(at(1, "")).entryList(QDir::Files).contains("report.txt"));
        QTRY_VERIFY(!QFile::exists(at(1, "big.bin")));
        QCOMPARE(n.dataBytes, 0);
        QVERIFY(!QFileInfo::exists(at(1, ".synctrash")));
        QTest::qWait(500);
        QCOMPARE(conflictsOf(*n.nodes[1]), 0);
    }

    // Two devices made the same name in different case: the older one is
    // synced, the newer one stays where it is but is held back, and syncs
    // once the older one goes.
    void namesThatClashWaitForTheOlderOne() {
        Net& n = *net_;
        n.add("alice");
        n.add("bob");
        write(at(0, "Report.txt"), "alice's, older");
        write(at(1, "report.txt"), "bob's, newer");
        {
            QFile old(at(0, "Report.txt"));
            QVERIFY(old.open(QIODevice::ReadWrite));
            old.setFileTime(QDateTime::currentDateTime().addDays(-10), QFileDevice::FileModificationTime);
        }
        n.join(0);
        n.join(1);
        QTest::qWait(600);  // each has scanned its own
        n.link(0, 1);
        QTRY_VERIFY_WITH_TIMEOUT([&] {
            for (const QVariant& h : n.nodes[1]->status.value("held").toList())
                if (h.toMap().value("path") == "report.txt" && h.toMap().value("reason") == "clash:Report.txt")
                    return true;
            return false;
        }(), 10000);
        QTest::qWait(800);
        QCOMPARE(contents(at(0, "Report.txt")), QByteArray("alice's, older"));  // nothing written over
        QCOMPARE(contents(at(1, "report.txt")), QByteArray("bob's, newer"));
        QVERIFY(!QDir(at(0, "")).entryList(QDir::Files).contains("report.txt"));

        // Alice's goes: Bob's syncs.
        QVERIFY(QFile::remove(at(0, "Report.txt")));
        QTRY_COMPARE_WITH_TIMEOUT(contents(at(0, "report.txt")), QByteArray("bob's, newer"), 15000);
        QVERIFY(QDir(at(0, "")).entryList(QDir::Files).contains("report.txt"));
        QCOMPARE(contents(at(1, "report.txt")), QByteArray("bob's, newer"));
    }

    void doesNotSyncNamesSomeMemberCannotHold() {
        Net& n = *net_;
        n.add("alice");
        n.add("bob");
#ifdef Q_OS_WIN
        // Windows cannot even create such names; the rule shows on Unicode.
        const QString bad = QStringLiteral("cafe\u0301.txt");
#else
        const QString bad = QStringLiteral("notes: draft.txt");
#endif
        write(at(0, bad), "x");
        write(at(0, "fine.txt"), "fine");
        n.join(0);
        n.join(1);
        n.link(0, 1);
        QTRY_COMPARE_WITH_TIMEOUT(contents(at(1, "fine.txt")), QByteArray("fine"), 10000);
        QTRY_VERIFY([&] {
            for (const QVariant& h : n.nodes[0]->status.value("held").toList())
                if (h.toMap().value("reason").toString().startsWith("name:")) return true;
            return false;
        }());
        QCOMPARE(QDir(at(1, "")).entryList(QDir::Files), QStringList{"fine.txt"});
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
        // Alice's own version, given up, is kept aside.
        const QStringList kept = QDir(at(0, ".synctrash")).entryList({"plan~*.md"}, QDir::Files);
        QCOMPARE(kept.size(), 1);
        QCOMPARE(contents(at(0, ".synctrash/" + kept.first())), QByteArray("alice's version"));
    }

    void keepsDeletedFilesForTheirTimeOnly() {
        Net& n = *net_;
        n.add("alice");
        n.add("bob");
        const QString fresh = QStringLiteral(".synctrash/fresh~%1.txt")
                                  .arg(QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss"));
        write(at(0, ".synctrash/old~20200101-000000.txt"), "old");
        write(at(0, ".synctrash/deep/older~20200101-000000"), "older");
        write(at(0, fresh), "fresh");
        write(at(0, "a.txt"), "alpha");
        n.join(0);
        n.join(1);
        n.link(0, 1);
        QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(at(0, ".synctrash/old~20200101-000000.txt")), 5000);
        QTRY_VERIFY(!QFileInfo::exists(at(0, ".synctrash/deep")));  // emptied folders go too
        QVERIFY(QFile::exists(at(0, fresh)));
        QTRY_COMPARE_WITH_TIMEOUT(contents(at(1, "a.txt")), QByteArray("alpha"), 10000);
        QVERIFY(!QFileInfo::exists(at(1, ".synctrash")));  // never synced
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

    // --- what the user sees -------------------------------------------------------------

    void listsFilesWithTheirState() {
        Net& n = *net_;
        n.add("alice");
        n.add("bob");
        write(at(0, "a.txt"), "alpha");
        write(at(0, "sub/b.txt"), "bravo");
        write(at(0, "x.log"), "log");
        write(at(0, "mine.txt"), "mine");
        write(at(0, ".syncignore"), "*.log\n");
        write(at(0, ".syncskip"), "mine.txt\n");
        n.join(0);
        n.join(1);
        // Alone: everything of hers is still to be sent to bob.
        QTRY_VERIFY_WITH_TIMEOUT(statesOf(*n.nodes[0]).contains("sub/b.txt"), 5000);
        QHash<QString, QString> states = statesOf(*n.nodes[0]);
        QCOMPARE(states.value("a.txt"), QStringLiteral("send"));
        QCOMPARE(states.value("sub/b.txt"), QStringLiteral("send"));
        QCOMPARE(states.value("sub"), QStringLiteral("send"));
        QCOMPARE(states.value("x.log"), QStringLiteral("ignored"));
        QCOMPARE(rowOf(*n.nodes[0], "x.log").value("detail").toString(), QStringLiteral("*.log"));
        QCOMPARE(states.value("mine.txt"), QStringLiteral("skipped"));
        QCOMPARE(rowOf(*n.nodes[0], "mine.txt").value("detail").toString(), QStringLiteral("mine.txt"));
        QVERIFY(rowOf(*n.nodes[0], "sub").value("dir").toBool());
        QCOMPARE(rowOf(*n.nodes[0], "a.txt").value("size").toLongLong(), qint64(5));
        QTRY_COMPARE_WITH_TIMEOUT(memberOf(*n.nodes[0], n.nodes[1]->key).value("toSendFiles").toLongLong(), qint64(3), 5000);
        QVERIFY(!memberOf(*n.nodes[0], n.nodes[1]->key).value("online").toBool());

        n.link(0, 1);
        QTRY_COMPARE_WITH_TIMEOUT(contents(at(1, "sub/b.txt")), QByteArray("bravo"), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(statesOf(*n.nodes[0]).value("sub/b.txt"), QStringLiteral("synced"), 5000);
        states = statesOf(*n.nodes[0]);
        QCOMPARE(states.value("a.txt"), QStringLiteral("synced"));
        QCOMPARE(states.value(".syncignore"), QStringLiteral("synced"));
        QCOMPARE(states.value("x.log"), QStringLiteral("ignored"));
        QCOMPARE(states.value("mine.txt"), QStringLiteral("skipped"));
        QVERIFY(!states.contains(".syncskip"));
        states = statesOf(*n.nodes[1]);
        QCOMPARE(states.value("a.txt"), QStringLiteral("synced"));
        QVERIFY(!states.contains("x.log"));  // never came, and is not here
        QVERIFY(!states.contains("mine.txt"));
        QTRY_COMPARE_WITH_TIMEOUT(memberOf(*n.nodes[0], n.nodes[1]->key).value("toSendFiles").toLongLong(), qint64(0), 5000);
        QVERIFY(memberOf(*n.nodes[0], n.nodes[1]->key).value("online").toBool());
        QTRY_VERIFY_WITH_TIMEOUT(memberOf(*n.nodes[0], n.nodes[1]->key).value("lastInSync").toLongLong() > 0, 5000);
        // Skipped before it ever synced, mine.txt is in nobody's index.
        QCOMPARE(memberOf(*n.nodes[1], n.nodes[0]->key).value("skipped").toLongLong(), qint64(0));

        // A path in depth.
        const QVariantMap info = n.nodes[1]->svc->fileInfo(kFolder, "a.txt");
        QCOMPARE(info.value("state").toString(), QStringLiteral("synced"));
        QCOMPARE(info.value("holders").toList().size(), 1);
        QCOMPARE(info.value("version").toMap().size(), 1);

        // Bob's copy of the index outlives the connection: what he has yet
        // to fetch waits for alice to come back.
        n.unlink(0, 1);
        write(at(0, "late.txt"), "late");
        QTRY_COMPARE_WITH_TIMEOUT(statesOf(*n.nodes[0]).value("late.txt"), QStringLiteral("send"), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(memberOf(*n.nodes[0], n.nodes[1]->key).value("toSendFiles").toLongLong(), qint64(1), 5000);
        FolderConfig paused = n.config(1);
        paused.paused       = true;  // takes the index, fetches nothing
        n.nodes[1]->svc->setFolders({paused});
        n.link(0, 1);
        QTRY_VERIFY_WITH_TIMEOUT(!n.nodes[1]->svc->fileInfo(kFolder, "late.txt").value("holders").toList().isEmpty(), 10000);
        n.unlink(0, 1);
        n.join(1);
        QTRY_COMPARE_WITH_TIMEOUT(statesOf(*n.nodes[1]).value("late.txt"), QStringLiteral("receive"), 5000);
        QCOMPARE(rowOf(*n.nodes[1], "late.txt").value("detail").toString(), QStringLiteral("waiting"));
        QTRY_COMPARE_WITH_TIMEOUT(memberOf(*n.nodes[1], n.nodes[0]->key).value("toReceiveFiles").toLongLong(), qint64(1), 5000);
        QVERIFY(!QFile::exists(at(1, "late.txt")));
        n.link(0, 1);
        QTRY_COMPARE_WITH_TIMEOUT(contents(at(1, "late.txt")), QByteArray("late"), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(statesOf(*n.nodes[1]).value("late.txt"), QStringLiteral("synced"), 5000);
    }

    void holdsBackLargeDeletes() {
        Net& n = *net_;
        n.add("alice");
        n.add("bob");
        for (int i = 0; i < 12; ++i) write(at(0, QStringLiteral("f%1.txt").arg(i)), "file");
        n.join(0);
        n.join(1);
        n.link(0, 1);
        QTRY_COMPARE_WITH_TIMEOUT(contents(at(1, "f11.txt")), QByteArray("file"), 15000);
        QTRY_COMPARE_WITH_TIMEOUT(n.nodes[1]->status.value("files").toLongLong(), qint64(12), 5000);

        // Alice wipes most of her copy: bob keeps his files and asks.
        for (int i = 0; i < 11; ++i) QVERIFY(QFile::remove(at(0, QStringLiteral("f%1.txt").arg(i))));
        QTRY_COMPARE_WITH_TIMEOUT(n.nodes[1]->status.value("pendingDeleteCount").toLongLong(), qint64(11), 10000);
        QTRY_VERIFY(n.nodes[1]->events.join(";").contains(" deletes:11"));
        QTest::qWait(500);
        QCOMPARE(contents(at(1, "f0.txt")), QByteArray("file"));
        QCOMPARE(n.nodes[1]->status.value("pendingDeletes").toList().size(), 11);

        // Keeping them makes them bob's changes: alice gets them back.
        n.nodes[1]->svc->confirmDeletes(kFolder, false);
        QTRY_COMPARE_WITH_TIMEOUT(contents(at(0, "f0.txt")), QByteArray("file"), 15000);
        QTRY_COMPARE_WITH_TIMEOUT(contents(at(0, "f10.txt")), QByteArray("file"), 15000);
        QTRY_COMPARE_WITH_TIMEOUT(n.nodes[1]->status.value("pendingDeleteCount").toLongLong(), qint64(0), 5000);
        QCOMPARE(n.nodes[1]->status.value("files").toLongLong(), qint64(12));

        // Deleting them again, and this time letting it happen.
        QTRY_COMPARE_WITH_TIMEOUT(n.nodes[0]->status.value("files").toLongLong(), qint64(12), 5000);
        QTest::qWait(300);
        for (int i = 0; i < 11; ++i) QVERIFY(QFile::remove(at(0, QStringLiteral("f%1.txt").arg(i))));
        QTRY_COMPARE_WITH_TIMEOUT(n.nodes[1]->status.value("pendingDeleteCount").toLongLong(), qint64(11), 10000);
        n.nodes[1]->svc->confirmDeletes(kFolder, true);
        QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(at(1, "f0.txt")), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(n.nodes[1]->status.value("files").toLongLong(), qint64(1), 5000);
        QCOMPARE(contents(at(1, "f11.txt")), QByteArray("file"));
        QCOMPARE(n.nodes[1]->status.value("pendingDeleteCount").toLongLong(), qint64(0));

        // A few deletes in a big folder pass without asking.
        for (int i = 0; i < 40; ++i) write(at(0, QStringLiteral("g%1.txt").arg(i)), "more");
        QTRY_COMPARE_WITH_TIMEOUT(n.nodes[1]->status.value("files").toLongLong(), qint64(41), 15000);
        QTest::qWait(300);
        for (int i = 0; i < 5; ++i) QVERIFY(QFile::remove(at(0, QStringLiteral("g%1.txt").arg(i))));
        QTRY_COMPARE_WITH_TIMEOUT(n.nodes[1]->status.value("files").toLongLong(), qint64(36), 10000);
        QCOMPARE(n.nodes[1]->status.value("pendingDeleteCount").toLongLong(), qint64(0));
    }

    void keepsAndRestoresKeptFiles() {
        Net& n = *net_;
        n.add("alice");
        n.add("bob");
        write(at(0, "docs/x.txt"), "first");
        write(at(0, "y.txt"), "why");
        n.join(0);
        n.join(1);
        n.link(0, 1);
        QTRY_COMPARE_WITH_TIMEOUT(contents(at(1, "docs/x.txt")), QByteArray("first"), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(contents(at(1, "y.txt")), QByteArray("why"), 10000);
        QTest::qWait(300);
        QVERIFY(QFile::remove(at(0, "docs/x.txt")));
        QTRY_VERIFY_WITH_TIMEOUT(!QFile::exists(at(1, "docs/x.txt")), 10000);

        QTRY_COMPARE_WITH_TIMEOUT(n.nodes[1]->svc->keptFiles(kFolder).size(), 1, 5000);
        const QVariantMap kept = n.nodes[1]->svc->keptFiles(kFolder).first().toMap();
        QCOMPARE(kept.value("path").toString(), QStringLiteral("docs/x.txt"));
        QCOMPARE(kept.value("size").toLongLong(), qint64(5));
        QVERIFY(kept.value("kept").toString().startsWith("docs/x~"));
        QVERIFY(kept.value("time").toLongLong() > 0);
        QCOMPARE(n.nodes[1]->svc->fileInfo(kFolder, "docs/x.txt").value("kept").toList().size(), 1);

        // Restored, it is bob's change: alice gets it back.
        QVERIFY(n.nodes[1]->svc->restoreKept(kFolder, kept.value("kept").toString()));
        QCOMPARE(contents(at(1, "docs/x.txt")), QByteArray("first"));
        QTRY_COMPARE_WITH_TIMEOUT(contents(at(0, "docs/x.txt")), QByteArray("first"), 15000);
        QCOMPARE(n.nodes[1]->svc->keptFiles(kFolder).size(), 0);
        QVERIFY(!n.nodes[1]->svc->restoreKept(kFolder, "../y.txt"));  // never out of .synctrash
        QCOMPARE(contents(at(1, "y.txt")), QByteArray("why"));

        // Discarding one.
        QTest::qWait(300);
        QVERIFY(QFile::remove(at(0, "y.txt")));
        QTRY_COMPARE_WITH_TIMEOUT(n.nodes[1]->svc->keptFiles(kFolder).size(), 1, 10000);
        QVERIFY(n.nodes[1]->svc->discardKept(kFolder, n.nodes[1]->svc->keptFiles(kFolder).first().toMap().value("kept").toString()));
        QCOMPARE(n.nodes[1]->svc->keptFiles(kFolder).size(), 0);
    }

    void previewsAndSavesRules() {
        Net& n = *net_;
        n.add("alice");
        n.add("bob");
        write(at(0, "a.txt"), "a");
        write(at(0, "b.log"), "b");
        write(at(0, "docs/c.txt"), "c");
        write(at(0, "docs/d.log"), "d");
        n.join(0);
        n.join(1);
        n.link(0, 1);
        QTRY_COMPARE_WITH_TIMEOUT(contents(at(1, "docs/d.log")), QByteArray("d"), 10000);

        const QVariantMap preview = n.nodes[0]->svc->previewRules(kFolder, false, "*.log\n!\n/docs/\n", "docs/new.txt");
        QCOMPARE(preview.value("problems").toList().size(), 1);
        QCOMPARE(preview.value("problems").toList().first().toMap().value("line").toInt(), 2);
        QCOMPARE(preview.value("stop").toLongLong(), qint64(3));  // b.log, docs/c.txt, docs/d.log
        QCOMPARE(preview.value("resume").toLongLong(), qint64(0));
        QHash<QString, QVariantMap> matches;
        for (const QVariant& v : preview.value("matches").toList()) matches.insert(v.toMap().value("path").toString(), v.toMap());
        QCOMPARE(matches.value("b.log").value("line").toInt(), 1);
        QCOMPARE(matches.value("docs").value("line").toInt(), 3);
        QCOMPARE(matches.value("docs/c.txt").value("via").toString(), QStringLiteral("docs"));
        QVERIFY(!matches.contains("a.txt"));
        const QVariantMap probe = preview.value("probe").toMap();
        QCOMPARE(probe.value("line").toInt(), 3);
        QVERIFY(probe.value("excluded").toBool());

        QVERIFY(n.nodes[0]->svc->saveRules(kFolder, false, "*.log\n"));
        QCOMPARE(n.nodes[0]->svc->rulesText(kFolder, false), QStringLiteral("*.log\n"));
        QTRY_COMPARE_WITH_TIMEOUT(contents(at(1, ".syncignore")), QByteArray("*.log\n"), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(statesOf(*n.nodes[0]).value("b.log"), QStringLiteral("ignored"), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(statesOf(*n.nodes[1]).value("docs/d.log"), QStringLiteral("ignored"), 5000);
        QVERIFY(QFile::exists(at(1, "docs/d.log")));  // ignoring never deletes
        // Left out now, it would come back without the rule.
        QCOMPARE(n.nodes[0]->svc->previewRules(kFolder, false, "").value("resume").toLongLong(), qint64(2));

        // "Skip on this device" for one file.
        QVERIFY(n.nodes[1]->svc->addRule(kFolder, true, "docs/c.txt"));
        QCOMPARE(n.nodes[1]->svc->rulesText(kFolder, true), QStringLiteral("/docs/c.txt\n"));
        QTRY_COMPARE_WITH_TIMEOUT(statesOf(*n.nodes[1]).value("docs/c.txt"), QStringLiteral("skipped"), 5000);
        QCOMPARE(rowOf(*n.nodes[1], "docs/c.txt").value("detail").toString(), QStringLiteral("/docs/c.txt"));
        QTest::qWait(600);
        QVERIFY(!QFile::exists(at(0, ".syncskip")));
        QCOMPARE(statesOf(*n.nodes[0]).value("docs/c.txt"), QStringLiteral("synced"));
        // Alice sees that bob holds one path back.
        QTRY_COMPARE_WITH_TIMEOUT(memberOf(*n.nodes[0], n.nodes[1]->key).value("skipped").toLongLong(), qint64(1), 5000);
    }

    void pausesOneDevice() {
        Net& n = *net_;
        n.add("alice");
        n.add("bob");
        write(at(0, "a.txt"), "a");
        n.join(0);
        n.join(1);
        n.link(0, 1);
        QTRY_COMPARE_WITH_TIMEOUT(contents(at(1, "a.txt")), QByteArray("a"), 10000);

        FolderConfig c = n.config(0);
        c.peers[n.nodes[1]->key].paused = true;
        n.nodes[0]->svc->setFolders({c});
        QTRY_VERIFY_WITH_TIMEOUT(memberOf(*n.nodes[0], n.nodes[1]->key).value("paused").toBool(), 5000);
        // Bob, no longer offered the folder, forgets alice's index of it.
        QTRY_VERIFY_WITH_TIMEOUT(n.nodes[1]->svc->fileInfo(kFolder, "a.txt").value("holders").toList().isEmpty(), 5000);
        write(at(0, "b.txt"), "b");
        write(at(1, "c.txt"), "c");
        QTRY_COMPARE_WITH_TIMEOUT(statesOf(*n.nodes[0]).value("b.txt"), QStringLiteral("synced"), 5000);  // nobody to send to
        QTest::qWait(1500);
        QVERIFY(!QFile::exists(at(1, "b.txt")));
        QVERIFY(!QFile::exists(at(0, "c.txt")));

        c.peers[n.nodes[1]->key].paused = false;
        n.nodes[0]->svc->setFolders({c});
        QTRY_COMPARE_WITH_TIMEOUT(contents(at(1, "b.txt")), QByteArray("b"), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(contents(at(0, "c.txt")), QByteArray("c"), 10000);
    }

    void offersFollowThePermission() {
        Net& n = *net_;
        n.add("alice");
        n.add("bob");
        n.join(0);  // alice shares; bob has not joined
        n.noOffers.insert({0, 1});
        n.link(0, 1);
        QTest::qWait(400);
        QCOMPARE(n.nodes[1]->offered.value(n.nodes[0]->key, 0), 0);  // not offered without the permission
        // Granted: alice tells bob about her folder when told of him again.
        n.noOffers.erase({0, 1});
        n.nodes[0]->svc->peerUp(n.nodes[1]->dev);
        QTRY_COMPARE(n.nodes[1]->offered.value(n.nodes[0]->key, 0), 1);
        // Taken away again: the offers it gets are empty, so it forgets them.
        n.noOffers.insert({0, 1});
        n.nodes[0]->svc->peerUp(n.nodes[1]->dev);
        QTRY_COMPARE(n.nodes[1]->offered.value(n.nodes[0]->key, 1), 0);
    }

    void listsTransfers() {
        Net& n = *net_;
        n.add("alice");
        n.add("bob");
        const QByteArray big = randomBytes(6 * 1024 * 1024, 3);
        write(at(0, "big.bin"), big);
        n.join(0);
        n.join(1);
        n.link(0, 1);
        bool seen = false;
        for (int i = 0; i < 400 && !seen; ++i) {
            QTest::qWait(10);
            for (const QVariant& v : n.nodes[1]->svc->transfers()) {
                const QVariantMap t = v.toMap();
                if (t.value("path").toString() != "big.bin" || !t.value("active").toBool()) continue;
                seen = true;
                QCOMPARE(t.value("folder").toString(), kFolder);
                QCOMPARE(t.value("size").toLongLong(), qint64(big.size()));
                QCOMPARE(t.value("key").toString(), n.nodes[0]->key);
                QVERIFY(t.value("done").toLongLong() <= big.size());
            }
        }
        QVERIFY(seen);
        QTRY_COMPARE_WITH_TIMEOUT(contents(at(1, "big.bin")), big, 20000);
        QTRY_VERIFY_WITH_TIMEOUT(n.nodes[1]->svc->transfers().isEmpty(), 5000);
        QVERIFY(memberOf(*n.nodes[1], n.nodes[0]->key).value("rate").toLongLong() >= 0);
    }
};

QTEST_GUILESS_MAIN(SyncServiceTest)
#include "SyncServiceTest.moc"
