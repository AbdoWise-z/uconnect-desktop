#include "AppController.hpp"
#include "ScreenController.hpp"

#include <algorithm>

#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QRegularExpression>
#include <QSettings>
#include <QStringDecoder>
#include <QUuid>
#include <QStandardPaths>
#include <QSysInfo>
#include <QtDebug>

#ifdef UCD_HAS_DEBUG_UI
#include <QOpenGLContext>

#include "debug/DebugWindow.hpp"
#else
namespace ucd {
class DebugWindow {};
}  // namespace ucd
#endif

namespace ucd {
namespace {

constexpr size_t kMaxLogLines = 1000;

namespace Key {
constexpr auto Server         = "server";
constexpr auto TopicLink      = "topicLink";
constexpr auto DeviceName     = "deviceName";
constexpr auto Services       = "services";
constexpr auto AutoConnect    = "autoConnect";
constexpr auto RememberTopic  = "rememberTopic";
constexpr auto Verbose        = "verboseLogging";
constexpr auto DebugOnStartup = "debugOnStartup";
constexpr auto SharedFolders  = "storage/sharedFolders";
constexpr auto ReadOnly       = "storage/readOnlyFolders";
constexpr auto DownloadFolder = "storage/downloadFolder";
constexpr auto MaxUploadMB    = "storage/maxUploadMB";
constexpr auto DefaultPerms   = "permissions/defaults";
constexpr auto Trust          = "trust";  // a group per trusted public key
}  // namespace Key

constexpr int kMaxBatchFiles = 10000;
constexpr int kMaxActivity   = 1000;
constexpr int kMaxSyncEvents = 200;

// Kept beside the identity, one JSON object per line, oldest first.
QString activityFile() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/storage-activity.jsonl");
}

// Where drag-out batches are fetched to before they are dragged out, and
// files opened from another device before they are opened.
QString dragStagingRoot() {
    return QStandardPaths::writableLocation(QStandardPaths::TempLocation) + QStringLiteral("/uconnect-drag");
}
QString openStagingRoot() {
    return QStandardPaths::writableLocation(QStandardPaths::TempLocation) + QStringLiteral("/uconnect-open");
}
QString newStagingFolder(const QString& root, quint32 id) {
    return QStringLiteral("%1/%2-%3").arg(root).arg(QDateTime::currentMSecsSinceEpoch()).arg(id);
}

// Drop targets may still be copying from a staging folder after the drag
// ends, and opened files may still be open, so they are cleared on a later
// start instead.
void removeOldStaging(const QString& root) {
    const QDateTime cutoff = QDateTime::currentDateTime().addDays(-1);
    const QFileInfoList stale = QDir(root).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo& dir : stale)
        if (dir.lastModified() < cutoff) QDir(dir.absoluteFilePath()).removeRecursively();
}

QString uniqueLocalPath(const QString& folder, const QString& name) {
    const QDir dir(folder);
    QString candidate = dir.filePath(name);
    for (int n = 1; QFileInfo::exists(candidate); ++n) candidate = dir.filePath(QStringLiteral("%1 (%2)").arg(name).arg(n));
    return candidate;
}

QString joinRemote(const QString& folder, const QString& name) {
    return folder.endsWith(QLatin1Char('/')) ? folder + name : folder + QLatin1Char('/') + name;
}

QStringList knownOnly(const QStringList& permissions, bool trusted = true) {
    QStringList out;
    for (QString p : permissions) {
        // "storage.download" was folded into "storage.read"; saved grants keep working.
        if (p == QLatin1String("storage.download")) p = perm::StorageRead;
        if (isKnownPermission(p) && (trusted || !isTrustedOnlyPermission(p)) && !out.contains(p)) out << p;
    }
    out.sort();
    return out;
}

}  // namespace

