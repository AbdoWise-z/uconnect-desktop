// Exercise native URL drops against the actual storage view, including its
// permission gates and the QML-to-C++ conversion of a list of file URLs.
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QGuiApplication>
#include <QMimeData>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
#include <QVariantList>

#include "app/PeerListModel.hpp"
#include "app/TransferListModel.hpp"
#include "core/Permissions.hpp"

class StorageApp : public QObject {
    Q_OBJECT
    Q_PROPERTY(QObject* peers READ peers CONSTANT)
    Q_PROPERTY(QObject* transfers READ transfers CONSTANT)
    Q_PROPERTY(QString downloadFolder READ downloadFolder CONSTANT)
public:
    ucd::PeerListModel peers_;
    ucd::TransferListModel transfers_;
    QList<QUrl> uploaded;
    QString destination;
    bool replaced = false;
    quint32 requestId = 0;
    QVariantList listing;  // what every storageList() returns
    QVariantList dragItems;
    QList<quint32> canceledBatches;
    QVariantList downloadedItems;
    QString opened;
    QString transferKind;  // "move" | "copy"
    QStringList transferred;
    QString transferredTo;
    QMap<quint32, QString> watches;  // open watches: id -> path

    QObject* peers() { return &peers_; }
    QObject* transfers() { return &transfers_; }
    QString downloadFolder() const { return QDir::tempPath(); }

    Q_INVOKABLE quint32 storageList(const QString&, const QString& path) {
        const quint32 id = ++requestId;
        QTimer::singleShot(0, this, [this, id, path] { emit storageListed(id, path, listing, {}); });
        return id;
    }
    Q_INVOKABLE int storageUpload(const QString&, const QString& folder, const QList<QUrl>& files,
                                  bool replace = false) {
        uploaded = files;
        destination = folder;
        replaced = replace;
        return static_cast<int>(files.size());
    }
    Q_INVOKABLE quint32 storageDownloadItems(const QString&, const QString&, const QVariantList& items) {
        downloadedItems = items;
        return ++requestId;
    }
    Q_INVOKABLE quint32 prepareDragOut(const QString&, const QString&, const QVariantList& items) {
        dragItems = items;
        return ++requestId;
    }
    Q_INVOKABLE quint32 storageOpen(const QString&, const QString& folder, const QString& name) {
        opened = folder + "/" + name;
        return ++requestId;
    }
    Q_INVOKABLE QVariantList storageMove(const QString&, const QStringList& paths, const QString& to) {
        return transfer("move", paths, to);
    }
    Q_INVOKABLE QVariantList storageCopy(const QString&, const QStringList& paths, const QString& to) {
        return transfer("copy", paths, to);
    }
    QVariantList transfer(const QString& kind, const QStringList& paths, const QString& to) {
        transferKind = kind;
        transferred = paths;
        transferredTo = to;
        QVariantList ids;
        for (int i = 0; i < paths.size(); ++i) ids << ++requestId;
        return ids;
    }
    Q_INVOKABLE quint32 storageWatch(const QString&, const QString& path) {
        watches.insert(++requestId, path);
        return requestId;
    }
    Q_INVOKABLE void storageUnwatch(quint32 id) { watches.remove(id); }
    Q_INVOKABLE void cancelBatch(quint32 id) { canceledBatches << id; }
    Q_INVOKABLE void storageCancel(quint32) {}
    Q_INVOKABLE void copyText(const QString&) {}
    Q_INVOKABLE void showInFolder(const QString&) {}
signals:
    void storageListed(quint32 requestId, const QString& path, const QVariantList& entries, const QString& error);
    void storageUploaded(const QString& devId, const QString& remotePath);
    void storageOperationFinished(quint32 requestId, const QString& devId, const QString& path, const QString& error);
    void storageOperationProgress(quint32 requestId, const QString& devId, qint64 done, qint64 total);
    void storageFolderChanged(quint32 watchId, const QString& path);
    void storageBatchProgress(quint32 batchId, int filesDone, int filesKnown);
    void storageBatchFinished(quint32 batchId, int failed, bool canceled, const QStringList& urls);
};

