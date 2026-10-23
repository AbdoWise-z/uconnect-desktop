#pragma once

#include <QAbstractListModel>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

#include "core/Session.hpp"

namespace ucd {

class PeerListModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY changed)
    Q_PROPERTY(int connectedCount READ connectedCount NOTIFY changed)
    // Bumped whenever any row changes. QML bindings that call get() read it
    // first, so they re-evaluate on every change.
    Q_PROPERTY(int revision READ revision NOTIFY changed)
public:
    enum Role {
        DevIdRole = Qt::UserRole + 1,
        ShortIdRole,
        DisplayNameRole,
        OsRole,
        ServicesRole,
        StateRole,
        StateCodeRole,
        CloseReasonRole,
        StaleRole,
        RelayedRole,
        AddressRole,
        NetworkRuleRole,
        PermissionSourceRole,
        HasHelloRole,
        BytesSentRole,
        BytesReceivedRole,
        IdentityStateRole,
        IdentityKeyRole,
        FingerprintRole,
        SpecialRole,
        PermissionsRole,
        GrantedToMeRole,
    };
    Q_ENUM(Role)

    using QAbstractListModel::QAbstractListModel;

    int                    rowCount(const QModelIndex& parent = {}) const override;
    QVariant               data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Rows keep their position across updates so the selection does not jump.
    void setPeers(const QVector<PeerRecord>& peers);
    const QVector<PeerRecord>& peers() const { return rows_; }

    int count() const { return static_cast<int>(rows_.size()); }
    int connectedCount() const;
    int revision() const { return revision_; }

    // All roles of one device, keyed by role name; empty if it is gone.
    Q_INVOKABLE QVariantMap get(const QString& devId) const;
    // Connected devices that advertise `service`, as role maps.
    Q_INVOKABLE QVariantList devicesOffering(const QString& service) const;
    // Every connected device, as role maps.
    Q_INVOKABLE QVariantList connectedDevices() const;

signals:
    void changed();

private:
    QVariant roleValue(const PeerRecord& p, int role) const;

    QVector<PeerRecord> rows_;
    int                 revision_ = 0;
};

}  // namespace ucd