AppController::AppController(const LaunchOptions& options, QObject* parent) : QObject(parent) {
    QSettings settings;
    firstRun_       = !settings.contains(Key::Server) && options.server.isEmpty();
    serverAddress_  = settings.value(Key::Server).toString();
    deviceName_     = settings.value(Key::DeviceName, QSysInfo::machineHostName()).toString();
    services_       = servicesFromNames(settings.value(Key::Services).toStringList());
    autoConnect_    = settings.value(Key::AutoConnect, false).toBool();
    rememberTopic_  = settings.value(Key::RememberTopic, true).toBool();
    verbose_        = settings.value(Key::Verbose, false).toBool() || options.verbose;
    debugOnStartup_ = settings.value(Key::DebugOnStartup, false).toBool();
    sharedFolders_  = settings.value(Key::SharedFolders).toStringList();
    readOnlyFolders_ = settings.value(Key::ReadOnly).toStringList();
    downloadFolder_ = settings.value(Key::DownloadFolder,
                                     QStandardPaths::writableLocation(QStandardPaths::DownloadLocation))
                          .toString();
    maxUploadMB_    = std::max(0, settings.value(Key::MaxUploadMB, 0).toInt());
    // The link carries the topic's secret key, so remembering it stores a
    // credential in plain settings. Opt-out exists in Settings > Security; move
    // it to the OS keychain before shipping.
    if (rememberTopic_) topicLink_ = settings.value(Key::TopicLink).toString();

    if (!options.server.isEmpty()) serverAddress_ = options.server;
    if (!options.topicLink.isEmpty()) topicLink_ = options.topicLink;
    if (!options.deviceName.isEmpty()) deviceName_ = options.deviceName;

    // Per settings profile, like everything else (see --profile).
    QString identityWarning;
    identity_ = Identity::loadOrCreate(
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/identity.json"),
        &identityWarning);
    myFingerprint_ = fingerprint(identity_->publicKey());
    loadTrust();
    loadActivity();
    loadSync();

    session_ = new Session;
    session_->moveToThread(&worker_);
    screen_ = new ScreenController(session_, this);
    connect(screen_, &ScreenController::rememberPermission, this, [this](const QString& key, const QString& permission) {
        setDevicePermission(key, permission, true);
    });
    connect(screen_, &ScreenController::notify, this, &AppController::notify);
    connect(&worker_, &QThread::finished, session_, &QObject::deleteLater);

    connect(session_, &Session::statusChanged, this, [this](Session::Status s, const QString& detail) {
        status_       = s;
        statusDetail_ = detail;
        emit statusChanged();
    });
    connect(session_, &Session::snapshotChanged, this, [this](const NodeSnapshot& s) {
        snapshot_ = s;
        emit snapshotChanged();
    });
    connect(session_, &Session::peersChanged, this, [this](const QVector<PeerRecord>& p) {
        for (const PeerRecord& peer : p)
            if (!peer.identityKey.isEmpty()) identityOf_.insert(peer.devId, peer.identityKey);
        peers_.setPeers(p);
    });
    connect(session_, &Session::logMessage, this, &AppController::appendLog);
    SyncService* sync = session_->sync();
    connect(sync, &SyncService::offersChanged, this, [this](const QString& key, const QVariantList& offers) {
        if (offers.isEmpty()) syncOffers_.remove(key);
        else syncOffers_.insert(key, offers);
        emit syncChanged();
    });
    connect(sync, &SyncService::folderChanged, this, [this](const QString& id, const QVariantMap& status) {
        syncStatus_.insert(id, status);
        emit syncChanged();
    });
    connect(sync, &SyncService::event, this, &AppController::addSyncEvent);
    connect(&peers_, &PeerListModel::changed, this, &AppController::syncChanged);  // who is online
    connect(this, &AppController::trustChanged, this, &AppController::syncChanged);
    connect(session_, &Session::storageActivity, this,
            [this](const QString& devId, const QString& device, const QString& key, const QString& action,
                   const QString& path, const QString& detail) {
                addActivity({
                    {"time", QDateTime::currentDateTime().toString(Qt::ISODate)},
                    {"devId", devId},
                    {"device", device.left(64)},
                    {"fingerprint", key.isEmpty() ? QString() : fingerprintOfHex(key)},
                    {"action", action},
                    {"path", path},
                    {"detail", detail},
                });
            });
    connect(session_, &Session::serverReport, this, [this](const QString& text) {
        serverReport_ = text;
    });

    StorageService* storage = session_->storage();
    connect(storage, &StorageService::listFinished, this,
            [this](quint32 id, const QString& path, const QVariantList& entries, const QString& error, bool readOnly) {
                if (batchLists_.contains(id)) onBatchList(id, entries, error);
                else emit storageListed(id, path, entries, error, readOnly);
            });
    connect(storage, &StorageService::downloadProgress, this, [this](quint32 id, qint64 received, qint64 total) {
        transfers_.progress(id, received, total);
    });
    connect(storage, &StorageService::downloadFinished, this,
            [this](quint32 id, const QString& localPath, const QString& error) {
                const bool canceled = canceled_.remove(id);
                transfers_.finish(id, localPath, error, canceled);
                if (batchOfDownload_.contains(id)) return onBatchDownload(id, localPath, error, canceled);
                if (error.isEmpty())
                    emit notify(tr("Downloaded %1").arg(QFileInfo(localPath).fileName()));
                else if (!canceled)
                    emit notify(tr("Download of %1 failed: %2").arg(transfers_.fileName(id), error));
            });
    connect(storage, &StorageService::uploadProgress, this, [this](quint32 id, qint64 sent, qint64 total) {
        transfers_.progress(id, sent, total);
    });
    connect(storage, &StorageService::operationFinished, this,
            [this](quint32 id, const QString& path, const QString& error) {
                const QString devId = operationDevices_.take(id);
                if (batchFolders_.contains(id)) return onBatchFolder(id, path, error);
                if (!error.isEmpty()) emit notify(error);
                emit storageOperationFinished(id, devId, path, error);
            });
    connect(storage, &StorageService::folderChanged, this, &AppController::storageFolderChanged);
    connect(storage, &StorageService::operationProgress, this, [this](quint32 id, qint64 done, qint64 total) {
        emit storageOperationProgress(id, operationDevices_.value(id), done, total);
    });
    connect(storage, &StorageService::uploadFinished, this,
            [this](quint32 id, const QString& remotePath, const QString& error) {
                const bool canceled = canceled_.remove(id);
                const QString devId = transfers_.devId(id);
                transfers_.finish(id, remotePath, error, canceled);
                if (batchOfUpload_.contains(id)) {
                    if (error.isEmpty()) emit storageUploaded(devId, remotePath);
                    return onBatchUpload(id, error, canceled);
                }
                if (error.isEmpty()) {
                    emit notify(tr("Uploaded %1").arg(transfers_.fileName(id)));
                    emit storageUploaded(devId, remotePath);
                } else if (!canceled) {
                    emit notify(tr("Upload of %1 failed: %2").arg(transfers_.fileName(id), error));
                }
            });

    removeOldStaging(dragStagingRoot());
    removeOldStaging(openStagingRoot());

    worker_.setObjectName(QStringLiteral("uconnect-session"));
    worker_.start();
    pushHello();
    pushSharedFolders();
    pushUploadLimit();
    onSession([s = session_, id = *identity_] { s->setIdentity(id); });
    onSession([s = session_, key = keyToHex(identity_->publicKey()),
               dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/sync")] {
        s->configureSync(key, dir);
    });
    pushSync();
    pushAccessPolicy();
    if (!identityWarning.isEmpty()) appendLog(identityWarning);

    if (options.showDebug || debugOnStartup_) showDebugWindow();
    if (options.autoConnect || (autoConnect_ && !firstRun_ && topicLinkValid())) connectToTopic();
}

AppController::~AppController() {
    delete screen_; screen_ = nullptr;
    QMetaObject::invokeMethod(session_, [s = session_] { s->stop(); }, Qt::BlockingQueuedConnection);
    worker_.quit();
    worker_.wait();
}

template <class F>
void AppController::onSession(F&& f) {
    QMetaObject::invokeMethod(session_, std::forward<F>(f), Qt::QueuedConnection);
}

// --- settings ----------------------------------------------------------------

void AppController::setServerAddress(const QString& value) {
    const QString trimmed = value.trimmed();
    if (trimmed == serverAddress_) return;
    serverAddress_ = trimmed;
    QSettings().setValue(Key::Server, trimmed);
    emit settingsChanged();
}

void AppController::setDeviceName(const QString& value) {
    const QString trimmed = value.trimmed();
    if (trimmed == deviceName_ || trimmed.isEmpty()) return;
    deviceName_ = trimmed;
    QSettings().setValue(Key::DeviceName, trimmed);
    emit settingsChanged();
    pushHello();
}

void AppController::setServices(uint32_t services) {
    if (services == services_) return;
    services_ = services;
    QSettings().setValue(Key::Services, serviceNames(services_));
    emit settingsChanged();
    pushHello();
}

bool AppController::isValidServer(const QString& address) const {
    const QString s     = address.trimmed();
    const int     colon = static_cast<int>(s.lastIndexOf(QLatin1Char(':')));
    if (colon <= 0) return false;
    bool      ok   = false;
    const int port = s.mid(colon + 1).toInt(&ok);
    if (!ok || port < 1 || port > 65535) return false;
    static const QRegularExpression whitespace(QStringLiteral("\\s"));
    return !s.left(colon).contains(whitespace);
}

void AppController::completeFirstRun(const QString& server, const QString& name) {
    if (!isValidServer(server)) return;
    setServerAddress(server);
    setDeviceName(name);
    // Recorded even if unchanged, so the next start does not ask again.
    QSettings().setValue(Key::Server, serverAddress_);
    firstRun_ = false;
    emit firstRunChanged();
}

