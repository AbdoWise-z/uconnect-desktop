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
         tr("Open files inside the app without saving a copy."), false},
        {perm::StorageDownload, "storage", tr("Download"),
         tr("Save copies of files to their device."), true},
        {perm::StorageWrite, "storage", tr("Write"),
         tr("Change, rename or delete existing files."), false},
        {perm::StorageUpload, "storage", tr("Upload"),
         tr("Add new files to the shared folders."), true},
        {perm::CameraView, "camera", tr("View"),
         tr("Watch this device's camera."), false},
        {perm::ScreenView, "screen", tr("View"),
         tr("Watch this device's screen."), false},
        {perm::ControlInput, "control", tr("Control"),
         tr("Use this device's mouse and keyboard."), false},
    };
    return table;
}

bool isKnownPermission(const QString& id) {
    const auto& t = permissionTable();
    return std::any_of(t.begin(), t.end(), [&](const PermissionInfo& p) { return p.id == id; });
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
        });
    }
    return out;
}

}  // namespace ucd
