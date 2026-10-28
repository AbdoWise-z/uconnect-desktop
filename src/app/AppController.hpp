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
#include <QTimer>
#include <QUrl>
#include <QVariantMap>

#include "app/PeerListModel.hpp"
#include "app/TransferListModel.hpp"
#include "core/OpenH264Installer.hpp"
#include "core/Session.hpp"
#include "core/screen/StopShortcut.hpp"

namespace ucd {

class DebugWindow;
class ScreenController;
class DisplayController;
class TrayIcon;

struct LaunchOptions {
    QString server;
    QString topicLink;
    QString deviceName;
    bool    autoConnect = false;
    bool    showDebug   = false;
    bool    verbose     = false;
    bool    forceRelay  = false;
    QString profile;            // --profile, if any
    bool    minimized   = false;  // start in the tray, without the window
};

class AppController : public QObject {
    Q_OBJECT
    // --- settings ---
    Q_PROPERTY(bool firstRun READ firstRun NOTIFY firstRunChanged)
    Q_PROPERTY(QObject* screen READ screen CONSTANT)
    Q_PROPERTY(QObject* camera READ camera CONSTANT)
    // Downloads Cisco's OpenH264 binary for the software codec fallback.
    Q_PROPERTY(QObject* openH264 READ openH264 CONSTANT)
    Q_PROPERTY(QObject* display READ display CONSTANT)
    Q_PROPERTY(QObject* tray READ tray CONSTANT)
    Q_PROPERTY(QString serverAddress READ serverAddress WRITE setServerAddress NOTIFY settingsChanged)
    Q_PROPERTY(QString deviceName READ deviceName WRITE setDeviceName NOTIFY settingsChanged)
    Q_PROPERTY(QStringList offeredServices READ offeredServices NOTIFY settingsChanged)
    Q_PROPERTY(bool autoConnect READ autoConnect NOTIFY settingsChanged)
    Q_PROPERTY(bool rememberTopic READ rememberTopic NOTIFY settingsChanged)
    Q_PROPERTY(bool verboseLogging READ verboseLogging NOTIFY settingsChanged)
    // Chosen colour scheme id (see qml/Theme.qml): dark, darker, light, colorful, system.
    Q_PROPERTY(QString colorScheme READ colorScheme NOTIFY settingsChanged)
    // Relay policy: 0 auto, 1 always, 2 direct (media never relays).
    Q_PROPERTY(int relayMode READ relayMode NOTIFY settingsChanged)
    Q_PROPERTY(bool debugOnStartup READ debugOnStartup NOTIFY settingsChanged)
    // Closing the window leaves the app running in the tray, where there is one.
    Q_PROPERTY(bool closeToTray READ closeToTray NOTIFY settingsChanged)
    // Starts when the user signs in (read from the system, see LoginItem)...
    Q_PROPERTY(bool launchAtLogin READ launchAtLogin NOTIFY settingsChanged)
    // ...and then in the tray, without opening the window.
    Q_PROPERTY(bool startHidden READ startHidden NOTIFY settingsChanged)
    // This run started in the tray, so the window waits to be opened.
    Q_PROPERTY(bool startsHidden READ startsHidden CONSTANT)
    Q_PROPERTY(QStringList sharedFolders READ sharedFolders NOTIFY settingsChanged)
    // Those of sharedFolders that other devices may not change.
    Q_PROPERTY(QStringList readOnlyFolders READ readOnlyFolders NOTIFY settingsChanged)
    Q_PROPERTY(QString downloadFolder READ downloadFolder NOTIFY settingsChanged)
    // Largest upload this device accepts, in MB; 0 for no limit.
    Q_PROPERTY(int maxUploadMB READ maxUploadMB NOTIFY settingsChanged)
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
    Q_PROPERTY(QStringList defaultPermissions READ defaultPermissions NOTIFY specialRulesChanged)
    // Devices with special rules: [{key, fingerprint, name, permissions, exception, since}].
    Q_PROPERTY(QVariantList specialDevices READ specialDevices NOTIFY specialRulesChanged)
    // This topic's network rules, [{pattern,label,perms}], first
    // match wins; and this device's IPv4 addresses (for the "m" parts).
    Q_PROPERTY(QVariantList networkRules READ networkRules NOTIFY networkRulesChanged)
    Q_PROPERTY(QStringList localAddresses READ localAddresses NOTIFY networkRulesChanged)
    // What other devices did in this device's shared folders, newest first:
    // [{time, devId, device, fingerprint, action, path, detail}].
    Q_PROPERTY(QVariantList storageActivity READ storageActivity NOTIFY storageActivityChanged)
    // --- folder sync ---
    // [{id, label, path, paused, state, files, bytes, dirs, free, needFiles,
    //   needBytes, conflicts, blocked, held, pendingDeletes,
    //   pendingDeleteCount, transfers, members: [{key, name, fingerprint,
    //   receive, contribute, paused, special, online, lastSeen, lastInSync,
    //   toSendFiles, toSendBytes, toSendChanges, toReceiveFiles,
    //   toReceiveBytes, toReceiveChanges, current, sending, rate,
    //   sendRate, eta, skipped}]}]; see SyncService::folderChanged().
    Q_PROPERTY(QVariantList syncFolders READ syncFolders NOTIFY syncChanged)
    // Folders connected devices offer this one: [{key, device, id, label, joined}].
    Q_PROPERTY(QVariantList syncOffers READ syncOffers NOTIFY syncChanged)
    // Conflicts and problems, newest first: [{time, folderId, folder, path, text}].
    Q_PROPERTY(QVariantList syncEvents READ syncEvents NOTIFY syncChanged)
    // Transfers across every synced folder: {active: [{folder, label, path,
    // size, done, key, device}], queued, rate (bytes/s fetched)}.
    Q_PROPERTY(QVariantMap syncTransfers READ syncTransfers NOTIFY syncChanged)
    // Sync problems (conflicts, deletes waiting, names that cannot sync, a
    // device that stays behind) also go to the system tray.
    Q_PROPERTY(bool syncNotifications READ syncNotifications NOTIFY settingsChanged)

public:
    QObject* screen() const;
    QObject* camera() const;
    QObject* openH264() const { return openH264_; }
    QObject* display() const;
    QObject* tray() const;
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
    QString     colorScheme() const { return colorScheme_; }
    int         relayMode() const { return relayMode_; }
    bool        debugOnStartup() const { return debugOnStartup_; }
    bool        closeToTray() const { return closeToTray_; }
    bool        launchAtLogin() const { return launchAtLogin_; }
    bool        startHidden() const { return startHidden_; }
    bool        startsHidden() const { return startsHidden_; }
    QStringList sharedFolders() const { return sharedFolders_; }
    QStringList readOnlyFolders() const { return readOnlyFolders_; }
    QString     downloadFolder() const { return downloadFolder_; }
    int         maxUploadMB() const { return maxUploadMB_; }

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
    QVariantList specialDevices() const;
    QVariantList networkRules() const;
    QStringList  localAddresses() const;
    QVariantList storageActivity() const { return activity_; }
    QVariantList syncFolders() const;
    QVariantList syncOffers() const;
    QVariantList syncEvents() const { return syncEvents_; }
    QVariantMap  syncTransfers() const;
    bool         syncNotifications() const { return syncNotifications_; }