void AppController::applySettings(const QVariantMap& values) {
    if (values.contains("screen")) screen_->configure(values.value("screen").toMap());
    QSettings settings;
    if (values.contains(Key::Server) && isValidServer(values.value(Key::Server).toString()))
        setServerAddress(values.value(Key::Server).toString());
    if (values.contains(Key::DeviceName)) setDeviceName(values.value(Key::DeviceName).toString());
    if (values.contains(Key::Services))
        setServices(servicesFromNames(values.value(Key::Services).toStringList()));

    // The dialog uses public field names; Key::* names are the disk schema.
    if (values.contains(QStringLiteral("sharedFolders"))) {
        QStringList folders;
        for (const QString& f : values.value(QStringLiteral("sharedFolders")).toStringList())
            if (!f.trimmed().isEmpty() && !folders.contains(f)) folders << f;
        if (folders != sharedFolders_) {
            sharedFolders_ = folders;
            settings.setValue(Key::SharedFolders, folders);
            emit settingsChanged();
            pushSharedFolders();
        }
    }
    if (values.contains(QStringLiteral("readOnlyFolders")) || values.contains(QStringLiteral("sharedFolders"))) {
        // Only folders still shared; a removed one forgets the setting.
        const QStringList asked = values.contains(QStringLiteral("readOnlyFolders"))
                                      ? values.value(QStringLiteral("readOnlyFolders")).toStringList()
                                      : readOnlyFolders_;
        QStringList readOnly;
        for (const QString& f : asked)
            if (sharedFolders_.contains(f) && !readOnly.contains(f)) readOnly << f;
        if (readOnly != readOnlyFolders_) {
            readOnlyFolders_ = readOnly;
            settings.setValue(Key::ReadOnly, readOnly);
            emit settingsChanged();
            pushSharedFolders();
        }
    }
    if (values.contains(QStringLiteral("downloadFolder"))) {
        const QString folder = values.value(QStringLiteral("downloadFolder")).toString().trimmed();
        if (!folder.isEmpty() && folder != downloadFolder_) {
            downloadFolder_ = folder;
            settings.setValue(Key::DownloadFolder, folder);
            emit settingsChanged();
        }
    }

    if (values.contains(QStringLiteral("maxUploadMB"))) {
        const int mb = std::max(0, values.value(QStringLiteral("maxUploadMB")).toInt());
        if (mb != maxUploadMB_) {
            maxUploadMB_ = mb;
            settings.setValue(Key::MaxUploadMB, mb);
            emit settingsChanged();
            pushUploadLimit();
        }
    }

    bool trustTouched = false;
    if (values.contains(QStringLiteral("defaultPermissions"))) {
        const QStringList perms = knownOnly(values.value(QStringLiteral("defaultPermissions")).toStringList(), false);
        if (perms != defaultPermissions_) {
            defaultPermissions_ = perms;
            settings.setValue(Key::DefaultPerms, perms);
            trustTouched = true;
        }
    }
    for (const QString& key : values.value(QStringLiteral("untrust")).toStringList()) {
        if (trusted_.remove(key) > 0) {
            settings.remove(QStringLiteral("%1/%2").arg(Key::Trust, key));
            trustTouched = true;
        }
    }
    if (trustTouched) {
        emit trustChanged();
        pushAccessPolicy();
    }

    auto applyBool = [&](const char* key, bool& field) {
        if (!values.contains(key)) return;
        const bool v = values.value(key).toBool();
        if (v == field) return;
        field = v;
        settings.setValue(key, v);
        emit settingsChanged();
    };
    applyBool(Key::AutoConnect, autoConnect_);
    applyBool(Key::Verbose, verbose_);
    applyBool(Key::DebugOnStartup, debugOnStartup_);

    const bool rememberBefore = rememberTopic_;
    applyBool(Key::RememberTopic, rememberTopic_);
    if (rememberTopic_ != rememberBefore) {
        if (rememberTopic_) settings.setValue(Key::TopicLink, topicLink_);
        else settings.remove(Key::TopicLink);
    }
}

QObject* AppController::screen() const { return screen_; }

// --- topic ---------------------------------------------------------------------

void AppController::setTopicLink(const QString& value) {
    const QString trimmed = value.trimmed();
    if (trimmed == topicLink_) return;
    topicLink_ = trimmed;
    if (rememberTopic_) QSettings().setValue(Key::TopicLink, trimmed);
    emit topicLinkChanged();
}

bool AppController::topicLinkValid() const {
    const auto creds = uconnect::TopicCreds::parse(topicLink_.toStdString());
    return creds && creds->is_keyed();
}

void AppController::generateTopicLink() {
    setTopicLink(QString::fromStdString(uconnect::TopicCreds::generate_keyed().to_uri()));
    emit notify(tr("New protected topic created. Share its link only with your own devices."));
}

void AppController::pasteTopicLink() {
    setTopicLink(QGuiApplication::clipboard()->text());
}

void AppController::copyTopicLink() {
    copyText(topicLink_);
    emit notify(tr("Invite link copied. It contains the topic key."));
}

void AppController::copyTopicId() {
    copyText(snapshot_.topicId);
    emit notify(tr("Topic ID copied."));
}

void AppController::copyText(const QString& text) const {
    QGuiApplication::clipboard()->setText(text);
}

// --- session -------------------------------------------------------------------

QString AppController::status() const {
    switch (status_) {
    case Session::Status::Offline: return QStringLiteral("offline");
    case Session::Status::Connecting: return QStringLiteral("connecting");
    case Session::Status::Online: return QStringLiteral("online");
    case Session::Status::Reconnecting: return QStringLiteral("reconnecting");
    case Session::Status::Error: return QStringLiteral("error");
    }
    return QStringLiteral("offline");
}

bool AppController::active() const {
    return status_ == Session::Status::Connecting || status_ == Session::Status::Online ||
           status_ == Session::Status::Reconnecting;
}

bool AppController::debugAvailable() const {
#ifdef UCD_HAS_DEBUG_UI
    return true;
#else
    return false;
#endif
}

void AppController::connectToTopic() {
    Session::Options o{serverAddress_, topicLink_, verbose_};
    // Show progress immediately; the worker may be busy for a moment.
    status_       = Session::Status::Connecting;
    statusDetail_ = tr("Starting...");
    emit statusChanged();
    onSession([s = session_, o] { s->start(o); });
}

void AppController::disconnectFromTopic() {
    onSession([s = session_] { s->stop(); });
}

void AppController::connectPeer(const QString& devId) {
    onSession([s = session_, devId] { s->connectPeer(devId); });
}

void AppController::disconnectPeer(const QString& devId) {
    onSession([s = session_, devId] { s->disconnectPeer(devId); });
}

void AppController::refreshPeers() {
    onSession([s = session_] { s->refreshNow(); });
}

void AppController::showDebugWindow() {
#ifdef UCD_HAS_DEBUG_UI
    // QOpenGLWindow crashes inside Qt when no GL context can be made (headless
    // platforms, some VMs and remote sessions), so check before opening it.
    if (QOpenGLContext probe; !probe.create()) {
        emit notify(tr("The debug window needs OpenGL, which is not available here."));
        appendLog(QStringLiteral("Debug window unavailable: no OpenGL context"));
        return;
    }
    if (!debugWindow_) {
        debugWindow_ = std::make_unique<DebugWindow>(this);
        // A QOpenGLWindow that was closed never repaints when shown again, so
        // each close discards the window and the next open builds a new one.
        connect(debugWindow_.get(), &QWindow::visibleChanged, this, [this](bool visible) {
            if (!visible && debugWindow_) debugWindow_.release()->deleteLater();
        });
    }
    debugWindow_->show();
    debugWindow_->raise();
    debugWindow_->requestActivate();
#endif
}

void AppController::requestServerStats() {
    serverReport_ = tr("Asking the server...");
    onSession([s = session_] { s->requestServerStats(); });
}

void AppController::requestTopicListing() {
    serverReport_ = tr("Asking the server...");
    onSession([s = session_] { s->requestTopicListing(); });
}

quint32 AppController::storageList(const QString& devId, const QString& path) {
    const quint32 id = nextRequestId_++;
    onSession([s = session_, devId, path, id] { s->storageList(devId, path, id); });
    return id;
}

