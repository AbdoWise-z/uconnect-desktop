// Apply through the real QML dialog and controller, then reopen both to verify
// the persisted values. Settings and identity use an isolated test profile.
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJSValue>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

#include "app/AppController.hpp"

class SettingsTest : public QObject {
    Q_OBJECT
    QTemporaryDir temp_;
    std::unique_ptr<ucd::AppController> controller_;
    std::unique_ptr<QQmlEngine> engine_;
    std::unique_ptr<QObject> dialog_;

    QObject* draft() const {
        for (QObject* child : dialog_->findChildren<QObject*>())
            if (child->metaObject()->indexOfProperty("sharedFolders") >= 0) return child;
        return nullptr;
    }

    bool addFolder(const QString& path) {
        return QMetaObject::invokeMethod(dialog_.get(), "addSharedFolder",
            Q_ARG(QVariant, QVariant(QUrl::fromLocalFile(path))));
    }

private slots:
    void initTestCase() {
        QVERIFY(temp_.isValid());
        QGuiApplication::setOrganizationName(QStringLiteral("uConnect tests"));
        QGuiApplication::setApplicationName(QFileInfo(temp_.path()).fileName());
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temp_.path());
        QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, temp_.path());
        QStandardPaths::setTestModeEnabled(true);

        const QDir source(QStringLiteral(UCD_QML_DIR));
        QByteArray module;
        for (const QString& name : source.entryList({"*.qml"}, QDir::Files)) {
            QVERIFY(QFile::copy(source.filePath(name), QDir(temp_.path()).filePath(name)));
            const QByteArray type = QFileInfo(name).completeBaseName().toUtf8();
            module += (type == "Theme" ? "singleton " : "") + type + " 1.0 " + name.toUtf8() + '\n';
        }
        QFile qmldir(QDir(temp_.path()).filePath("qmldir"));
        QVERIFY(qmldir.open(QIODevice::WriteOnly));
        QCOMPARE(qmldir.write(module), module.size());
    }

    void init() {
        QQuickStyle::setStyle(QStringLiteral("Basic"));
        QSettings settings;
        settings.clear();
        settings.setValue(QStringLiteral("server"), QStringLiteral("localhost:4433"));
        settings.setValue(QStringLiteral("deviceName"), QStringLiteral("Settings test"));
        settings.sync();
        controller_ = std::make_unique<ucd::AppController>(ucd::LaunchOptions{});
        // A factory allows successive tests to use a fresh isolated controller.
        qmlRegisterSingletonType<ucd::AppController>("UConnect", 1, 0, "App",
            [this](QQmlEngine*, QJSEngine*) -> QObject* {
                QQmlEngine::setObjectOwnership(controller_.get(), QQmlEngine::CppOwnership);
                return controller_.get();
            });
        engine_ = std::make_unique<QQmlEngine>();
        QQmlComponent component(engine_.get(), QUrl::fromLocalFile(temp_.path() + "/SettingsWindow.qml"));
        QVERIFY2(component.isReady(), qPrintable(component.errorString()));
        dialog_.reset(component.create());
        QVERIFY2(dialog_, qPrintable(component.errorString()));
        QVERIFY(QMetaObject::invokeMethod(dialog_.get(), "load"));
    }

    void cleanup() {
        dialog_.reset();
        engine_.reset();
        controller_.reset();
        qmlClearTypeRegistrations();
    }

    void cleanupTestCase() {
        // Remove only the identity created for this test's unique profile.
        const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QFile::remove(QDir(appData).filePath("identity.json"));
        QDir().rmdir(appData);
    }

    void addedFolderSurvivesApplyAndReload() {
        const QString folder = QDir(temp_.path()).filePath("Shader folder");
        QVERIFY(QDir().mkpath(folder));
        QVERIFY(addFolder(folder));
        QCOMPARE(engine_->newQObject(draft()).property("sharedFolders").toVariant().toStringList(), QStringList{folder});
        QTRY_VERIFY(dialog_->property("modified").toBool());
        QVERIFY(QMetaObject::invokeMethod(dialog_.get(), "apply"));
        QCOMPARE(controller_->sharedFolders(), QStringList{folder});
        QCOMPARE(engine_->newQObject(draft()).property("sharedFolders").toVariant().toStringList(), QStringList{folder});
        QVERIFY(!dialog_->property("modified").toBool());
        QSettings().sync();
        QCOMPARE(QSettings().value("storage/sharedFolders").toStringList(), QStringList{folder});
        QVERIFY(QMetaObject::invokeMethod(dialog_.get(), "load"));
        QCOMPARE(controller_->sharedFolders(), QStringList{folder});
        ucd::AppController restarted(ucd::LaunchOptions{});
        QCOMPARE(restarted.sharedFolders(), QStringList{folder});
    }

    void appliesDownloadFolderAndDefaultPermissions() {
        const QString folder = QDir(temp_.path()).filePath("Downloads");
        QObject* settingsDraft = draft();
        QVERIFY(settingsDraft);
        QVERIFY(settingsDraft->setProperty("downloadFolder", folder));
        QVERIFY(QMetaObject::invokeMethod(dialog_.get(), "setDefaultPermission",
            Q_ARG(QVariant, QVariant(QStringLiteral("storage.upload"))), Q_ARG(QVariant, QVariant(true))));
        QVERIFY(QMetaObject::invokeMethod(dialog_.get(), "apply"));
        QCOMPARE(controller_->downloadFolder(), folder);
        QCOMPARE(controller_->defaultPermissions(), QStringList{QStringLiteral("storage.upload")});
        QVERIFY(!dialog_->property("modified").toBool());
        QSettings().sync();
        ucd::AppController restarted(ucd::LaunchOptions{});
        QCOMPARE(restarted.downloadFolder(), folder);
        QCOMPARE(restarted.defaultPermissions(), QStringList{QStringLiteral("storage.upload")});
    }

    void keepsReadOnlyOnlyForSharedFolders() {
        const QString a = QDir(temp_.path()).filePath("A"), b = QDir(temp_.path()).filePath("B");
        controller_->applySettings({{"sharedFolders", QStringList{a, b}}, {"readOnlyFolders", QStringList{b, "C:/not/shared"}}});
        QCOMPARE(controller_->readOnlyFolders(), QStringList{b});
        controller_->applySettings({{"sharedFolders", QStringList{a}}});  // B no longer shared
        QVERIFY(controller_->readOnlyFolders().isEmpty());
        controller_->applySettings({{"readOnlyFolders", QStringList{a}}});
        QSettings().sync();
        ucd::AppController restarted(ucd::LaunchOptions{});
        QCOMPARE(restarted.readOnlyFolders(), QStringList{a});
    }

    void screenGrantsCannotBecomeDefaults() {
        controller_->applySettings({{"defaultPermissions", QStringList{
            "screen.view", "screen.direct", "control.input", "control.direct", "storage.observe"}}});
        QCOMPARE(controller_->defaultPermissions(), QStringList{"storage.observe"});
        const QString key(64, QLatin1Char('b'));
        controller_->trustDevice(key, "Screen peer");
        controller_->setDevicePermission(key, "screen.direct", true);
        const auto devices = controller_->trustedDevices();
        QCOMPARE(devices.size(), 1);
        QVERIFY(devices.first().toMap().value("permissions").toStringList().contains("screen.direct"));
        controller_->applySettings({{"defaultPermissions", QStringList{"control.direct"}}});
        QVERIFY(controller_->defaultPermissions().isEmpty());
    }
    void screenSettingsSurviveApplyAndRestart() {
        QVERIFY(QMetaObject::invokeMethod(dialog_.get(), "setScreenSetting",
            Q_ARG(QVariant, QVariant("maxFps")), Q_ARG(QVariant, QVariant(24))));
        QVERIFY(QMetaObject::invokeMethod(dialog_.get(), "setScreenSetting", Q_ARG(QVariant, QVariant("maxViewers")), Q_ARG(QVariant, QVariant(3))));
        QTRY_VERIFY(dialog_->property("modified").toBool());
        QVERIFY(QMetaObject::invokeMethod(dialog_.get(), "apply"));
        QCOMPARE(controller_->screen()->property("settings").toMap().value("maxViewers").toInt(), 3);
        QCOMPARE(controller_->camera()->property("settings").toMap().value("maxViewers").toInt(), 3);
        QCOMPARE(controller_->screen()->property("settings").toMap().value("maxFps").toInt(), 24);
        QVERIFY(!dialog_->property("modified").toBool());
        QSettings().sync();
        ucd::AppController restarted(ucd::LaunchOptions{});
        QCOMPARE(restarted.screen()->property("settings").toMap().value("maxFps").toInt(), 24);
    }
