#pragma once
// The QML-facing side of the app. Lives on the GUI thread, owns the worker
// thread the Session runs on, forwards user actions to it, and owns the
// persisted settings.

#include <deque>
#include <memory>
#include <optional>

#include <QObject>
#include <QString>
#include <QStringList>
#include <QDateTime>
#include <QMap>
#include <QSet>
#include <QThread>
#include <QUrl>
#include <QVariantMap>

#include "app/PeerListModel.hpp"
#include "app/TransferListModel.hpp"
#include "core/Session.hpp"

namespace ucd {

class DebugWindow;

struct LaunchOptions {
    QString server;
    QString topicLink;
    QString deviceName;
    bool    autoConnect = false;
    bool    showDebug   = false;
    bool    verbose     = false;
};

class AppController : public QObject {
    Q_OBJECT
    // --- settings ---
    Q_PROPERTY(bool firstRun READ firstRun NOTIFY firstRunChanged)
    Q_PROPERTY(QString serverAddress READ serverAddress WRITE setServerAddress NOTIFY settingsChanged)
    Q_PROPERTY(QString deviceName READ deviceName WRITE setDeviceName NOTIFY settingsChanged)
    Q_PROPERTY(QStringList offeredServices READ offeredServices NOTIFY settingsChanged)
    Q_PROPERTY(bool autoConnect READ autoConnect NOTIFY settingsChanged)
    Q_PROPERTY(bool rememberTopic READ rememberTopic NOTIFY settingsChanged)
    Q_PROPERTY(bool verboseLogging READ verboseLogging NOTIFY settingsChanged)
    Q_PROPERTY(bool debugOnStartup READ debugOnStartup NOTIFY settingsChanged)
    Q_PROPERTY(QStringList sharedFolders READ sharedFolders NOTIFY settingsChanged)
    Q_PROPERTY(QString downloadFolder READ downloadFolder NOTIFY settingsChanged)
    // --- topic ---
    Q_PROPERTY(QString topicLink READ topicLink WRITE setTopicLink NOTIFY topicLinkChanged)
    Q_PROPERTY(bool topicLinkValid READ topicLinkValid NOTIFY topicLinkChanged)
    // --- session ---
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString statusDetail READ statusDetail NOTIFY statusChanged)
    Q_PROPERTY(bool active READ active NOTIFY statusChanged)
    Q_PROPERTY(bool serverConnected READ serverConnected NOTIFY snapshotChanged)
    Q_PROPERTY(QString selfId READ selfId NOTIFY snapshotChanged)
    Q_PROPERTY(QString topicId READ topicId NOTIFY snapshotChanged)
    Q_PROPERTY(QString reflexive READ reflexive NOTIFY snapshotChanged)
    Q_PROPERTY(ucd::PeerListModel* peers READ peers CONSTANT)
    Q_PROPERTY(bool debugAvailable READ debugAvailable CONSTANT)
    Q_PROPERTY(ucd::TransferListModel* transfers READ transfers CONSTANT)
    // --- identity and permissions ---
    Q_PROPERTY(QString myFingerprint READ myFingerprint CONSTANT)
    Q_PROPERTY(QVariantList permissionDefinitions READ permissionDefinitions CONSTANT)
    Q_PROPERTY(QStringList defaultPermissions READ defaultPermissions NOTIFY trustChanged)
    Q_PROPERTY(QVariantList trustedDevices READ trustedDevices NOTIFY trustChanged)

public:
    explicit AppController(const LaunchOptions& options, QObject* parent = nullptr);
    ~AppController() override;

    bool        firstRun() const { return firstRun_; }
    QString     serverAddress() const { return serverAddress_; }
    void        setServerAddress(const QString& value);
    QString     deviceName() const { return deviceName_; }
    void        setDeviceName(const QString& value);
    QStringList offeredServices() const { return serviceNames(services_); }
    bool        autoConnect() const { return autoConnect_; }
    bool        rememberTopic() const { return rememberTopic_; }
    bool        verboseLogging() const { return verbose_; }
    bool        debugOnStartup() const { return debugOnStartup_; }
    QStringList sharedFolders() const { return sharedFolders_; }
    QString     downloadFolder() const { return downloadFolder_; }

    QString topicLink() const { return topicLink_; }
    void    setTopicLink(const QString& value);
    bool    topicLinkValid() const;

    // "offline" | "connecting" | "online" | "reconnecting" | "error"
    QString status() const;
    QString statusDetail() const { return statusDetail_; }
    bool    active() const;

    bool    serverConnected() const { return snapshot_.serverConnected; }
    QString selfId() const { return snapshot_.selfId; }
    QString topicId() const { return snapshot_.topicId; }
    QString reflexive() const { return snapshot_.reflexive; }

    QString      myFingerprint() const { return myFingerprint_; }
    QVariantList permissionDefinitions() const;
    QStringList  defaultPermissions() const { return defaultPermissions_; }
    // [{key, fingerprint, name, permissions, since}], by name.
    QVariantList trustedDevices() const;

    PeerListModel*     peers() { return &peers_; }
    TransferListModel* transfers() { return &transfers_; }
    bool               debugAvailable() const;