quint32 AppController::storageDownload(const QString& devId, const QString& remotePath) {
    const quint32 id = nextRequestId_++;
    transfers_.add(id, devId, remotePath, {}, false, downloadFolder_);
    onSession([s = session_, devId, remotePath, folder = downloadFolder_, id] {
        s->storageDownload(devId, remotePath, folder, id);
    });
    return id;
}

quint32 AppController::storageWatch(const QString& devId, const QString& path) {
    const quint32 id = nextRequestId_++;
    onSession([s = session_, devId, path, id] { s->storageWatch(devId, path, id); });
    return id;
}

void AppController::storageUnwatch(quint32 watchId) {
    onSession([s = session_, watchId] { s->storageUnwatch(watchId); });
}

void AppController::retryTransfer(quint32 requestId) {
    auto retry = transfers_.retryOf(requestId);
    if (!retry) return;
    // A device that restarted is back under a new id; find it by its identity.
    const auto& peers = peers_.peers();
    auto connected = [](const PeerRecord& p) { return p.stateCode == static_cast<int>(uconnect::PeerState::Connected); };
    const bool stillThere = std::any_of(peers.begin(), peers.end(), [&](const PeerRecord& p) {
        return p.devId == retry->devId && connected(p);
    });
    const QString identity = identityOf_.value(retry->devId);
    if (!stillThere && !identity.isEmpty()) {
        const auto back = std::find_if(peers.begin(), peers.end(), [&](const PeerRecord& p) {
            return p.identityKey == identity && connected(p);
        });
        if (back != peers.end()) retry->devId = back->devId;
    }
    transfers_.remove(requestId);
    const quint32 id = nextRequestId_++;
    transfers_.add(id, retry->devId, retry->upload ? joinRemote(retry->folder, QFileInfo(retry->localPath).fileName())
                                                   : retry->remotePath,
                   retry->localPath, retry->upload, retry->folder, retry->replace);
    if (retry->upload) {
        onSession([s = session_, r = *retry, id] { s->storageUpload(r.devId, r.localPath, r.folder, id, r.replace); });
    } else {
        onSession([s = session_, r = *retry, id] { s->storageDownload(r.devId, r.remotePath, r.folder, id); });
    }
}

void AppController::storageCancel(quint32 requestId) {
    canceled_.insert(requestId);
    onSession([s = session_, requestId] { s->storageCancel(requestId); });
}

// --- batch downloads ---------------------------------------------------------------

quint32 AppController::storageDownloadItems(const QString& devId, const QString& folder, const QVariantList& items) {
    return startBatch(devId, folder, items, downloadFolder_, false);
}

quint32 AppController::prepareDragOut(const QString& devId, const QString& folder, const QVariantList& items) {
    const QString staging = newStagingFolder(dragStagingRoot(), nextRequestId_);
    const quint32 batch = startBatch(devId, folder, items, staging, true);
    if (batches_.contains(batch)) batches_[batch].staging = staging;
    return batch;
}

quint32 AppController::storagePreview(const QString& devId, const QString& folder, const QString& name) {
    const QString staging = newStagingFolder(openStagingRoot(), nextRequestId_);
    const quint32 batch = startBatch(devId, folder, {QVariantMap{{"name", name}, {"dir", false}}}, staging, true);
    if (batches_.contains(batch)) {
        batches_[batch].staging = staging;
        batches_[batch].preview = true;
    }
    return batch;
}

QVariantMap AppController::previewText(const QUrl& url) const {
    constexpr qint64 kLimit = 256 * 1024;
    QFile file(url.toLocalFile());
    if (!file.open(QIODevice::ReadOnly)) return {{"binary", true}};
    const QByteArray bytes = file.read(kLimit);
    const bool truncated = file.size() > kLimit;
    if (bytes.left(8192).contains('\0')) return {{"binary", true}};
    QStringDecoder utf8(QStringDecoder::Utf8);
    QString text = utf8.decode(truncated ? bytes.left(bytes.size() - 4) : bytes);  // a cut may split a character
    if (utf8.hasError()) return {{"binary", true}};
    return {{"text", text}, {"truncated", truncated}, {"binary", false}};
}

quint32 AppController::storageOpen(const QString& devId, const QString& folder, const QString& name) {
    // A folder of its own per open, so the file keeps its name.
    const QString staging = newStagingFolder(openStagingRoot(), nextRequestId_);
    const quint32 batch = startBatch(devId, folder, {QVariantMap{{"name", name}, {"dir", false}}}, staging, true);
    if (batches_.contains(batch)) {
        batches_[batch].staging = staging;
        batches_[batch].open    = true;
    }
    return batch;
}

void AppController::cancelBatch(quint32 batchId) {
    if (!batches_.contains(batchId)) return;
    batches_[batchId].canceled = true;
    for (quint32 id : batches_[batchId].downloads)
        if (batchOfDownload_.contains(id)) storageCancel(id);
    finishBatchIfDone(batchId);
}

quint32 AppController::startBatch(const QString& devId, const QString& folder, const QVariantList& items,
                                  const QString& localFolder, bool quiet) {
    const quint32 batch = nextRequestId_++;
    batches_[batch].devId = devId;
    batches_[batch].quiet = quiet;
    QDir().mkpath(localFolder);
    for (const QVariant& value : items) {
        const QVariantMap item = value.toMap();
        const QString     name = item.value("name").toString();
        if (name.isEmpty() || name.contains(QLatin1Char('/'))) continue;
        const int slot = static_cast<int>(batches_[batch].topLevel.size());
        batches_[batch].topLevel << QString();
        if (item.value("dir").toBool()) {
            // A whole folder: never merged into one that is already there.
            const QString local = uniqueLocalPath(localFolder, safeFileName(name));
            QDir().mkpath(local);
            batches_[batch].topLevel[slot] = local;
            listForBatch(batch, joinRemote(folder, name), local);
        } else {
            downloadForBatch(batch, joinRemote(folder, name), localFolder, slot);
        }
    }
    finishBatchIfDone(batch);
    return batch;
}

void AppController::listForBatch(quint32 batchId, const QString& remoteDir, const QString& localDir) {
    const quint32 id = nextRequestId_++;
    batchLists_.insert(id, {batchId, remoteDir, localDir});
    ++batches_[batchId].pending;
    onSession([s = session_, devId = batches_[batchId].devId, remoteDir, id] { s->storageList(devId, remoteDir, id); });
}

void AppController::downloadForBatch(quint32 batchId, const QString& remotePath, const QString& localDir,
                                     int topSlot) {
    DownloadBatch& batch = batches_[batchId];
    if (batch.filesKnown >= kMaxBatchFiles) {
        ++batch.failed;
        return;
    }
    const quint32 id = nextRequestId_++;
    ++batch.pending;
    ++batch.filesKnown;
    batch.downloads << id;
    batchOfDownload_.insert(id, batchId);
    if (topSlot >= 0) topSlotOfDownload_.insert(id, topSlot);
    transfers_.add(id, batch.devId, remotePath, {}, false, localDir);
    onSession([s = session_, devId = batch.devId, remotePath, localDir, id] {
        s->storageDownload(devId, remotePath, localDir, id);
    });
    emit storageBatchProgress(batchId, batch.filesDone, batch.filesKnown);
}