#ifdef UCD_HAS_SCREEN
    void loadsScreenViewerAndSafetyQml() {
        QQmlComponent viewer(engine_.get(), QUrl::fromLocalFile(temp_.path() + "/ScreenView.qml"));
        QVERIFY2(viewer.isReady(), qPrintable(viewer.errorString()));
        std::unique_ptr<QObject> view(viewer.createWithInitialProperties({{"devId", QString(32, 'a')}}));
        QVERIFY2(view, qPrintable(viewer.errorString()));
        QQmlComponent safety(engine_.get(), QUrl::fromLocalFile(temp_.path() + "/ScreenSafety.qml"));
        QVERIFY2(safety.isReady(), qPrintable(safety.errorString()));
        std::unique_ptr<QObject> host(safety.create());
        QVERIFY2(host, qPrintable(safety.errorString()));
    }
#endif

    void appliesTheUploadLimit() {
        QObject* settingsDraft = draft();
        QVERIFY(settingsDraft);
        QCOMPARE(controller_->maxUploadMB(), 0);
        QVERIFY(settingsDraft->setProperty("maxUploadMB", 500));
        QTRY_VERIFY(dialog_->property("modified").toBool());
        QVERIFY(QMetaObject::invokeMethod(dialog_.get(), "apply"));
        QCOMPARE(controller_->maxUploadMB(), 500);
        QSettings().sync();
        ucd::AppController restarted(ucd::LaunchOptions{});
        QCOMPARE(restarted.maxUploadMB(), 500);
        controller_->applySettings({{"maxUploadMB", -3}});
        QCOMPARE(controller_->maxUploadMB(), 0);
    }

    // Download was folded into Read; grants saved before that keep their effect.
    void migratesSavedDownloadPermissionToRead() {
        const QString key = QString(64, QLatin1Char('a'));
        {
            QSettings settings;
            settings.setValue("permissions/defaults", QStringList{"storage.download", "storage.observe"});
            settings.setValue(QStringLiteral("trust/%1/name").arg(key), "Old device");
            settings.setValue(QStringLiteral("trust/%1/permissions").arg(key), QStringList{"storage.download"});
            settings.sync();
        }
        ucd::AppController restarted(ucd::LaunchOptions{});
        QCOMPARE(restarted.defaultPermissions(), (QStringList{"storage.observe", "storage.read"}));
        const QVariantList trusted = restarted.trustedDevices();
        QCOMPARE(trusted.size(), 1);
        QCOMPARE(trusted.first().toMap().value("permissions").toStringList(), QStringList{"storage.read"});
    }

    void emptyFolderListRemovesSavedFolders() {
        const QString folder = QDir(temp_.path()).filePath("Shader folder");
        QVERIFY(addFolder(folder));
        QVERIFY(QMetaObject::invokeMethod(dialog_.get(), "apply"));
        controller_->applySettings({{"deviceName", "Renamed device"}});
        QCOMPARE(controller_->sharedFolders(), QStringList{folder});
        controller_->applySettings({{"sharedFolders", QStringList{}}});
        QVERIFY(controller_->sharedFolders().isEmpty());
        QSettings().sync();
        ucd::AppController restarted(ucd::LaunchOptions{});
        QVERIFY(restarted.sharedFolders().isEmpty());
    }
};

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    SettingsTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "SettingsTest.moc"
