// The synced-folder views against a fake App: the window builds from a
// folder's status, the file tree lists what sync reports with its states,
// filters and search narrow it, the rules editor asks for a preview as the
// text changes and saves through App, and the devices view says where each
// member is at.
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
#include <QVariantList>

class SyncApp : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList syncFolders READ syncFolders NOTIFY syncChanged)
    Q_PROPERTY(QVariantList syncOffers READ syncOffers NOTIFY syncChanged)
    Q_PROPERTY(QVariantList syncEvents READ syncEvents NOTIFY syncChanged)
    Q_PROPERTY(QVariantMap syncTransfers READ syncTransfers NOTIFY syncChanged)
    Q_PROPERTY(QString colorScheme READ colorScheme CONSTANT)
public:
    QVariantList folders, offers, events, files, kept;
    QVariantMap  transfers, info, preview;
    QString      rules;
    // What the views asked for.
    QStringList  calls;
    QString      savedText;
    QString      lastProbe;
    QString      lastPreviewText;

    QVariantList syncFolders() const { return folders; }
    QVariantList syncOffers() const { return offers; }
    QVariantList syncEvents() const { return events; }
    QVariantMap  syncTransfers() const { return transfers; }
    QString      colorScheme() const { return QStringLiteral("dark"); }

    Q_INVOKABLE void loadSyncFiles(const QString& id) {
        calls << "files:" + id;
        QTimer::singleShot(0, this, [this, id] { emit syncFilesLoaded(id, files); });
    }
    Q_INVOKABLE void loadSyncFileInfo(const QString& id, const QString& path) {
        calls << "info:" + path;
        QTimer::singleShot(0, this, [this, id, path] { emit syncFileInfoLoaded(id, path, info); });
    }
    Q_INVOKABLE void loadSyncRules(const QString& id, bool skipFile) {
        calls << QStringLiteral("rules:%1").arg(skipFile);
        QTimer::singleShot(0, this, [this, id, skipFile] { emit syncRulesLoaded(id, skipFile, rules); });
    }
    Q_INVOKABLE void previewSyncRules(const QString& id, bool skipFile, const QString& text, const QString& probe) {
        calls << QStringLiteral("preview:%1").arg(skipFile);
        lastPreviewText = text;
        lastProbe       = probe;
        QTimer::singleShot(0, this, [this, id, skipFile] { emit syncRulesPreviewed(id, skipFile, preview); });
    }
    Q_INVOKABLE void saveSyncRules(const QString&, bool, const QString& text) {
        calls << "save";
        savedText = text;
    }
    Q_INVOKABLE void addSyncRule(const QString&, bool skipFile, const QString& path) {
        calls << QStringLiteral("addRule:%1:%2").arg(skipFile).arg(path);
    }
    Q_INVOKABLE void loadSyncKept(const QString& id) {
        calls << "kept";
        QTimer::singleShot(0, this, [this, id] { emit syncKeptLoaded(id, kept); });
    }
    Q_INVOKABLE void restoreSyncKept(const QString&, const QString& k) { calls << "restore:" + k; }
    Q_INVOKABLE void discardSyncKept(const QString&, const QString& k) { calls << "discard:" + k; }
    Q_INVOKABLE void confirmSyncDeletes(const QString&, bool apply) { calls << QStringLiteral("deletes:%1").arg(apply); }
    Q_INVOKABLE void setSyncMemberPaused(const QString&, const QString& key, bool paused) {
        calls << QStringLiteral("pause:%1:%2").arg(key).arg(paused);
    }
    Q_INVOKABLE void setSyncMember(const QString&, const QString& key, bool r, bool c) {
        calls << QStringLiteral("member:%1:%2:%3").arg(key).arg(r).arg(c);
    }
    Q_INVOKABLE void setSyncPaused(const QString&, bool) { calls << "folderPause"; }
    Q_INVOKABLE void rescanSyncFolder(const QString&) { calls << "rescan"; }
    Q_INVOKABLE void openSyncFolder(const QString&) { calls << "open"; }
    Q_INVOKABLE void openSyncKept(const QString&) { calls << "openKept"; }
    Q_INVOKABLE void openSyncPath(const QString&, const QString& p) { calls << "openPath:" + p; }
    Q_INVOKABLE void showSyncPath(const QString&, const QString& p) { calls << "showPath:" + p; }
    Q_INVOKABLE void resolveSyncConflict(const QString&, const QString& p, const QString& choice, const QString&) {
        calls << "resolve:" + p + ":" + choice;
    }
    Q_INVOKABLE void clearSyncEvents() { calls << "clear"; }
    Q_INVOKABLE void leaveSyncFolder(const QString&) { calls << "leave"; }
    Q_INVOKABLE QString createSyncFolder(const QString&, const QString&) { return {}; }
    Q_INVOKABLE bool    joinSyncFolder(const QString&, const QString&, const QString&, const QUrl&) { return false; }
    Q_INVOKABLE QString syncMemberName(const QString& id) const { return "member " + id.left(4); }

