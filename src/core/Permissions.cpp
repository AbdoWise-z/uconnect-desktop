#include "Permissions.hpp"

#include <algorithm>

#include <QCoreApplication>
#include <QVariantMap>

namespace ucd {
namespace {

QString tr(const char* text) {
    return QCoreApplication::translate("Permissions", text);
}

}  // namespace

const std::vector<PermissionInfo>& permissionTable() {
    static const std::vector<PermissionInfo> table{
        {perm::StorageObserve, "storage", tr("Observe"),
         tr("Browse the shared folders and see the names, sizes and dates of the files in them."), true},
        {perm::StorageRead, "storage", tr("Read"),
         tr("Open files and save copies of them to their device."), true},
        {perm::StorageWrite, "storage", tr("Write"),
         tr("Rename, move, replace or delete existing files and folders."), true},
        {perm::StorageUpload, "storage", tr("Upload"),
         tr("Add new files and folders, and copy what is already there."), true},
        {perm::CameraView, "camera", tr("View"),
         tr("Watch this device's camera."),
#ifdef UCD_HAS_SCREEN
         true,
#else
         false,
#endif
         true},
        {perm::CameraDirect, "camera", tr("Without asking"),
         tr("View the camera without a consent prompt."),
#ifdef UCD_HAS_SCREEN
         true,
#else
         false,
#endif
         true},
        {perm::ScreenView, "screen", tr("View"),
         tr("Watch this device's screen."),
#ifdef UCD_HAS_SCREEN
         true,
#else
         false,
#endif
         true},
        {perm::ScreenDirect, "screen", tr("Without asking"),
         tr("View the screen without a consent prompt."),
#ifdef UCD_HAS_SCREEN
         true,
#else
         false,
#endif
         true},
        {perm::ControlInput, "control", tr("Control"),
         tr("Use this device's mouse and keyboard."),
#ifdef UCD_HAS_SCREEN
         true,
#else
         false,
#endif
         true},
        {perm::ControlDirect, "control", tr("Without asking"),
         tr("Control mouse and keyboard without a consent prompt."),
#ifdef UCD_HAS_SCREEN
         true,
#else
         false,
#endif
         true},
        {perm::DisplayShow, "display", tr("Use as display"),
         tr("Ask to use this device's screen as an extra monitor."),
#ifdef UCD_HAS_SCREEN
         true,
#else
         false,
#endif
         true},
        {perm::DisplayDirect, "display", tr("Without asking"),
         tr("Use this device's screen as a monitor without a consent prompt."),
#ifdef UCD_HAS_SCREEN
         true,
#else
         false,
#endif
         true},
        {perm::SyncOffer, "sync", tr("Offer sync"),
         tr("Be offered this device's synced folders, and be sent the folders it is a member of."), true},
        {perm::SyncReceive, "sync", tr("Receive sync"),
         tr("Have its synced folders offered here, so this device can join them."), true},
    };
    return table;
}

bool isKnownPermission(const QString& id) {
    const auto& t = permissionTable();
    return std::any_of(t.begin(), t.end(), [&](const PermissionInfo& p) { return p.id == id; });
}

bool isSensitivePermission(const QString& id) {
    const auto& table = permissionTable();
    return std::any_of(table.begin(), table.end(), [&](const PermissionInfo& p) { return p.id == id && p.sensitive; });
}

PermissionSet knownPermissions(const PermissionSet& permissions) {
    PermissionSet out;
    for (const auto& permission : permissions)
        if (isKnownPermission(permission)) out.insert(permission);
    return out;
}

QVariantList permissionsForQml() {
    QVariantList out;
    for (const PermissionInfo& p : permissionTable()) {
        out.push_back(QVariantMap{
            {"id", p.id},
            {"service", p.service},
            {"label", p.label},
            {"description", p.description},
            {"available", p.available},
            {"sensitive", p.sensitive},
        });
    }
    return out;
}

}  // namespace ucd