    Q_INVOKABLE bool isValidServer(const QString& address) const;
    Q_INVOKABLE void completeFirstRun(const QString& server, const QString& name);
    // Keys: server, deviceName, services, autoConnect, rememberTopic,
    // verboseLogging, debugOnStartup, sharedFolders, downloadFolder,
    // defaultPermissions, and untrust (public keys to stop trusting).
    // Absent keys are left unchanged.
    Q_INVOKABLE void applySettings(const QVariantMap& values);

    Q_INVOKABLE void connectToTopic();
    Q_INVOKABLE void disconnectFromTopic();
    Q_INVOKABLE void generateTopicLink();
    Q_INVOKABLE void pasteTopicLink();
    Q_INVOKABLE void copyTopicLink();
    Q_INVOKABLE void copyTopicId();
    Q_INVOKABLE void copyText(const QString& text) const;
    Q_INVOKABLE void connectPeer(const QString& devId);
    Q_INVOKABLE void disconnectPeer(const QString& devId);
    Q_INVOKABLE void refreshPeers();
    Q_INVOKABLE void showDebugWindow();

    // Storage on another device. Results arrive as storageListed() and in
    // `transfers`; the returned id identifies the request.
    Q_INVOKABLE quint32 storageList(const QString& devId, const QString& path);
    Q_INVOKABLE quint32 storageDownload(const QString& devId, const QString& remotePath);
    // `replace`: overwrite files of the same name (needs their storage.write)
    // instead of adding numbered copies.
    Q_INVOKABLE int     storageUpload(const QString& devId, const QString& remoteFolder, const QList<QUrl>& files,
                                      bool replace = false);
    // Write operations; each finishes with storageOperationFinished().
    Q_INVOKABLE quint32 storageRename(const QString& devId, const QString& path, const QString& newName);
    Q_INVOKABLE int     storageDelete(const QString& devId, const QStringList& paths);
    Q_INVOKABLE quint32 storageMakeFolder(const QString& devId, const QString& folder, const QString& name);
    Q_INVOKABLE void    storageCancel(quint32 requestId);
    Q_INVOKABLE void    showInFolder(const QString& localPath) const;
    Q_INVOKABLE QString localPath(const QUrl& url) const;

    // Trust is per public key (hex), which a device proves in each session.
    // `name` is remembered as what the device was called when trusted. A newly
    // trusted device starts with the default permissions.
    Q_INVOKABLE void trustDevice(const QString& key, const QString& name);
    Q_INVOKABLE void untrustDevice(const QString& key);
    Q_INVOKABLE void setDevicePermission(const QString& key, const QString& permission, bool granted);

    // Used by the debug window.
    const NodeSnapshot&        snapshot() const { return snapshot_; }
    const std::deque<QString>& logLines() const { return log_; }
    const QString&             serverReport() const { return serverReport_; }
    void                       clearLog() { log_.clear(); }
    void                       requestServerStats();
    void                       requestTopicListing();

signals:
    void firstRunChanged();
    void settingsChanged();
    void topicLinkChanged();
    void statusChanged();
    void snapshotChanged();
    // A short, transient message for the status bar.
    void notify(const QString& message);
    void storageListed(quint32 requestId, const QString& path, const QVariantList& entries,
                       const QString& error);
    void storageUploaded(const QString& devId, const QString& remotePath);
    void storageOperationFinished(quint32 requestId, const QString& devId, const QString& path, const QString& error);
    void trustChanged();

private:
    template <class F> void onSession(F&& f);
    void setServices(uint32_t services);
    void pushHello();
    quint32 startOperation(const QString& devId, const QJsonObject& request);
    void pushSharedFolders();
    void pushAccessPolicy();
    void loadTrust();
    void saveTrusted(const QString& key);
    void appendLog(const QString& line);

    bool     firstRun_ = false;
    QString  serverAddress_;
    QString  deviceName_;
    uint32_t services_       = ServiceNone;
    bool     autoConnect_    = false;
    bool     rememberTopic_  = true;
    bool     verbose_        = false;
    bool     debugOnStartup_ = false;
    QString  topicLink_;
    QStringList sharedFolders_;
    QString     downloadFolder_;

    Session::Status status_ = Session::Status::Offline;
    QString         statusDetail_;
    NodeSnapshot    snapshot_;
    PeerListModel   peers_;

    struct TrustedDevice {
        QString     name;
        QStringList permissions;
        QDateTime   since;
    };
    std::optional<Identity>      identity_;
    QString                      myFingerprint_;
    QStringList                  defaultPermissions_;
    QMap<QString, TrustedDevice> trusted_;  // by public key (hex)

    TransferListModel transfers_;
    quint32           nextRequestId_ = 1;
    QSet<quint32>     canceled_;
    QHash<quint32, QString> operationDevices_;  // request id -> device

    std::deque<QString> log_;
    QString             serverReport_;

    QThread  worker_;
    Session* session_ = nullptr;  // lives on worker_

    std::unique_ptr<DebugWindow> debugWindow_;
};

}  // namespace ucd