signals:
    void syncChanged();
    void syncFilesLoaded(const QString& id, const QVariantList& files);
    void syncFileInfoLoaded(const QString& id, const QString& path, const QVariantMap& info);
    void syncRulesLoaded(const QString& id, bool skipFile, const QString& text);
    void syncRulesPreviewed(const QString& id, bool skipFile, const QVariantMap& preview);
    void syncKeptLoaded(const QString& id, const QVariantList& kept);
    void notify(const QString& message);
};

namespace {

const QString kFolder = QStringLiteral("0123456789abcdef0123456789abcdef");
const QString kBob    = QString(64, QLatin1Char('b'));

QVariantMap file(const QString& path, bool dir, const QString& state, const QString& detail = {}, qint64 size = 10,
                 double progress = -1) {
    return {{"path", path}, {"name", path.section('/', -1)}, {"dir", dir}, {"size", size}, {"mtime", 1700000000000ll},
            {"state", state}, {"detail", detail}, {"progress", progress}, {"version", QVariantMap{{"aaaa", 2}}}};
}

QVariantMap member(const QString& key, const QString& name, bool online) {
    return {{"key", key},          {"name", name},          {"fingerprint", "AB12 CD34"}, {"receive", true},
            {"contribute", true},  {"paused", false},       {"special", true},            {"online", online},
            {"toSendFiles", 2},    {"toSendBytes", 2048},   {"toSendChanges", 0},         {"toReceiveFiles", 1},
            {"toReceiveBytes", 4096}, {"toReceiveChanges", 1}, {"current", "docs/big.bin"}, {"sending", false},
            {"rate", 1024},        {"sendRate", 0},         {"eta", 4},                   {"skipped", 3},
            {"lastSeen", 1700000000000ll}, {"lastInSync", 1690000000000ll}};
}

}  // namespace

class SyncViewTest : public QObject {
    Q_OBJECT
    SyncApp                       app_;
    QTemporaryDir                 source_;
    std::unique_ptr<QQmlEngine>   engine_;
    std::unique_ptr<QQuickWindow> window_;
    QQuickItem*                   item_ = nullptr;

    QQuickItem* create(const QString& name, const QVariantMap& properties) {
        QQmlComponent component(engine_.get(), QUrl::fromLocalFile(source_.path() + "/" + name));
        if (!component.isReady()) qWarning() << component.errorString();
        QQuickItem* item = qobject_cast<QQuickItem*>(component.createWithInitialProperties(properties));
        if (!item) qWarning() << component.errorString();
        return item;
    }
    QQuickItem* listNamed(const QString& objectName) {
        return item_ ? item_->findChild<QQuickItem*>(objectName) : nullptr;
    }
    int rowCount(const QString& objectName) {
        QQuickItem* list = listNamed(objectName);
        return list ? list->property("count").toInt() : -1;
    }
    QStringList rowPaths() {
        QStringList out;
        for (const QVariant& v : item_->property("rows").toList()) out << v.toMap().value("path").toString();
        return out;
    }

private slots:
    void initTestCase() {
        QVERIFY(source_.isValid());
        const QDir qml(QStringLiteral(UCD_QML_DIR));
        QByteArray module;
        for (const QString& name : qml.entryList({"*.qml"}, QDir::Files)) {
            QVERIFY(QFile::copy(qml.filePath(name), QDir(source_.path()).filePath(name)));
            const QByteArray type = QFileInfo(name).completeBaseName().toUtf8();
            module += (type == "Theme" ? "singleton " : "") + type + " 1.0 " + name.toUtf8() + '\n';
        }
        QFile qmldir(QDir(source_.path()).filePath("qmldir"));
        QVERIFY(qmldir.open(QIODevice::WriteOnly));
        QCOMPARE(qmldir.write(module), module.size());
        qmldir.close();
        qmlRegisterSingletonInstance("UConnect", 1, 0, "App", &app_);
        engine_ = std::make_unique<QQmlEngine>();
    }