    // Sync. A new folder starts with no members; add them with
    // setSyncMember(). Joining an offer makes the offering device a member
    // that may send and receive.
    Q_INVOKABLE QString createSyncFolder(const QUrl& folder, const QString& label);
    Q_INVOKABLE bool    joinSyncFolder(const QString& key, const QString& id, const QString& label, const QUrl& folder);
    // Stops syncing; the files stay.
    Q_INVOKABLE void    leaveSyncFolder(const QString& id);
    Q_INVOKABLE void    setSyncMember(const QString& id, const QString& key, bool receive, bool contribute);
    Q_INVOKABLE void    setSyncPaused(const QString& id, bool paused);
    // `choice`: "mine", "theirs" (with the holder's key) or "skip".
    Q_INVOKABLE void    resolveSyncConflict(const QString& id, const QString& path, const QString& choice,
                                            const QString& key);
    Q_INVOKABLE void    rescanSyncFolder(const QString& id);
    Q_INVOKABLE void    openSyncFolder(const QString& id) const;
    // Opens .synctrash, where files sync removed or set aside wait 30 days.
    Q_INVOKABLE void    openSyncKept(const QString& id);
    Q_INVOKABLE void    clearSyncEvents();
    // A member that is paused keeps its grants but gets and gives nothing.
    Q_INVOKABLE void    setSyncMemberPaused(const QString& id, const QString& key, bool paused);
    // The folder's files, rules and kept files come from the sync thread and
    // arrive through the *Loaded signals below.
    Q_INVOKABLE void    loadSyncFiles(const QString& id);
    Q_INVOKABLE void    loadSyncFileInfo(const QString& id, const QString& path);
    Q_INVOKABLE void    loadSyncRules(const QString& id, bool skipFile);
    Q_INVOKABLE void    previewSyncRules(const QString& id, bool skipFile, const QString& text, const QString& probe);
    Q_INVOKABLE void    saveSyncRules(const QString& id, bool skipFile, const QString& text);
    // "Ignore everywhere" (`skipFile` false) or "Skip on this device" for one path.
    Q_INVOKABLE void    addSyncRule(const QString& id, bool skipFile, const QString& path);
    Q_INVOKABLE void    loadSyncKept(const QString& id);
    Q_INVOKABLE void    restoreSyncKept(const QString& id, const QString& kept);
    Q_INVOKABLE void    discardSyncKept(const QString& id, const QString& kept);
    // Deletes held back by the large-delete guard: apply them, or keep the files.
    Q_INVOKABLE void    confirmSyncDeletes(const QString& id, bool apply);
    Q_INVOKABLE void    openSyncPath(const QString& id, const QString& path) const;
    Q_INVOKABLE void    showSyncPath(const QString& id, const QString& path) const;
    // The device behind a version vector's member id, or the id shortened.
    Q_INVOKABLE QString syncMemberName(const QString& memberId) const;
    Q_INVOKABLE void clearStorageActivity();