void AppController::onBatchList(quint32 listId, const QVariantList& entries, const QString& error) {
    const BatchList list = batchLists_.take(listId);
    if (!batches_.contains(list.batch)) return;
    --batches_[list.batch].pending;
    if (!error.isEmpty()) {
        ++batches_[list.batch].failed;
    } else if (!batches_[list.batch].canceled) {
        for (const QVariant& value : entries) {
            const QVariantMap entry = value.toMap();
            const QString     name  = entry.value("name").toString();
            if (name.isEmpty() || name.contains(QLatin1Char('/'))) continue;
            const QString remote = joinRemote(list.remoteDir, name);
            if (entry.value("dir").toBool()) {
                const QString local = QDir(list.localDir).filePath(safeFileName(name));
                QDir().mkpath(local);
                listForBatch(list.batch, remote, local);
            } else {
                downloadForBatch(list.batch, remote, list.localDir, -1);
            }
        }
    }
    finishBatchIfDone(list.batch);
}

void AppController::onBatchDownload(quint32 downloadId, const QString& localPath, const QString& error,
                                    bool canceled) {
    const quint32 batchId = batchOfDownload_.take(downloadId);
    const int     slot    = topSlotOfDownload_.value(downloadId, -1);  // -1: inside a folder
    topSlotOfDownload_.remove(downloadId);
    if (!batches_.contains(batchId)) return;
    DownloadBatch& batch = batches_[batchId];
    --batch.pending;
    ++batch.filesDone;
    if (!error.isEmpty() && !canceled) {
        ++batch.failed;
        if (batch.firstError.isEmpty()) batch.firstError = error;
    }
    if (error.isEmpty() && slot >= 0 && slot < batch.topLevel.size()) batch.topLevel[slot] = localPath;
    emit storageBatchProgress(batchId, batch.filesDone, batch.filesKnown);
    finishBatchIfDone(batchId);
}

void AppController::finishBatchIfDone(quint32 batchId) {
    const auto it = batches_.find(batchId);
    if (it == batches_.end() || it->pending > 0) return;
    const DownloadBatch batch = *it;
    batches_.erase(it);

    QStringList urls;
    for (const QString& path : batch.topLevel)
        if (!path.isEmpty() && QFileInfo::exists(path)) urls << QUrl::fromLocalFile(path).toString();
    if (batch.canceled && !batch.staging.isEmpty()) QDir(batch.staging).removeRecursively();

    if (batch.preview && !batch.canceled)
        emit storagePreviewReady(batchId, urls.value(0), batch.failed == 0 && !urls.isEmpty() ? QString()
                                                         : batch.firstError.isEmpty() ? tr("Could not fetch the file.")
                                                                                      : batch.firstError);

    if (batch.open && !batch.canceled) {
        // A copy: changes made to it stay on this device.
        if (batch.failed == 0 && !urls.isEmpty()) {
            if (!QDesktopServices::openUrl(QUrl(urls.first())))
                emit notify(tr("No app is set up to open %1.").arg(QFileInfo(batch.topLevel.first()).fileName()));
        } else {
            emit notify(tr("Could not open the file: %1").arg(batch.firstError));
        }
    }

    if (!batch.quiet && !batch.canceled) {
        const int ok = batch.filesDone - batch.failed;
        if (batch.failed == 0)
            emit notify(ok == 1 ? tr("Downloaded 1 file") : tr("Downloaded %1 files").arg(ok));
        else
            emit notify(tr("Downloaded %1 of %2 files; %3 failed").arg(ok).arg(batch.filesKnown).arg(batch.failed));
    }
    emit storageBatchFinished(batchId, batch.failed, batch.canceled, urls);
}

quint32 AppController::startOperation(const QString& devId, const QJsonObject& request) {
    const quint32 id = nextRequestId_++;
    operationDevices_.insert(id, devId);
    onSession([s = session_, devId, request, id] { s->storageOperation(devId, request, id); });
    return id;
}

quint32 AppController::storageRename(const QString& devId, const QString& path, const QString& newName) {
    return startOperation(devId, {{"t", "fs.rename"}, {"path", path}, {"name", newName.trimmed()}});
}

int AppController::storageDelete(const QString& devId, const QStringList& paths) {
    for (const QString& path : paths) startOperation(devId, {{"t", "fs.delete"}, {"path", path}});
    return static_cast<int>(paths.size());
}

quint32 AppController::storageMakeFolder(const QString& devId, const QString& folder, const QString& name) {
    return startOperation(devId, {{"t", "fs.mkdir"}, {"path", folder}, {"name", name.trimmed()}});
}

QVariantList AppController::storageMove(const QString& devId, const QStringList& paths, const QString& toFolder) {
    QVariantList ids;
    for (const QString& path : paths)
        ids << startOperation(devId, {{"t", "fs.move"}, {"path", path}, {"to", toFolder}});
    return ids;
}

QVariantList AppController::storageCopy(const QString& devId, const QStringList& paths, const QString& toFolder) {
    QVariantList ids;
    for (const QString& path : paths)
        ids << startOperation(devId, {{"t", "fs.copy"}, {"path", path}, {"to", toFolder}});
    return ids;
}

int AppController::storageUpload(const QString& devId, const QString& remoteFolder, const QList<QUrl>& files,
                                 bool replace) {
    if (remoteFolder == QLatin1String("/")) {
        emit notify(tr("Open a shared folder before uploading."));
        return 0;
    }
    // With folders in the drop, everything goes in one batch with one summary.
    const bool anyFolder = std::any_of(files.begin(), files.end(), [](const QUrl& url) {
        return url.isLocalFile() && QFileInfo(url.toLocalFile()).isDir();
    });
    const quint32 batch = anyFolder ? nextRequestId_++ : 0;
    if (anyFolder) {
        uploadBatches_[batch].devId   = devId;
        uploadBatches_[batch].replace = replace;
    }

    int added = 0;
    int skipped = 0;
    QSet<QString> seen;
    for (const QUrl& url : files) {
        const QFileInfo info(url.toLocalFile());
        if (!url.isLocalFile() || !info.exists() || !info.isReadable()) { ++skipped; continue; }
        if (seen.contains(info.absoluteFilePath())) continue;
        seen.insert(info.absoluteFilePath());
        if (info.isDir()) folderForBatch(batch, remoteFolder, info.absoluteFilePath(), true);
        else uploadFile(devId, info.absoluteFilePath(), remoteFolder, replace, batch);
        ++added;
    }
    if (skipped > 0) emit notify(tr("Skipped %n item(s) that could not be read.", nullptr, skipped));
    if (anyFolder) finishUploadBatchIfDone(batch);
    return added;
}