class StorageViewTest : public QObject {
    Q_OBJECT
    QTemporaryDir source_;
    StorageApp app_;
    std::unique_ptr<QQmlEngine> engine_;
    std::unique_ptr<QQuickWindow> window_;
    QQuickItem* view_ = nullptr;

    void grant(bool upload, const QStringList& more = {}) {
        ucd::PeerRecord peer;
        peer.devId = QStringLiteral("test-device");
        peer.name = QStringLiteral("Test device");
        if (upload) peer.grantedToMe = {ucd::perm::StorageUpload};
        peer.grantedToMe += more;
        app_.peers_.setPeers({peer});
    }

    static QVariantMap entry(const QString& name, bool dir = false) {
        return {{"name", name}, {"dir", dir}, {"size", 10.0}, {"mtime", 0.0}};
    }

    // Lists the given entries in the open folder and waits for them.
    void show(const QVariantList& entries) {
        app_.listing = entries;
        QVERIFY(QMetaObject::invokeMethod(view_, "load"));
        QTRY_VERIFY(!view_->property("loading").toBool());
    }

    QQuickItem* list() const { return view_->findChild<QQuickItem*>("storageFileList"); }

    // A point on row `i`, `x` pixels from the list's left edge.
    QPoint rowPoint(int i, qreal x = 300) const {
        return list()->mapToScene(QPointF(x, 13 + 26 * i)).toPoint();
    }

    void click(int i, Qt::KeyboardModifiers modifiers = Qt::NoModifier, qreal x = 300) {
        QTest::mouseClick(window_.get(), Qt::LeftButton, modifiers, rowPoint(i, x));
    }

    QStringList selectedNames() const {
        QVariant result;
        QMetaObject::invokeMethod(view_, "selectedItems", Q_RETURN_ARG(QVariant, result));
        QStringList names;
        for (const QVariant& item : result.toList()) names << item.toMap().value("name").toString();
        return names;
    }

