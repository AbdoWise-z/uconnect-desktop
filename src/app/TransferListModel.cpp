#include "TransferListModel.hpp"

#include <algorithm>

namespace ucd {

int TransferListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

QVariant TransferListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= rows_.size()) return {};
    const Transfer& t = rows_.at(index.row());
    switch (role) {
    case IdRole: return t.id;
    case DevIdRole: return t.devId;
    case FileNameRole: return t.remotePath.section(QLatin1Char('/'), -1);
    case RemotePathRole: return t.remotePath;
    case LocalPathRole: return t.localPath;
    case ReceivedRole: return QVariant::fromValue(t.received);
    case TotalRole: return QVariant::fromValue(t.total);
    case StateRole: return t.state;
    case ErrorRole: return t.error;
    default: return {};
    }
}

QHash<int, QByteArray> TransferListModel::roleNames() const {
    return {
        {IdRole, "transferId"},
        {DevIdRole, "devId"},
        {FileNameRole, "fileName"},
        {RemotePathRole, "remotePath"},
        {LocalPathRole, "localPath"},
        {ReceivedRole, "received"},
        {TotalRole, "total"},
        {StateRole, "transferState"},
        {ErrorRole, "error"},
    };
}

int TransferListModel::rowOf(quint32 id) const {
    const auto it = std::find_if(rows_.begin(), rows_.end(), [id](const Transfer& t) { return t.id == id; });
    return it == rows_.end() ? -1 : static_cast<int>(it - rows_.begin());
}

void TransferListModel::add(quint32 id, const QString& devId, const QString& remotePath) {
    beginInsertRows({}, static_cast<int>(rows_.size()), static_cast<int>(rows_.size()));
    rows_.push_back({id, devId, remotePath, {}, 0, -1, QStringLiteral("active"), {}});
    endInsertRows();
    emit activeCountChanged();
}

void TransferListModel::progress(quint32 id, qint64 received, qint64 total) {
    const int row = rowOf(id);
    if (row < 0) return;
    rows_[row].received = received;
    rows_[row].total    = total;
    emit dataChanged(index(row), index(row), {ReceivedRole, TotalRole});
}

void TransferListModel::finish(quint32 id, const QString& localPath, const QString& error, bool canceled) {
    const int row = rowOf(id);
    if (row < 0) return;
    Transfer& t = rows_[row];
    t.localPath = localPath;
    t.error     = error;
    t.state     = canceled ? QStringLiteral("canceled")
                  : error.isEmpty() ? QStringLiteral("done")
                                    : QStringLiteral("failed");
    emit dataChanged(index(row), index(row));
    emit activeCountChanged();
}

QString TransferListModel::fileName(quint32 id) const {
    const int row = rowOf(id);
    return row < 0 ? QString() : rows_.at(row).remotePath.section(QLatin1Char('/'), -1);
}

int TransferListModel::activeCount() const {
    return static_cast<int>(std::count_if(rows_.begin(), rows_.end(), [](const Transfer& t) {
        return t.state == QLatin1String("active");
    }));
}

void TransferListModel::clearFinished() {
    for (int row = static_cast<int>(rows_.size()) - 1; row >= 0; --row) {
        if (rows_.at(row).state == QLatin1String("active")) continue;
        beginRemoveRows({}, row, row);
        rows_.removeAt(row);
        endRemoveRows();
    }
}

}  // namespace ucd