void AppController::uploadFile(const QString& devId, const QString& localPath, const QString& remoteFolder,
                               bool replace, quint32 batchId) {
    if (batchId != 0) {
        UploadBatch& batch = uploadBatches_[batchId];
        if (batch.files >= kMaxBatchFiles || !QFileInfo(localPath).isReadable()) {
            ++batch.failed;
            if (batch.firstError.isEmpty())
                batch.firstError = batch.files >= kMaxBatchFiles
                                       ? tr("only the first %1 files are uploaded").arg(kMaxBatchFiles)
                                       : tr("could not read %1").arg(QFileInfo(localPath).fileName());
            return;
        }
        ++batch.files;
        ++batch.pending;
    }
    const quint32 id = nextRequestId_++;
    if (batchId != 0) batchOfUpload_.insert(id, batchId);
    transfers_.add(id, devId, joinRemote(remoteFolder, QFileInfo(localPath).fileName()), localPath, true,
                   remoteFolder, replace);
    onSession([s = session_, devId, localPath, remoteFolder, id, replace] {
        s->storageUpload(devId, localPath, remoteFolder, id, replace);
    });
}

// Creates the folder on the device (or, when replacing, reuses one of that
// name), then uploads what is in it; subfolders follow the same way.
void AppController::folderForBatch(quint32 batchId, const QString& remoteParent, const QString& localDir, bool top) {
    UploadBatch& batch = uploadBatches_[batchId];
    ++batch.pending;
    QJsonObject request{{"t", "fs.mkdir"}, {"path", remoteParent},
                        {"name", safeFileName(QFileInfo(localDir).fileName())}};
    if (batch.replace) request.insert("existing", true);
    const quint32 id = startOperation(batch.devId, request);
    batchFolders_.insert(id, {batchId, localDir, top});
}

void AppController::onBatchFolder(quint32 opId, const QString& remotePath, const QString& error) {
    const BatchFolder step = batchFolders_.take(opId);
    if (!uploadBatches_.contains(step.batch)) return;
    UploadBatch& batch = uploadBatches_[step.batch];
    --batch.pending;
    if (!error.isEmpty()) {
        ++batch.failed;
        batch.firstError = batch.firstError.isEmpty() ? error : batch.firstError;
        return finishUploadBatchIfDone(step.batch);
    }
    if (step.top) {
        batch.topFolders << remotePath;
        emit storageOperationFinished(opId, batch.devId, remotePath, {});  // the view shows the new folder
    }
    const QString devId = batch.devId;  // `batch` may move as the hash grows
    const bool    replace = batch.replace;
    const QFileInfoList children = QDir(step.localDir).entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDir::DirsFirst | QDir::Name);
    for (const QFileInfo& child : children) {
        if (child.isDir()) {
            // A linked folder could lead back up the tree; it is not followed.
            if (child.isSymLink() || child.isJunction()) continue;
            folderForBatch(step.batch, remotePath, child.absoluteFilePath(), false);
        } else {
            uploadFile(devId, child.absoluteFilePath(), remotePath, replace, step.batch);
        }
    }
    finishUploadBatchIfDone(step.batch);
}

void AppController::onBatchUpload(quint32 uploadId, const QString& error, bool canceled) {
    const quint32 batchId = batchOfUpload_.take(uploadId);
    if (!uploadBatches_.contains(batchId)) return;
    UploadBatch& batch = uploadBatches_[batchId];
    --batch.pending;
    if (error.isEmpty()) {
        ++batch.uploaded;
    } else if (!canceled) {
        ++batch.failed;
        batch.firstError = batch.firstError.isEmpty() ? error : batch.firstError;
    }
    finishUploadBatchIfDone(batchId);
}

void AppController::finishUploadBatchIfDone(quint32 batchId) {
    const auto it = uploadBatches_.find(batchId);
    if (it == uploadBatches_.end() || it->pending > 0) return;
    const UploadBatch batch = *it;
    uploadBatches_.erase(it);
    if (batch.failed == 0)
        emit notify(batch.topFolders.size() == 1
                        ? tr("Uploaded %1 (%n file(s))", nullptr, batch.uploaded)
                              .arg(batch.topFolders.first().section(QLatin1Char('/'), -1))
                        : tr("Uploaded %n file(s)", nullptr, batch.uploaded));
    else
        emit notify(tr("Upload finished with %n problem(s): %1", nullptr, batch.failed).arg(batch.firstError));
}

void AppController::showInFolder(const QString& localPath) const {
    const QFileInfo fi(localPath);
    QDesktopServices::openUrl(QUrl::fromLocalFile(fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath()));
}

QString AppController::localPath(const QUrl& url) const {
    return url.toLocalFile();
}

// --- identity and permissions ------------------------------------------------

QVariantList AppController::permissionDefinitions() const {
    return permissionsForQml();
}

QVariantList AppController::trustedDevices() const {
    QVariantList out;
    for (auto it = trusted_.begin(); it != trusted_.end(); ++it) {
        out.push_back(QVariantMap{
            {"key", it.key()},
            {"fingerprint", fingerprintOfHex(it.key())},
            {"name", it->name},
            {"permissions", it->permissions},
            {"since", it->since.toString(Qt::ISODate)},
        });
    }
    std::sort(out.begin(), out.end(), [](const QVariant& a, const QVariant& b) {
        return a.toMap().value("name").toString().compare(b.toMap().value("name").toString(), Qt::CaseInsensitive) < 0;
    });
    return out;
}

void AppController::trustDevice(const QString& key, const QString& name) {
    if (!keyFromHex(key) || trusted_.contains(key)) return;
    trusted_.insert(key, {name.left(64), defaultPermissions_, QDateTime::currentDateTimeUtc()});
    saveTrusted(key);
    emit trustChanged();
    pushAccessPolicy();
    emit notify(tr("%1 is now trusted.").arg(name));
}

void AppController::untrustDevice(const QString& key) {
    const auto it = trusted_.find(key);
    if (it == trusted_.end()) return;
    const QString name = it->name;
    trusted_.erase(it);
    QSettings().remove(QStringLiteral("%1/%2").arg(Key::Trust, key));
    emit trustChanged();
    pushAccessPolicy();
    emit notify(tr("%1 is no longer trusted.").arg(name));
}

void AppController::setDevicePermission(const QString& key, const QString& permission, bool granted) {
    const auto it = trusted_.find(key);
    if (it == trusted_.end() || !isKnownPermission(permission)) return;
    QStringList perms = it->permissions;
    perms.removeAll(permission);
    if (granted) perms << permission;
    perms.sort();
    if (perms == it->permissions) return;
    it->permissions = perms;
    saveTrusted(key);
    emit trustChanged();
    pushAccessPolicy();
}

void AppController::loadTrust() {
    QSettings settings;
    defaultPermissions_ = knownOnly(settings.value(Key::DefaultPerms).toStringList(), false);
    settings.beginGroup(Key::Trust);
    for (const QString& key : settings.childGroups()) {
        if (!keyFromHex(key)) continue;
        settings.beginGroup(key);
        trusted_.insert(key, {settings.value("name").toString(),
                              knownOnly(settings.value("permissions").toStringList()),
                              settings.value("since").toDateTime()});
        settings.endGroup();
    }
}

void AppController::saveTrusted(const QString& key) {
    const auto it = trusted_.constFind(key);
    if (it == trusted_.constEnd()) return;
    QSettings settings;
    settings.beginGroup(QStringLiteral("%1/%2").arg(Key::Trust, key));
    settings.setValue("name", it->name);
    settings.setValue("permissions", it->permissions);
    settings.setValue("since", it->since);
}