    bool dropAt(const QList<QUrl>& urls, const QPoint& at) {
        QMimeData mime;
        mime.setUrls(urls);
        QDragEnterEvent enter(at, Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(window_.get(), &enter);
        QDragMoveEvent move(at, Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(window_.get(), &move);
        QDropEvent event(QPointF(at), Qt::CopyAction | Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(window_.get(), &event);
        return event.isAccepted() && event.dropAction() == Qt::CopyAction;
    }

    bool drop(const QList<QUrl>& urls) {
        QMimeData mime;
        mime.setUrls(urls);
        QDragEnterEvent enter(QPoint(300, 150), Qt::CopyAction | Qt::MoveAction, &mime,
                              Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(window_.get(), &enter);
        QDropEvent event(QPointF(300, 150), Qt::CopyAction | Qt::MoveAction, &mime,
                         Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(window_.get(), &event);
        return event.isAccepted() && event.dropAction() == Qt::CopyAction;
    }

private slots:
    void initTestCase() {
        QVERIFY(source_.isValid());
        // Local QML imports use the same components as the app, with Theme
        // declared as a singleton just as in qt_add_qml_module.
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
        app_.uploaded.clear();
        app_.destination.clear();
        app_.replaced = false;
        app_.listing.clear();
        app_.dragItems.clear();
        app_.canceledBatches.clear();
        app_.downloadedItems.clear();
        app_.opened.clear();
        app_.transferKind.clear();
        app_.transferred.clear();
        app_.transferredTo.clear();
        app_.watches.clear();
        grant(true);
        window_ = std::make_unique<QQuickWindow>();
        window_->resize(1000, 680);
        window_->setColor(QColor("#1E1F22"));
        QQmlComponent component(engine_.get(), QUrl::fromLocalFile(source_.path() + "/StorageView.qml"));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        view_ = qobject_cast<QQuickItem*>(component.createWithInitialProperties({
            {"devId", "test-device"}, {"deviceName", "Test device"}, {"connected", true},
            {"path", "/share/sub"}, {"width", 1000}, {"height", 680}
        }));
        QVERIFY2(view_, qPrintable(component.errorString()));
        view_->setParentItem(window_->contentItem());
        window_->show();
        QTRY_VERIFY(!view_->property("loading").toBool());
    }

    void cleanup() {
        delete view_;
        view_ = nullptr;
        window_.reset();
    }

    void cleanupTestCase() {
        engine_.reset();
    }

    void dropsMultipleFilesAsCopies() {
        const QList<QUrl> files{QUrl::fromLocalFile("C:/tmp/report with spaces.pdf"),
                               QUrl::fromLocalFile("C:/tmp/photo.png")};
        QVERIFY(view_->property("canUpload").toBool());
        QVERIFY(drop(files));
        QCOMPARE(app_.uploaded, files);
        QCOMPARE(app_.destination, QStringLiteral("/share/sub"));
        QVERIFY(view_->property("transfersOpen").toBool());
    }

    void rejectsDropsWithoutPermission() {
        grant(false);
        QTRY_VERIFY(!view_->property("canUpload").toBool());
        QVERIFY(!drop({QUrl::fromLocalFile("C:/tmp/file.txt")}));
        QVERIFY(app_.uploaded.isEmpty());
    }

    void rejectsDropsAtRootOrWhenDisconnected() {
        view_->setProperty("path", "/");
        QVERIFY(!drop({QUrl::fromLocalFile("C:/tmp/file.txt")}));
        view_->setProperty("path", "/share/sub");
        view_->setProperty("connected", false);
        QVERIFY(!drop({QUrl::fromLocalFile("C:/tmp/file.txt")}));
        QVERIFY(app_.uploaded.isEmpty());
    }

    void selectsWithClicksModifiersAndCheckboxes() {
        show({entry("a.txt"), entry("b.txt"), entry("c.txt"), entry("d.txt")});
        click(1);
        QCOMPARE(selectedNames(), QStringList{"b.txt"});
        click(3, Qt::ShiftModifier);  // a range from the one clicked last
        QCOMPARE(selectedNames(), (QStringList{"b.txt", "c.txt", "d.txt"}));
        click(3, Qt::ControlModifier);  // Ctrl+click deselects a selected one
        QCOMPARE(selectedNames(), (QStringList{"b.txt", "c.txt"}));
        click(3, Qt::ControlModifier);  // and adds an unselected one
        QCOMPARE(selectedNames(), (QStringList{"b.txt", "c.txt", "d.txt"}));
        click(0, Qt::NoModifier, 10);  // the checkbox adds without clearing
        QCOMPARE(selectedNames(), (QStringList{"a.txt", "b.txt", "c.txt", "d.txt"}));
        click(0, Qt::NoModifier, 10);
        QCOMPARE(selectedNames(), (QStringList{"b.txt", "c.txt", "d.txt"}));
        QCOMPARE(view_->property("selectedCount").toInt(), 3);
        click(2);  // a plain click on a selected one keeps just that one
        QCOMPARE(selectedNames(), QStringList{"c.txt"});
    }

    // Pressing on an unselected row and moving over others selects them all.
    void selectsBySweepingAcrossRows() {
        show({entry("a.txt"), entry("b.txt"), entry("c.txt"), entry("d.txt")});
        QTest::mousePress(window_.get(), Qt::LeftButton, Qt::NoModifier, rowPoint(0));
        for (int y = 0; y <= 52; y += 13) QTest::mouseMove(window_.get(), rowPoint(0) + QPoint(0, y));
        QTest::mouseRelease(window_.get(), Qt::LeftButton, Qt::NoModifier, rowPoint(2));
        QCOMPARE(selectedNames(), (QStringList{"a.txt", "b.txt", "c.txt"}));

        // Sweeping back up shrinks the range to what is under the pointer.
        QTest::mousePress(window_.get(), Qt::LeftButton, Qt::NoModifier, rowPoint(3));
        QTest::mouseMove(window_.get(), rowPoint(2));
        QTest::mouseRelease(window_.get(), Qt::LeftButton, Qt::NoModifier, rowPoint(2));
        QCOMPARE(selectedNames(), (QStringList{"c.txt", "d.txt"}));
    }

    void dropsOntoAFolderRowUploadIntoIt() {
        show({entry("photos", true), entry("notes.txt")});
        const QList<QUrl> files{QUrl::fromLocalFile("C:/tmp/a.jpg")};
        QVERIFY(dropAt(files, rowPoint(0)));
        QCOMPARE(app_.destination, QStringLiteral("/share/sub/photos"));
        QVERIFY(dropAt(files, rowPoint(1)));  // a file row: the folder shown
        QCOMPARE(app_.destination, QStringLiteral("/share/sub"));
    }

    // With Write, a clash asks whether to replace; without, it keeps both.
    // Double-clicking a file opens it (with Read); a folder is browsed into.
    void doubleClickOpensFilesAndEntersFolders() {
        show({entry("docs", true), entry("a.txt")});
        QTest::mouseDClick(window_.get(), Qt::LeftButton, Qt::NoModifier, rowPoint(1));
        QVERIFY(app_.opened.isEmpty());  // no Read permission

        grant(true, {ucd::perm::StorageRead});
        QTest::mouseDClick(window_.get(), Qt::LeftButton, Qt::NoModifier, rowPoint(1));
        QCOMPARE(app_.opened, QStringLiteral("/share/sub/a.txt"));

        QTest::mouseDClick(window_.get(), Qt::LeftButton, Qt::NoModifier, rowPoint(0));
        QCOMPARE(view_->property("path").toString(), QStringLiteral("/share/sub/docs"));
    }

    // The folder shown is watched; a change reloads it, keeping the selection.
    void refreshesWhenTheFolderChangesKeepingTheSelection() {
        show({entry("a.txt"), entry("b.txt"), entry("c.txt")});
        QCOMPARE(app_.watches.values(), QStringList({"/share/sub"}));
        click(1);
        click(2, Qt::ControlModifier);

        app_.listing = {entry("a.txt"), entry("new.txt"), entry("b.txt"), entry("c.txt")};
        emit app_.storageFolderChanged(app_.watches.firstKey(), "/share/sub");
        QTRY_COMPARE(list()->property("count").toInt(), 4);
        QCOMPARE(selectedNames(), QStringList({"b.txt", "c.txt"}));
        QCOMPARE(app_.watches.size(), 1);  // still the one watch

        // Somewhere else: the old watch ends and the new folder is watched.
        QVERIFY(QMetaObject::invokeMethod(view_, "navigate", Q_ARG(QVariant, QVariant("/share/other"))));
        QTRY_COMPARE(app_.watches.values(), QStringList({"/share/other"}));
        QVERIFY(selectedNames().isEmpty());
    }

    // Cut and paste moves; copy and paste copies; both into the open folder.
    void cutCopyAndPasteOnTheDevice() {
        grant(true, {ucd::perm::StorageWrite});
        show({entry("docs", true), entry("a.txt"), entry("b.txt")});
        click(1);
        click(2, Qt::ControlModifier);
        QTest::keyClick(window_.get(), Qt::Key_X, Qt::ControlModifier);
        QCOMPARE(view_->property("clipboard").toMap().value("mode").toString(), QStringLiteral("cut"));
        QVERIFY(QMetaObject::invokeMethod(view_, "paste"));
        QVERIFY(app_.transferred.isEmpty());  // moving into the folder it is in: nothing to do

        QVERIFY(QMetaObject::invokeMethod(view_, "navigate", Q_ARG(QVariant, QVariant("/share/sub/docs"))));
        QTRY_VERIFY(!view_->property("loading").toBool());
        QTest::keyClick(window_.get(), Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(app_.transferKind, QStringLiteral("move"));
        QCOMPARE(app_.transferred, QStringList({"/share/sub/a.txt", "/share/sub/b.txt"}));
        QCOMPARE(app_.transferredTo, QStringLiteral("/share/sub/docs"));
        QCOMPARE(view_->property("activeOpCount").toInt(), 2);
        QVERIFY(view_->property("clipboard").toMap().value("paths").toList().isEmpty());

        // Progress and completion of what was pasted.
        const quint32 first = app_.requestId - 1;
        emit app_.storageOperationProgress(first, "test-device", 50, 200);
        QCOMPARE(view_->property("opProgress").toString(), QStringLiteral("25%"));
        emit app_.storageOperationFinished(first, "test-device", "/share/sub/docs/a.txt", {});
        emit app_.storageOperationFinished(first + 1, "test-device", "/share/sub/docs/b.txt", {});
        QCOMPARE(view_->property("activeOpCount").toInt(), 0);

        // Copying needs only upload, and can paste into the same folder.
        grant(true);
        show({entry("c.txt")});
        click(0);
        QTest::keyClick(window_.get(), Qt::Key_C, Qt::ControlModifier);
        QTest::keyClick(window_.get(), Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(app_.transferKind, QStringLiteral("copy"));
        QCOMPARE(app_.transferred, QStringList({"/share/sub/docs/c.txt"}));
        QCOMPARE(app_.transferredTo, QStringLiteral("/share/sub/docs"));
    }

    // Dragging the selection onto a folder row moves it there.
    void draggingOntoAFolderMovesTheSelection() {
        grant(true, {ucd::perm::StorageWrite, ucd::perm::StorageRead});
        show({entry("docs", true), entry("a.txt"), entry("b.txt")});
        click(1);
        click(2, Qt::ControlModifier);
        QTest::mousePress(window_.get(), Qt::LeftButton, Qt::NoModifier, rowPoint(2));
        QTest::mouseMove(window_.get(), rowPoint(2) - QPoint(0, 12));
        QTest::mouseMove(window_.get(), rowPoint(0));
        QCOMPARE(view_->property("moveTarget").toInt(), 0);
        QTest::mouseRelease(window_.get(), Qt::LeftButton, Qt::NoModifier, rowPoint(0));
        QCOMPARE(app_.transferKind, QStringLiteral("move"));
        QCOMPARE(app_.transferred, QStringList({"/share/sub/a.txt", "/share/sub/b.txt"}));
        QCOMPARE(app_.transferredTo, QStringLiteral("/share/sub/docs"));
        QCOMPARE(app_.canceledBatches.size(), 1);  // the fetch for a drag-out was dropped
        QCOMPARE(view_->property("moveTarget").toInt(), -1);
    }

    void asksBeforeReplacingWhenItMayWrite() {
        const QList<QUrl> files{QUrl::fromLocalFile("C:/tmp/notes.txt")};
        show({entry("notes.txt")});
        QVERIFY(dropAt(files, rowPoint(5)));
        QCOMPARE(app_.uploaded, files);
        QVERIFY(!app_.replaced);

        app_.uploaded.clear();
        grant(true, {ucd::perm::StorageWrite});
        show({entry("notes.txt")});
        QVERIFY(dropAt(files, rowPoint(5)));
        QVERIFY(app_.uploaded.isEmpty());  // waiting for the answer
        QObject* prompt = nullptr;
        for (QObject* child : view_->findChildren<QObject*>())
            if (child->metaObject()->indexOfSignal("chosen(QString,QString)") >= 0) prompt = child;
        QVERIFY(prompt);
        QVERIFY(prompt->property("visible").toBool());
        QVERIFY(QMetaObject::invokeMethod(prompt, "choose", Q_ARG(QVariant, QVariant("replace"))));
        QCOMPARE(app_.uploaded, files);
        QVERIFY(app_.replaced);

        // A folder of the same name is merged into rather than replaced.
        app_.uploaded.clear();
        const QList<QUrl> folder{QUrl::fromLocalFile("C:/tmp/Photos")};
        show({entry("Photos", true)});
        QVERIFY(dropAt(folder, rowPoint(5)));
        QVERIFY(prompt->property("visible").toBool());
        QCOMPARE(prompt->property("title").toString(), QStringLiteral("Merge with existing folders?"));
        QVERIFY(QMetaObject::invokeMethod(prompt, "choose", Q_ARG(QVariant, QVariant("replace"))));
        QCOMPARE(app_.uploaded, folder);
        QVERIFY(app_.replaced);
    }

    // Dragging a selection out first fetches it; letting go too early cancels.
    void dragOutFetchesTheSelectionAndCancelsWhenReleasedEarly() {
        grant(true, {ucd::perm::StorageRead});
        show({entry("a.txt"), entry("folder", true), entry("c.txt")});
        click(0);
        click(1, Qt::ControlModifier);
        QTest::mousePress(window_.get(), Qt::LeftButton, Qt::NoModifier, rowPoint(1));
        QTest::mouseMove(window_.get(), rowPoint(1) + QPoint(20, 20));
        QTest::mouseMove(window_.get(), rowPoint(1) + QPoint(30, 30));
        QCOMPARE(app_.dragItems.size(), 2);
        QCOMPARE(app_.dragItems.at(0).toMap().value("name").toString(), QStringLiteral("a.txt"));
        QCOMPARE(app_.dragItems.at(1).toMap().value("dir").toBool(), true);
        QCOMPARE(view_->property("dragState").toString(), QStringLiteral("preparing"));
        QTest::mouseRelease(window_.get(), Qt::LeftButton, Qt::NoModifier, rowPoint(1) + QPoint(30, 30));
        QCOMPARE(app_.canceledBatches.size(), 1);
        QCOMPARE(view_->property("dragState").toString(), QString());
        QCOMPARE(selectedNames(), (QStringList{"a.txt", "folder"}));  // a drag keeps the selection
    }

    void transfersPopupOpensMinimizesAndMaximizes() {
        auto* popup = view_->findChild<QQuickItem*>("storageTransfers");
        QVERIFY(popup);
        view_->setProperty("transfersOpen", false);
        app_.transfers_.add(200, "other-device", "/x/elsewhere.bin");
        QVERIFY(!popup->isVisible());  // not this device's
        app_.transfers_.add(201, "test-device", "/share/sub/movie.mkv");
        app_.transfers_.progress(201, 50, 200);
        QTRY_VERIFY(popup->isVisible());
        QCOMPARE(view_->property("transfersOpen").toBool(), true);
        const qreal normal = popup->height();
        popup->setProperty("maximized", true);
        QTRY_VERIFY(popup->height() > normal);
        popup->setProperty("minimized", true);
        QTRY_VERIFY(popup->height() < normal);
        QVERIFY(popup->isVisible());
        const QImage image = window_->grabWindow();
        QVERIFY(image.save(QCoreApplication::applicationDirPath() + "/storage-popup-minimized.png"));
        popup->setProperty("minimized", false);
        popup->setProperty("maximized", false);
        QVERIFY(window_->grabWindow().save(QCoreApplication::applicationDirPath() + "/storage-popup.png"));
        app_.transfers_.finish(201, "C:/tmp/movie.mkv", {}, false);
        app_.transfers_.finish(200, "C:/tmp/elsewhere.bin", {}, false);
        app_.transfers_.clearFinished();
    }

    void refreshesOnlyTheUploadedFolder() {
        const quint32 before = app_.requestId;
        emit app_.storageUploaded("other-device", "/share/sub/file.txt");
        emit app_.storageUploaded("test-device", "/share/other/file.txt");
        QCOMPARE(app_.requestId, before);
        emit app_.storageUploaded("test-device", "/share/sub/file.txt");
        QTRY_COMPARE(app_.requestId, before + 1);
    }

    void displaysBothTransferDirections() {
        app_.transfers_.add(100, "test-device", "/share/sub/report.pdf", "C:/tmp/report.pdf", true);
        app_.transfers_.progress(100, 512, 1024);
        app_.transfers_.add(101, "test-device", "/share/sub/photo.png");
        app_.transfers_.progress(101, 2048, 2048);
        app_.transfers_.finish(101, "C:/tmp/photo.png", {}, false);
        view_->setProperty("transfersOpen", true);
        QTest::qWait(100);
        QCOMPARE(app_.transfers_.data(app_.transfers_.index(0), ucd::TransferListModel::DirectionRole).toString(),
                 QStringLiteral("upload"));
        const QImage image = window_->grabWindow();
        QVERIFY(!image.isNull());
        QVERIFY(image.save(QCoreApplication::applicationDirPath() + "/storage-view.png"));
        app_.transfers_.finish(100, "/share/sub/report (1).pdf", {}, false);
        QCOMPARE(app_.transfers_.data(app_.transfers_.index(0), ucd::TransferListModel::LocalPathRole).toString(),
                 QStringLiteral("C:/tmp/report.pdf"));
        QCOMPARE(app_.transfers_.fileName(100), QStringLiteral("report (1).pdf"));
        app_.transfers_.clearFinished();
    }
};

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    QGuiApplication::setFont(QFont(QStringLiteral("Segoe UI"), 10));
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    StorageViewTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "StorageViewTest.moc"