    PeerListModel*     peers() { return &peers_; }
    TransferListModel* transfers() { return &transfers_; }
    bool               debugAvailable() const;

    Q_INVOKABLE bool isValidServer(const QString& address) const;
    Q_INVOKABLE void completeFirstRun(const QString& server, const QString& name);
    // Keys: server, deviceName, services, autoConnect, rememberTopic,
    // verboseLogging, debugOnStartup, closeToTray, launchAtLogin, startHidden,
    // sharedFolders, readOnlyFolders,
    // downloadFolder, maxUploadMB,
    // defaultPermissions, networkRules, and removeSpecial (public keys whose
    // special rules to drop).
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
    // `files` may include folders, which are uploaded with everything in them.
    // `replace`: overwrite files of the same name (needs their storage.write)
    // and merge into folders of the same name, instead of adding numbered
    // copies.
    Q_INVOKABLE int     storageUpload(const QString& devId, const QString& remoteFolder, const QList<QUrl>& files,
                                      bool replace = false);
    // Write operations; each finishes with storageOperationFinished().
    Q_INVOKABLE quint32 storageRename(const QString& devId, const QString& path, const QString& newName);
    Q_INVOKABLE int     storageDelete(const QString& devId, const QStringList& paths);
    Q_INVOKABLE quint32 storageMakeFolder(const QString& devId, const QString& folder, const QString& name);
    // Move or copy items into `toFolder` on the same device; one request per
    // path, whose ids are returned. Long ones report storageOperationProgress().
    Q_INVOKABLE QVariantList storageMove(const QString& devId, const QStringList& paths, const QString& toFolder);
    Q_INVOKABLE QVariantList storageCopy(const QString& devId, const QStringList& paths, const QString& toFolder);
    // Downloads files and whole folders (recursively) into the download folder.
    // `items` are {name, dir} entries of `folder` on that device. Returns a
    // batch id; storageBatchFinished() reports the outcome.
    Q_INVOKABLE quint32 storageDownloadItems(const QString& devId, const QString& folder, const QVariantList& items);
    // As storageDownloadItems(), into a private staging folder, so the items
    // can then be dragged out of the app as ordinary local files.
    Q_INVOKABLE quint32 prepareDragOut(const QString& devId, const QString& folder, const QVariantList& items);
    // Fetches a file to a temporary folder and opens it with its default app.
    // Returns a batch id, as storageDownloadItems().
    Q_INVOKABLE quint32 storageOpen(const QString& devId, const QString& folder, const QString& name);
    // Fetches a file for a quick look; storagePreviewReady() tells where.
    Q_INVOKABLE quint32 storagePreview(const QString& devId, const QString& folder, const QString& name);
    // The start of a fetched file as text: {text, truncated, binary}.
    Q_INVOKABLE QVariantMap previewText(const QUrl& file) const;
    Q_INVOKABLE void    cancelBatch(quint32 batchId);
    // Reports changes to a folder's listing with storageFolderChanged() until
    // storageUnwatch(). Ends quietly when the device disconnects.
    Q_INVOKABLE quint32 storageWatch(const QString& devId, const QString& path);
    Q_INVOKABLE void    storageUnwatch(quint32 watchId);
    Q_INVOKABLE void    storageCancel(quint32 requestId);
    // Starts a failed transfer again under a new id, in place of the old row.
    // A download picks up from what arrived before, where it can.
    Q_INVOKABLE void    retryTransfer(quint32 requestId);
    Q_INVOKABLE void    showInFolder(const QString& localPath) const;
    Q_INVOKABLE QString localPath(const QUrl& url) const;

