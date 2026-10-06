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
    quint32 requestId = 0;

    QObject* peers() { return &peers_; }
    QObject* transfers() { return &transfers_; }
    QString downloadFolder() const { return QDir::tempPath(); }

    Q_INVOKABLE quint32 storageList(const QString&, const QString& path) {
        const quint32 id = ++requestId;
        QTimer::singleShot(0, this, [this, id, path] { emit storageListed(id, path, {}, {}); });
        return id;
    }
    Q_INVOKABLE int storageUpload(const QString&, const QString& folder, const QList<QUrl>& files) {
        uploaded = files;
        destination = folder;
        return static_cast<int>(files.size());
    }
    Q_INVOKABLE void showInFolder(const QString&) {}
signals:
    void storageListed(quint32 requestId, const QString& path, const QVariantList& entries, const QString& error);
    void storageUploaded(const QString& devId, const QString& remotePath);
};

class StorageViewTest : public QObject {
    Q_OBJECT
    QTemporaryDir source_;
    StorageApp app_;
    std::unique_ptr<QQmlEngine> engine_;
    std::unique_ptr<QQuickWindow> window_;
    QQuickItem* view_ = nullptr;

    void grant(bool upload) {
        ucd::PeerRecord peer;
        peer.devId = QStringLiteral("test-device");
        peer.name = QStringLiteral("Test device");
        if (upload) peer.grantedToMe = {ucd::perm::StorageUpload};
        app_.peers_.setPeers({peer});
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

    void refreshesOnlyTheUploadedFolder() {
        const quint32 before = app_.requestId;
        emit app_.storageUploaded("other-device", "/share/sub/file.txt");
        emit app_.storageUploaded("test-device", "/share/other/file.txt");
        QCOMPARE(app_.requestId, before);
        emit app_.storageUploaded("test-device", "/share/sub/file.txt");
        QCOMPARE(app_.requestId, before + 1);
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