void AppController::pushAccessPolicy() {
    AccessPolicy policy;
    policy.defaults = PermissionSet(defaultPermissions_.begin(), defaultPermissions_.end());
    for (auto it = trusted_.begin(); it != trusted_.end(); ++it)
        policy.trusted.insert(it.key(), PermissionSet(it->permissions.begin(), it->permissions.end()));
    onSession([s = session_, policy] { s->setAccessPolicy(policy); });
}

void AppController::pushSharedFolders() {
    onSession([s = session_, folders = sharedFolders_, readOnly = readOnlyFolders_] {
        s->setSharedFolders(folders, readOnly);
    });
}

void AppController::pushUploadLimit() {
    onSession([s = session_, bytes = qint64(maxUploadMB_) * 1024 * 1024] { s->setUploadLimit(bytes); });
}

void AppController::pushHello() {
    Hello h{deviceName_, localOsName(), services_};
    onSession([s = session_, h] { s->setLocalHello(h); });
}

void AppController::addActivity(const QVariantMap& entry) {
    activity_.prepend(entry);
    while (activity_.size() > kMaxActivity) activity_.removeLast();
    emit storageActivityChanged();
    QDir().mkpath(QFileInfo(activityFile()).absolutePath());
    QFile file(activityFile());
    if (file.open(QIODevice::Append | QIODevice::Text))
        file.write(QJsonDocument(QJsonObject::fromVariantMap(entry)).toJson(QJsonDocument::Compact) + '\n');
}

void AppController::loadActivity() {
    QFile file(activityFile());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return;
    const QList<QByteArray> lines = file.readAll().split('\n');
    file.close();
    for (const QByteArray& line : lines) {
        const QJsonDocument doc = QJsonDocument::fromJson(line);
        if (doc.isObject()) activity_.prepend(doc.object().toVariantMap());
    }
    // Keep the file from growing without end.
    if (activity_.size() > kMaxActivity) {
        activity_ = activity_.mid(0, kMaxActivity);
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
            for (auto it = activity_.crbegin(); it != activity_.crend(); ++it)
                file.write(QJsonDocument(QJsonObject::fromVariantMap(it->toMap())).toJson(QJsonDocument::Compact) + '\n');
    }
}

void AppController::clearStorageActivity() {
    activity_.clear();
    QFile::remove(activityFile());
    emit storageActivityChanged();
}

// --- folder sync -----------------------------------------------------------------

void AppController::loadSync() {
    QSettings settings;
    settings.beginGroup(QStringLiteral("sync"));
    for (const QString& id : settings.childGroups()) {
        if (QByteArray::fromHex(id.toLatin1()).size() != 16) continue;
        settings.beginGroup(id);
        sync::FolderConfig c;
        c.id     = id;
        c.label  = settings.value("label").toString();
        c.path   = settings.value("path").toString();
        c.paused = settings.value("paused", false).toBool();
        const QStringList receive    = settings.value("receive").toStringList();
        const QStringList contribute = settings.value("contribute").toStringList();
        for (const QString& key : receive) c.peers[key].receive = true;
        for (const QString& key : contribute) c.peers[key].contribute = true;
        settings.endGroup();
        if (!c.path.isEmpty()) syncFolders_ << c;
    }
}

void AppController::saveSync() const {
    QSettings settings;
    settings.remove(QStringLiteral("sync"));
    settings.beginGroup(QStringLiteral("sync"));
    for (const sync::FolderConfig& c : syncFolders_) {
        settings.beginGroup(c.id);
        settings.setValue("label", c.label);
        settings.setValue("path", c.path);
        settings.setValue("paused", c.paused);
        QStringList receive, contribute;
        for (auto it = c.peers.begin(); it != c.peers.end(); ++it) {
            if (it->receive) receive << it.key();
            if (it->contribute) contribute << it.key();
        }
        settings.setValue("receive", receive);
        settings.setValue("contribute", contribute);
        settings.endGroup();
    }
}

void AppController::pushSync() {
    onSession([s = session_, folders = syncFolders_] { s->setSyncFolders(folders); });
    emit syncChanged();
}

QString AppController::deviceName(const QString& key) const {
    if (const auto t = trusted_.constFind(key); t != trusted_.cend() && !t->name.isEmpty()) return t->name;
    for (const PeerRecord& p : peers_.peers())
        if (p.identityKey == key && !p.name.isEmpty()) return p.name;
    return tr("Unknown device");
}

QVariantList AppController::syncFolders() const {
    QSet<QString> online;
    for (const PeerRecord& p : peers_.peers())
        if (!p.identityKey.isEmpty() && p.stateCode == static_cast<int>(uconnect::PeerState::Connected))
            online.insert(p.identityKey);
    QVariantList out;
    for (const sync::FolderConfig& c : syncFolders_) {
        QVariantMap folder = syncStatus_.value(c.id);
        folder.insert("id", c.id);
        folder.insert("label", c.label);
        folder.insert("path", QDir::toNativeSeparators(c.path));
        folder.insert("paused", c.paused);
        if (!folder.contains("state")) folder.insert("state", c.paused ? "paused" : "scanning");
        // Every trusted device can be made a member; members no longer
        // trusted are listed too, as they still hold grants here.
        QStringList keys = trusted_.keys();
        for (auto it = c.peers.begin(); it != c.peers.end(); ++it)
            if (!keys.contains(it.key())) keys << it.key();
        QVariantList members;
        for (const QString& key : keys) {
            const sync::Grant g = c.peers.value(key);
            members << QVariantMap{{"key", key}, {"name", deviceName(key)}, {"fingerprint", fingerprintOfHex(key)},
                                   {"receive", g.receive}, {"contribute", g.contribute},
                                   {"trusted", trusted_.contains(key)}, {"online", online.contains(key)}};
        }
        folder.insert("members", members);
        // Conflicts with device names for the keys.
        QVariantList conflicts;
        for (const QVariant& v : folder.value("conflicts").toList()) {
            QVariantMap conflict = v.toMap();
            conflict.insert("followingName", deviceName(conflict.value("following").toString()));
            QVariantList candidates;
            for (const QVariant& cv : conflict.value("candidates").toList()) {
                QVariantMap candidate = cv.toMap();
                candidate.insert("name", candidate.value("mine").toBool() ? tr("This device")
                                                                          : deviceName(candidate.value("key").toString()));
                candidates << candidate;
            }
            conflict.insert("candidates", candidates);
            conflicts << conflict;
        }
        folder.insert("conflicts", conflicts);
        QVariantList held;
        for (const QVariant& v : folder.value("held").toList()) {
            QVariantMap h = v.toMap();
            h.insert("text", heldText(h.value("path").toString(), h.value("reason").toString()));
            held << h;
        }
        folder.insert("held", held);
        out << folder;
    }
    return out;
}

QVariantList AppController::syncOffers() const {
    QVariantList out;
    for (auto it = syncOffers_.cbegin(); it != syncOffers_.cend(); ++it) {
        for (const QVariant& v : *it) {
            const QVariantMap offer = v.toMap();
            const QString     id    = offer.value("id").toString();
            const bool joined = std::any_of(syncFolders_.begin(), syncFolders_.end(),
                                            [&](const sync::FolderConfig& c) { return c.id == id; });
            out << QVariantMap{{"key", it.key()}, {"device", deviceName(it.key())}, {"id", id},
                               {"label", offer.value("label")}, {"joined", joined}};
        }
    }
    return out;
}