    // Special rules: permissions for one device, by the public key (hex) it
    // proves in each session; they add to its network rule and the defaults.
    // Adding them starts empty, so nothing changes until they are edited.
    // `name` is remembered as what it was called then.
    Q_INVOKABLE void addSpecialRules(const QString& key, const QString& name);
    Q_INVOKABLE void removeSpecialRules(const QString& key);
    // Sets one permission in a device's special rules (adding them if needed).
    Q_INVOKABLE void setDevicePermission(const QString& key, const QString& permission, bool granted);
    // An exception: only the device's special rules apply, not its network
    // rule or the defaults. Turning it on takes in what those granted, so
    // nothing changes until the rules are edited.
    Q_INVOKABLE void setSpecialException(const QString& key, bool exception);

    // Network rules (per topic). Saved through applySettings({networkRules}).
    Q_INVOKABLE QString networkPatternProblem(const QString& pattern) const;
    // Asks a connected device for its rules on this topic; the answer comes as
    // networkRulesOffered, for the user to import (importNetworkRules).
    Q_INVOKABLE void requestNetworkRules(const QString& devId);
    Q_INVOKABLE void importNetworkRules(const QVariantList& rules, bool replace);

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
    // A message for the system tray.
    void trayMessage(const QString& title, const QString& text);
    // Answers to loadSyncFiles() and the other sync loads.
    void syncFilesLoaded(const QString& id, const QVariantList& files);
    void syncFileInfoLoaded(const QString& id, const QString& path, const QVariantMap& info);
    void syncRulesLoaded(const QString& id, bool skipFile, const QString& text);
    void syncRulesPreviewed(const QString& id, bool skipFile, const QVariantMap& preview);
    void syncKeptLoaded(const QString& id, const QVariantList& kept);
    // `readOnly`: the folder is in a share its device keeps read-only.
    void storageListed(quint32 requestId, const QString& path, const QVariantList& entries,
                       const QString& error, bool readOnly);
    void storageUploaded(const QString& devId, const QString& remotePath);
    void storageOperationFinished(quint32 requestId, const QString& devId, const QString& path, const QString& error);
    void storageOperationProgress(quint32 requestId, const QString& devId, qint64 done, qint64 total);
    void storageFolderChanged(quint32 watchId, const QString& path);
    void storageBatchProgress(quint32 batchId, int filesDone, int filesKnown);
    // `urls`: file URLs of the top-level items, as downloaded.
    void storageBatchFinished(quint32 batchId, int failed, bool canceled, const QStringList& urls);
    void storagePreviewReady(quint32 batchId, const QString& url, const QString& error);
    void specialRulesChanged();
    void networkRulesChanged();
    // A device's rules (or the reason it would not share them), to offer for
    // import.
    void networkRulesOffered(const QString& devId, const QString& deviceName, const QVariantList& rules,
                             const QString& error);
    void storageActivityChanged();
    void syncChanged();

private:
    template <class F> void onSession(F&& f);
    void setServices(uint32_t services);
    void pushHello();
    quint32 startOperation(const QString& devId, const QJsonObject& request);
    quint32 startBatch(const QString& devId, const QString& folder, const QVariantList& items,
                       const QString& localFolder, bool quiet);
    void    listForBatch(quint32 batchId, const QString& remoteDir, const QString& localDir);
    void    downloadForBatch(quint32 batchId, const QString& remotePath, const QString& localDir, int topSlot);
    void    onBatchList(quint32 listId, const QVariantList& entries, const QString& error);
    void    onBatchDownload(quint32 downloadId, const QString& localPath, const QString& error, bool canceled);
    void    finishBatchIfDone(quint32 batchId);
    void    uploadFile(const QString& devId, const QString& localPath, const QString& remoteFolder, bool replace,
                       quint32 batchId);
    void    folderForBatch(quint32 batchId, const QString& remoteParent, const QString& localDir, bool top);
    void    onBatchFolder(quint32 opId, const QString& remotePath, const QString& error);
    void    onBatchUpload(quint32 uploadId, const QString& error, bool canceled);
    void    finishUploadBatchIfDone(quint32 batchId);
    void pushSharedFolders();
    // (Re)registers the sign-in start with the current arguments.
    bool registerLogin(bool enabled);
    void pushUploadLimit();
    void pushAccessPolicy();
    QString      currentTopicKey() const;
    void         loadNetworkRules();
    void         saveNetworkRules();
    void         refreshLocalAddresses();
    QStringList  currentPermissionsOf(const QString& key) const;
    // What a device with this key is granted here, as far as is known: for
    // a connected peer every layer, otherwise its special rules and the
    // defaults; and the two sync questions asked of it.
    QStringList  permissionsOfKey(const QString& key) const;
    bool         syncsWithKey(const QString& key) const;
    bool         mayReceiveSyncFrom(const QString& key) const;
    void loadSpecialRules();
    void saveSpecialRules(const QString& key);
    void appendLog(const QString& line);
    void addActivity(const QVariantMap& entry);
    void loadSync();
    void saveSync() const;
    void pushSync();
    void addSyncEvent(const QString& folderId, const QString& path, const QString& kind);
    // A device that has had the same amount left to sync for a long time
    // while connected is told about, once.
    void checkSyncBehind(const QString& folderId, const QVariantMap& status);
    QString deviceName(const QString& key) const;
    // Why a file present here is not synced, for people; see
    // SyncFolder::heldReason().
    QString heldText(const QString& path, const QString& reason) const;
    void loadActivity();

