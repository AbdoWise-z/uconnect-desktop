#pragma once
// Permissions this device can grant to others, each with a unique, stable
// name ("<service>.<action>"). The UI is built from this table, so adding a
// permission is one entry here plus the check where the action is served.

#include <vector>

#include <QSet>
#include <QString>
#include <QVariantList>

namespace ucd {

namespace perm {
inline const QString StorageObserve  = QStringLiteral("storage.observe");
inline const QString StorageRead     = QStringLiteral("storage.read");
inline const QString StorageWrite    = QStringLiteral("storage.write");
inline const QString StorageUpload   = QStringLiteral("storage.upload");
inline const QString CameraView      = QStringLiteral("camera.view");
inline const QString ScreenView      = QStringLiteral("screen.view");
inline const QString ScreenDirect    = QStringLiteral("screen.direct");
inline const QString ControlInput    = QStringLiteral("control.input");
inline const QString ControlDirect   = QStringLiteral("control.direct");
inline const QString DisplayShow     = QStringLiteral("display.show");
inline const QString DisplayDirect   = QStringLiteral("display.direct");
}  // namespace perm

struct PermissionInfo {
    QString id;
    QString service;      // as in Hello's service names
    QString label;        // short, shown next to a checkbox
    QString description;  // one sentence: what the other device can do
    bool    available;    // false while the action itself does not exist yet
    bool    trustedOnly = false;
};

const std::vector<PermissionInfo>& permissionTable();
bool                               isKnownPermission(const QString& id);
bool                               isTrustedOnlyPermission(const QString& id);

// The table for QML: a list of maps with the fields above.
QVariantList permissionsForQml();

using PermissionSet = QSet<QString>;
PermissionSet permissionsForUntrusted(const PermissionSet& permissions);

}  // namespace ucd