QString AppController::createSyncFolder(const QUrl& folder, const QString& label) {
    const QString path = folder.toLocalFile();
    if (path.isEmpty() || !QFileInfo(path).isDir()) return {};
    sync::FolderConfig c;
    c.id    = QString::fromLatin1(QUuid::createUuid().toRfc4122().toHex());
    c.label = label.trimmed().isEmpty() ? QFileInfo(path).fileName() : label.trimmed().left(100);
    c.path  = path;
    syncFolders_ << c;
    saveSync();
    pushSync();
    return c.id;
}

bool AppController::joinSyncFolder(const QString& key, const QString& id, const QString& label, const QUrl& folder) {
    const QString path = folder.toLocalFile();
    if (path.isEmpty() || !QFileInfo(path).isDir() || !trusted_.contains(key)) return false;
    if (std::any_of(syncFolders_.begin(), syncFolders_.end(), [&](const sync::FolderConfig& c) { return c.id == id; }))
        return false;
    sync::FolderConfig c;
    c.id    = id;
    c.label = label.left(100);
    c.path  = path;
    c.peers.insert(key, {true, true});
    syncFolders_ << c;
    saveSync();
    pushSync();
    emit notify(tr("Joined %1. Files already in the folder are merged with it.").arg(c.label));
    return true;
}

void AppController::leaveSyncFolder(const QString& id) {
    const auto it = std::find_if(syncFolders_.begin(), syncFolders_.end(),
                                 [&](const sync::FolderConfig& c) { return c.id == id; });
    if (it == syncFolders_.end()) return;
    const QString label = it->label;
    syncFolders_.erase(it);
    syncStatus_.remove(id);
    saveSync();
    pushSync();
    emit notify(tr("Stopped syncing %1. Its files stay where they are.").arg(label));
}

void AppController::setSyncMember(const QString& id, const QString& key, bool receive, bool contribute) {
    for (sync::FolderConfig& c : syncFolders_) {
        if (c.id != id) continue;
        if (!receive && !contribute) c.peers.remove(key);
        else c.peers.insert(key, {receive, contribute});
        saveSync();
        pushSync();
        return;
    }
}

void AppController::setSyncPaused(const QString& id, bool paused) {
    for (sync::FolderConfig& c : syncFolders_) {
        if (c.id != id || c.paused == paused) continue;
        c.paused = paused;
        saveSync();
        pushSync();
        return;
    }
}

void AppController::resolveSyncConflict(const QString& id, const QString& path, const QString& choice,
                                        const QString& key) {
    onSession([s = session_, id, path, choice, key] { s->sync()->resolve(id, path, choice, key); });
}

void AppController::rescanSyncFolder(const QString& id) {
    onSession([s = session_, id] { s->sync()->rescan(id); });
}

void AppController::openSyncFolder(const QString& id) const {
    for (const sync::FolderConfig& c : syncFolders_)
        if (c.id == id) QDesktopServices::openUrl(QUrl::fromLocalFile(c.path));
}

void AppController::openSyncKept(const QString& id) {
    for (const sync::FolderConfig& c : syncFolders_) {
        if (c.id != id) continue;
        const QString kept = QDir(c.path).filePath(QLatin1String(sync::kKeptDir));
        if (QFileInfo(kept).isDir()) QDesktopServices::openUrl(QUrl::fromLocalFile(kept));
        else emit notify(tr("Nothing has been removed from %1 by syncing yet.").arg(c.label));
    }
}

void AppController::clearSyncEvents() {
    syncEvents_.clear();
    emit syncChanged();
}

QString AppController::heldText(const QString& path, const QString& reason) const {
    if (reason.startsWith(QLatin1String("clash:")))
        return tr("Not synced: its name differs only in case from %1, which is older and is synced. "
                  "Devices that ignore case can hold only one; this one syncs once %1 is gone.")
            .arg(reason.mid(6));
    const QString problem = reason.mid(5);  // after "name:"
    QString why;
    if (problem.startsWith(QLatin1String("char:")))
        why = tr("“%1” cannot be used in names on every system").arg(problem.mid(5));
    else if (problem == QLatin1String("control"))
        why = tr("its name holds a control character");
    else if (problem == QLatin1String("trailing"))
        why = tr("its name ends with a dot or a space, which Windows drops");
    else if (problem == QLatin1String("reserved"))
        why = tr("Windows keeps that name for a device");
    else if (problem == QLatin1String("long"))
        why = tr("its name is longer than 255 characters");
    else if (problem == QLatin1String("unicode"))
        why = tr("its name is written in a Unicode form some systems store differently");
    else
        why = tr("its name cannot be used");
    Q_UNUSED(path);
    return tr("Not synced: %1. Rename it to sync it.").arg(why);
}

void AppController::addSyncEvent(const QString& folderId, const QString& path, const QString& kind) {
    QString label;
    for (const sync::FolderConfig& c : syncFolders_)
        if (c.id == folderId) label = c.label;
    QString text;
    if (kind == QLatin1String("conflict"))
        text = tr("Your change to %1 conflicts with another device's. Choose which to keep.").arg(path);
    else if (kind.startsWith(QLatin1String("following:")))
        text = tr("Two other devices changed %1 differently; following %2, connected longest. You can switch.")
                   .arg(path, deviceName(kind.section(QLatin1Char(':'), 1)));
    else if (kind == QLatin1String("kept"))
        text = tr("Other devices changed %1 differently; keeping the version you have. You can switch.").arg(path);
    else if (kind == QLatin1String("waiting"))
        text = tr("Other devices changed %1 differently; waiting for one of them to connect.").arg(path);
    else if (kind == QLatin1String("blocked:changed"))
        text = tr("%1 changed here while it was being synced; looking at it again.").arg(path);
    else if (kind == QLatin1String("blocked:not_empty"))
        text = tr("Did not delete the folder %1: it holds files that are not synced.").arg(path);
    else if (kind == QLatin1String("blocked:in_the_way"))
        text = tr("Could not create %1: something else is in the way.").arg(path);
    else if (kind.startsWith(QLatin1String("held:")))
        text = path + QStringLiteral(" — ") + heldText(path, kind.mid(5));
    else if (kind == QLatin1String("blocked:clash"))
        text = tr("%1 from another device was not written: a file or folder here has the same name in "
                  "another case, and this device can hold only one.").arg(path);
    else if (kind == QLatin1String("blocked:outside"))
        text = tr("Refused %1: it would be outside the synced folder.").arg(path);
    else
        text = tr("Could not write %1.").arg(path);
    syncEvents_.prepend(QVariantMap{{"time", QDateTime::currentDateTime().toString(Qt::ISODate)},
                                    {"folderId", folderId}, {"folder", label}, {"path", path},
                                    {"kind", kind.section(QLatin1Char(':'), 0, 0)}, {"text", text}});
    while (syncEvents_.size() > kMaxSyncEvents) syncEvents_.removeLast();
    if (kind == QLatin1String("conflict") || kind.startsWith(QLatin1String("following:")))
        emit notify(tr("%1: %2").arg(label, text));
    emit syncChanged();
}

void AppController::appendLog(const QString& line) {
    qInfo().noquote() << "[session]" << line;
    log_.push_back(line);
    while (log_.size() > kMaxLogLines) log_.pop_front();
}

}  // namespace ucd