    bool     firstRun_ = false;
    QString  serverAddress_;
    QString  deviceName_;
    uint32_t services_       = ServiceNone;
    bool     autoConnect_    = false;
    bool     rememberTopic_  = true;
    bool     verbose_        = false;
    QString  colorScheme_    = QStringLiteral("dark");
    bool     forceRelay_     = false;  // --force-relay, for this run only
    int      relayMode_      = 0;      // persisted: 0 auto, 1 always, 2 direct
    bool     debugOnStartup_ = false;
    bool     closeToTray_    = true;
    bool     launchAtLogin_  = false;
    bool     startHidden_    = true;
    bool     startsHidden_   = false;
    QString  profile_;
    QString  topicLink_;
    QStringList sharedFolders_;
    QStringList readOnlyFolders_;
    QString     downloadFolder_;
    int         maxUploadMB_ = 0;

    Session::Status status_ = Session::Status::Offline;
    QString         statusDetail_;
    NodeSnapshot    snapshot_;
    PeerListModel   peers_;

    struct SpecialRules {
        QString     name;
        QStringList permissions;
        QDateTime   since;
        bool        exception = false;  // only these apply (see AccessPolicy)
    };
    std::optional<Identity>      identity_;
    QString                      myFingerprint_;
    QStringList                  defaultPermissions_;
    QMap<QString, SpecialRules>  special_;  // by public key (hex)