    void init() {
        app_.calls.clear();
        app_.files = {file("docs", true, "synced"),
                      file("docs/plan.md", false, "send", "1"),
                      file("docs/big.bin", false, "transferring", "", 100000, 0.4),
                      file("notes.txt", false, "synced"),
                      file("new.txt", false, "receive", "waiting"),
                      file("build", true, "ignored", "/build"),
                      file("mine.txt", false, "skipped", "mine.txt"),
                      file("CON.txt", false, "held", "name:reserved"),
                      file("clash.txt", false, "conflict")};
        app_.rules   = QStringLiteral("*.log\n");
        app_.preview = {{"problems", QVariantList{QVariantMap{{"line", 2}, {"pattern", "!"}}}},
                        {"matches", QVariantList{QVariantMap{{"path", "a.log"}, {"dir", false}, {"line", 1},
                                                             {"pattern", "*.log"}, {"negate", false}, {"via", ""}}}},
                        {"stop", 3},
                        {"resume", 0},
                        {"probe", QVariantMap{{"path", "x.log"}, {"line", 1}, {"pattern", "*.log"}, {"negate", false},
                                              {"via", ""}, {"excluded", true}}}};
        app_.kept = {QVariantMap{{"kept", "docs/x~20260101-010101.txt"}, {"path", "docs/x.txt"},
                                 {"time", 1700000000000ll}, {"size", 5}}};
        app_.folders = {QVariantMap{
            {"id", kFolder}, {"label", "Shared"}, {"path", "C:/shared"}, {"paused", false}, {"state", "syncing"},
            {"files", 8}, {"bytes", 123456}, {"dirs", 2}, {"free", 5000000000ll}, {"needFiles", 1}, {"needBytes", 60000},
            {"conflicts", QVariantList{QVariantMap{
                {"path", "clash.txt"}, {"involved", true}, {"following", ""}, {"followingName", ""}, {"waiting", false},
                {"candidates", QVariantList{
                    QVariantMap{{"key", ""}, {"mine", true}, {"name", "This device"}, {"size", 10}, {"mtime", 1700000000000ll}, {"deleted", false}, {"dir", false}},
                    QVariantMap{{"key", kBob}, {"mine", false}, {"name", "Bob"}, {"size", 20}, {"mtime", 1700000001000ll}, {"deleted", false}, {"dir", false}}}}}}},
            {"blocked", QVariantMap{}}, {"held", QVariantList{}},
            {"members", QVariantList{member(kBob, "Bob", true)}},
            {"pendingDeletes", QVariantList{"a.txt", "b.txt"}}, {"pendingDeleteCount", 2},
            {"transfers", QVariantList{QVariantMap{{"path", "docs/big.bin"}, {"size", 100000}, {"done", 40000}, {"key", kBob}, {"device", "Bob"}}}}}};
        app_.transfers = {{"active", QVariantList{QVariantMap{{"folder", kFolder}, {"label", "Shared"}, {"path", "docs/big.bin"},
                                                               {"size", 100000}, {"done", 40000}, {"key", kBob}, {"device", "Bob"}}}},
                          {"queued", 1}, {"rate", 2048}};
        app_.events = {QVariantMap{{"time", "2026-10-28T10:00:00"}, {"folderId", kFolder}, {"folder", "Shared"},
                                   {"path", "clash.txt"}, {"kind", "conflict"}, {"text", "Your change conflicts."}}};
        window_ = std::make_unique<QQuickWindow>();
        window_->resize(1000, 680);
        window_->show();
    }

    void cleanup() {
        delete item_;
        item_ = nullptr;
        window_.reset();
    }

    void cleanupTestCase() { engine_.reset(); }

