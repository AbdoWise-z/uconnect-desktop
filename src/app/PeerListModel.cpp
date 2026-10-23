#include "PeerListModel.hpp"

#include <algorithm>

#include <QSet>

namespace ucd {

int PeerListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : count();
}

QVariant PeerListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= rows_.size()) return {};
    return roleValue(rows_.at(index.row()), role == Qt::DisplayRole ? DisplayNameRole : role);
}

QVariant PeerListModel::roleValue(const PeerRecord& p, int role) const {
    switch (role) {
    case DisplayNameRole: return p.hasHello && !p.name.isEmpty() ? p.name : tr("Unknown device");
    case DevIdRole: return p.devId;
    case ShortIdRole: return shortId(p.devId);
    case OsRole: return p.os;
    case ServicesRole: return serviceNames(p.services);
    case StateRole: return p.state;
    case StateCodeRole: return p.stateCode;
    case CloseReasonRole: return p.closeReason;
    case StaleRole: return p.stale;
    case RelayedRole: return p.relayed;
    case AddressRole: return p.address;
    case NetworkRuleRole: return p.networkRule;
    case PermissionSourceRole: return p.permissionSource;
    case HasHelloRole: return p.hasHello;
    case BytesSentRole: return QVariant::fromValue(p.bytesSent);
    case BytesReceivedRole: return QVariant::fromValue(p.bytesReceived);
    case IdentityStateRole: return p.identityState;
    case IdentityKeyRole: return p.identityKey;
    case FingerprintRole: return p.fingerprint;
    case SpecialRole: return p.special;
    case ExceptionRole: return p.exception;
    case PermissionFromRole: return p.permissionFrom;
    case PermissionsRole: return p.permissions;
    case GrantedToMeRole: return p.grantedToMe;
    default: return {};
    }
}

QHash<int, QByteArray> PeerListModel::roleNames() const {
    return {
        {DevIdRole, "devId"},
        {ShortIdRole, "shortId"},
        {DisplayNameRole, "displayName"},
        {OsRole, "os"},
        {ServicesRole, "services"},
        {StateRole, "peerState"},
        {StateCodeRole, "stateCode"},
        {CloseReasonRole, "closeReason"},
        {StaleRole, "stale"},
        {RelayedRole, "relayed"},
        {AddressRole, "address"},
        {NetworkRuleRole, "networkRule"},
        {PermissionSourceRole, "permissionSource"},
        {HasHelloRole, "hasHello"},
        {BytesSentRole, "bytesSent"},
        {BytesReceivedRole, "bytesReceived"},
        {IdentityStateRole, "identityState"},
        {IdentityKeyRole, "identityKey"},
        {FingerprintRole, "fingerprint"},
        {SpecialRole, "special"},
        {ExceptionRole, "exception"},
        {PermissionFromRole, "permissionFrom"},
        {PermissionsRole, "permissions"},
        {GrantedToMeRole, "grantedToMe"},
    };
}

void PeerListModel::setPeers(const QVector<PeerRecord>& peers) {
    QHash<QString, const PeerRecord*> incoming;
    for (const auto& p : peers) incoming.insert(p.devId, &p);

    bool anyChange = false;
    for (int row = count() - 1; row >= 0; --row) {
        if (incoming.contains(rows_.at(row).devId)) continue;
        beginRemoveRows({}, row, row);
        rows_.removeAt(row);
        endRemoveRows();
        anyChange = true;
    }

    QSet<QString> present;
    for (int row = 0; row < count(); ++row) {
        PeerRecord&       existing = rows_[row];
        const PeerRecord& next     = *incoming.value(existing.devId);
        present.insert(existing.devId);
        if (existing == next) continue;
        existing = next;
        emit dataChanged(index(row), index(row));
        anyChange = true;
    }

    for (const auto& p : peers) {
        if (present.contains(p.devId)) continue;
        beginInsertRows({}, count(), count());
        rows_.push_back(p);
        endInsertRows();
        anyChange = true;
    }

    if (anyChange) {
        ++revision_;
        emit changed();
    }
}

int PeerListModel::connectedCount() const {
    return static_cast<int>(std::count_if(rows_.begin(), rows_.end(), [](const PeerRecord& p) {
        return p.stateCode == static_cast<int>(uconnect::PeerState::Connected);
    }));
}

QVariantMap PeerListModel::get(const QString& devId) const {
    QVariantMap out;
    const auto  it = std::find_if(rows_.begin(), rows_.end(),
                                  [&](const PeerRecord& p) { return p.devId == devId; });
    if (it == rows_.end()) return out;
    const auto roles = roleNames();
    for (auto r = roles.begin(); r != roles.end(); ++r)
        out.insert(QString::fromLatin1(r.value()), roleValue(*it, r.key()));
    return out;
}

QVariantList PeerListModel::devicesOffering(const QString& service) const {
    const uint32_t bit = servicesFromNames({service});
    QVariantList   out;
    for (const PeerRecord& p : rows_) {
        if (p.stateCode != static_cast<int>(uconnect::PeerState::Connected) || !(p.services & bit))
            continue;
        out.push_back(get(p.devId));
    }
    return out;
}

QVariantList PeerListModel::connectedDevices() const {
    QVariantList out;
    for (const PeerRecord& p : rows_)
        if (p.stateCode == static_cast<int>(uconnect::PeerState::Connected)) out.push_back(get(p.devId));
    return out;
}

}  // namespace ucd