    TransferListModel transfers_;
    quint32           nextRequestId_ = 1;
    QSet<quint32>     canceled_;
    QHash<quint32, QString> operationDevices_;  // request id -> device
    QHash<QString, QString> identityOf_;        // device id -> its public key, once verified
    QVector<NetworkRule>    networkRules_;      // the current topic's
    QString                 rulesTopic_;        // the topic id they belong to
    QList<QHostAddress>     localAddresses_;
    QTimer                  addressTimer_;

    // A multi-item or folder download: lists folders as it goes and downloads
    // every file found, keeping the folder structure.
    struct DownloadBatch {
        QString        devId;
        QStringList    topLevel;  // local paths of the items asked for
        QList<quint32> downloads;
        QString        staging;   // set for drag-out and open batches
        QString        firstError;
        int            pending    = 0;  // lists and downloads not yet finished
        int            filesKnown = 0;
        int            filesDone  = 0;
        int            failed     = 0;
        bool           quiet      = false;
        bool           canceled   = false;
        bool           open       = false;  // open the file when it arrives
        bool           preview    = false;  // report it with storagePreviewReady()
    };
    struct BatchList {
        quint32 batch = 0;
        QString remoteDir;
        QString localDir;
    };
    QHash<quint32, DownloadBatch> batches_;
    QHash<quint32, BatchList>     batchLists_;        // list request id ->
    QHash<quint32, quint32>       batchOfDownload_;   // download id -> batch
    QHash<quint32, int>           topSlotOfDownload_;

    // An upload that includes folders: each folder is created on the device
    // before what is in it is uploaded.
    struct UploadBatch {
        QString     devId;
        bool        replace  = false;
        QStringList topFolders;  // remote paths of the folders asked for
        QString     firstError;
        int         pending  = 0;  // folders and files not yet finished
        int         files    = 0;
        int         uploaded = 0;
        int         failed   = 0;
    };
    struct BatchFolder {
        quint32 batch = 0;
        QString localDir;
        bool    top = false;
    };
    QHash<quint32, UploadBatch> uploadBatches_;
    QHash<quint32, BatchFolder> batchFolders_;   // mkdir request id ->
    QHash<quint32, quint32>     batchOfUpload_;  // upload id -> batch

    QVariantList        activity_;
    QVector<sync::FolderConfig>   syncFolders_;
    QHash<QString, QVariantMap>   syncStatus_;  // by folder id
    QHash<QString, QVariantList>  syncOffers_;  // by peer key
    QVariantList                  syncEvents_;
    bool                          syncNotifications_ = true;
    struct Behind {
        qint64 left  = -1;  // bytes to send and receive, as last seen
        qint64 since = 0;   // ms since epoch, when it was last different
        bool   told  = false;
    };
    QHash<QString, Behind>        syncBehind_;  // by folder id + member key
    std::deque<QString> log_;
    QString             serverReport_;

    QThread  worker_;
    Session* session_ = nullptr;  // lives on worker_
    ScreenController* screen_ = nullptr;
    ScreenController* camera_ = nullptr;
    OpenH264Installer* openH264_ = nullptr;
    DisplayController* display_ = nullptr;
    TrayIcon* tray_ = nullptr;
    std::unique_ptr<screen::StopShortcut> mediaStop_;
    QString mediaShortcutSequence_, mediaShortcutError_;
    bool mediaShortcutActive_ = false;

    std::unique_ptr<DebugWindow> debugWindow_;
};

}  // namespace ucd