    void filesViewShowsTheTreeWithStates() {
        item_ = create("SyncFilesView.qml", {{"folderId", kFolder}, {"width", 900}, {"height", 600}});
        QVERIFY(item_);
        item_->setParentItem(window_->contentItem());
        QTRY_VERIFY(!item_->property("loading").toBool());
        QVERIFY(app_.calls.contains("files:" + kFolder));
        // The tree: folders first, closed.
        QCOMPARE(rowPaths(), (QStringList{"build", "docs", "clash.txt", "CON.txt", "mine.txt", "new.txt", "notes.txt"}));
        QCOMPARE(rowCount("syncFileList"), 7);
        QMetaObject::invokeMethod(item_, "toggle", Q_ARG(QVariant, "docs"));
        QCOMPARE(rowPaths(), (QStringList{"build", "docs", "docs/big.bin", "docs/plan.md", "clash.txt", "CON.txt",
                                          "mine.txt", "new.txt", "notes.txt"}));
        // Filters list matches by full path.
        item_->setProperty("filterIndex", 1);  // pending
        QCOMPARE(rowPaths(), (QStringList{"docs/plan.md", "docs/big.bin", "new.txt", "clash.txt"}));
        item_->setProperty("filterIndex", 2);  // problems
        QCOMPARE(rowPaths(), (QStringList{"new.txt", "CON.txt", "clash.txt"}));
        item_->setProperty("filterIndex", 3);  // left out
        QCOMPARE(rowPaths(), (QStringList{"build", "mine.txt"}));
        item_->setProperty("filterIndex", 0);
        item_->setProperty("query", "plan");
        QCOMPARE(rowPaths(), (QStringList{"docs/plan.md"}));
        // The state texts.
        QVariant text;
        QMetaObject::invokeMethod(item_, "stateLabel", Q_RETURN_ARG(QVariant, text), Q_ARG(QVariant, app_.files[2]));
        QCOMPARE(text.toString(), QStringLiteral("Receiving, 40%"));
        QMetaObject::invokeMethod(item_, "stateLabel", Q_RETURN_ARG(QVariant, text), Q_ARG(QVariant, app_.files[4]));
        QCOMPARE(text.toString(), QStringLiteral("Waiting for a device that has it"));
        QMetaObject::invokeMethod(item_, "stateLabel", Q_RETURN_ARG(QVariant, text), Q_ARG(QVariant, app_.files[5]));
        QCOMPARE(text.toString(), QStringLiteral("Ignored everywhere (/build)"));
        // History asks App for the path in depth.
        QMetaObject::invokeMethod(item_, "showHistory", Q_ARG(QVariant, "docs/plan.md"));
        QTRY_VERIFY(app_.calls.contains("info:docs/plan.md"));
        QCOMPARE(item_->property("infoPath").toString(), QStringLiteral("docs/plan.md"));
    }

    void rulesViewPreviewsAndSaves() {
        item_ = create("SyncRulesView.qml", {{"folderId", kFolder}, {"width", 900}, {"height", 600}});
        QVERIFY(item_);
        item_->setParentItem(window_->contentItem());
        QTRY_VERIFY(item_->property("loaded").toBool());
        QCOMPARE(app_.calls.first(), QStringLiteral("rules:0"));
        QTRY_VERIFY(app_.calls.contains("preview:0"));
        QVERIFY(!item_->property("modified").toBool());
        QTRY_COMPARE(item_->property("stop").toInt(), 3);
        QCOMPARE(item_->property("problems").toList().size(), 1);
        QCOMPARE(item_->property("matches").toList().size(), 1);

        QQuickItem* editor = item_->findChild<QQuickItem*>("syncRulesEditor");
        QVERIFY(editor);
        editor->setProperty("text", "*.log\n*.tmp\n");
        QVERIFY(item_->property("modified").toBool());
        QTRY_COMPARE_WITH_TIMEOUT(app_.lastPreviewText, QStringLiteral("*.log\n*.tmp\n"), 3000);  // after the debounce
        QMetaObject::invokeMethod(item_, "save");
        QCOMPARE(app_.savedText, QStringLiteral("*.log\n*.tmp\n"));
        QVERIFY(!item_->property("modified").toBool());

        // The other file loads on its own.
        item_->setProperty("skipFile", true);
        QTRY_VERIFY(app_.calls.contains("rules:1"));
    }

    void devicesViewSaysWhereEachMemberIs() {
        item_ = create("SyncDevicesView.qml", {{"folderId", kFolder}, {"members", app_.folders[0].toMap()["members"]},
                                               {"width", 900}});
        QVERIFY(item_);
        item_->setParentItem(window_->contentItem());
        QVariant text;
        QMetaObject::invokeMethod(item_, "leftText", Q_RETURN_ARG(QVariant, text), Q_ARG(QVariant, member(kBob, "Bob", true)));
        QVERIFY2(text.toString().contains("To send: 2 file(s), 2.0 KB"), qPrintable(text.toString()));
        QVERIFY2(text.toString().contains("To receive: 1 file(s), 4.0 KB and 1 other change(s)"), qPrintable(text.toString()));
        QVERIFY2(text.toString().contains("1.0 KB/s, 4 s left"), qPrintable(text.toString()));
        QMetaObject::invokeMethod(item_, "currentText", Q_RETURN_ARG(QVariant, text), Q_ARG(QVariant, member(kBob, "Bob", true)));
        QCOMPARE(text.toString(), QStringLiteral("Receiving docs/big.bin"));
        QVariantMap done = member(kBob, "Bob", false);
        done["toSendFiles"] = 0;
        done["toReceiveFiles"] = 0;
        done["toReceiveChanges"] = 0;
        done["toSendBytes"] = 0;
        done["toReceiveBytes"] = 0;
        QMetaObject::invokeMethod(item_, "leftText", Q_RETURN_ARG(QVariant, text), Q_ARG(QVariant, done));
        QVERIFY2(text.toString().startsWith("Nothing left as far as this device knows"), qPrintable(text.toString()));
        QVERIFY2(text.toString().contains("last fully in sync"), qPrintable(text.toString()));
        done["paused"] = true;
        QMetaObject::invokeMethod(item_, "leftText", Q_RETURN_ARG(QVariant, text), Q_ARG(QVariant, done));
        QVERIFY(text.toString().startsWith("Paused"));
    }

    void keptViewListsAndRestores() {
        item_ = create("SyncKeptView.qml", {{"folderId", kFolder}, {"width", 900}, {"height", 500}});
        QVERIFY(item_);
        item_->setParentItem(window_->contentItem());
        QTRY_COMPARE(rowCount("syncKeptList"), 1);
        QCOMPARE(item_->property("kept").toList().first().toMap().value("path").toString(), QStringLiteral("docs/x.txt"));
    }

    void windowBuildsFromTheStatus() {
        QQmlComponent component(engine_.get(), QUrl::fromLocalFile(source_.path() + "/SyncWindow.qml"));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        std::unique_ptr<QObject> object(component.create());
        QVERIFY2(object, qPrintable(component.errorString()));
        auto* win = qobject_cast<QQuickWindow*>(object.get());
        QVERIFY(win);
        win->show();
        QCOMPARE(win->property("currentId").toString(), kFolder);
        QVariant text;
        QMetaObject::invokeMethod(win, "stateText", Q_RETURN_ARG(QVariant, text), Q_ARG(QVariant, app_.folders[0]));
        QCOMPARE(text.toString(), QStringLiteral("Fetching 1 file(s), 58.6 KB · 1 conflict(s) · 2 delete(s) waiting for you"));
        QMetaObject::invokeMethod(win, "summaryText", Q_RETURN_ARG(QVariant, text), Q_ARG(QVariant, app_.folders[0]));
        QCOMPARE(text.toString(), QStringLiteral("8 file(s) in 2 folder(s), 120.6 KB · 4.7 GB free on this disk"));
        // Every view builds; with UCD_SYNC_SHOTS set, each is saved as a picture there.
        const QString shots = qEnvironmentVariable("UCD_SYNC_SHOTS");
        for (int view = 0; view < 5; ++view) {
            win->setProperty("view", view);
            QCoreApplication::processEvents();
            if (shots.isEmpty()) continue;
            QTest::qWait(400);
            win->grabWindow().save(QDir(shots).filePath(QStringLiteral("sync-view-%1.png").arg(view)));
        }
        QTRY_VERIFY(app_.calls.contains("files:" + kFolder));
        QTRY_VERIFY(app_.calls.contains("rules:0"));
        QTRY_VERIFY(app_.calls.contains("kept"));
        QCOMPARE(win->property("transfers").toMap().value("queued").toInt(), 1);
        win->close();
    }
};

QTEST_MAIN(SyncViewTest)
#include "SyncViewTest.moc"
