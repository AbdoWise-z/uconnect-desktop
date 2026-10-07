#include "TransferListModel.hpp"

#include <algorithm>

#include <QElapsedTimer>

namespace ucd {
namespace {

qint64 nowMs() {
    static QElapsedTimer clock = [] {
        QElapsedTimer t;
        t.start();
        return t;
    }();
    return clock.elapsed();
}

}  // namespace

double TransferListModel::etaOf(const Transfer& t) {
    if (t.state != QLatin1String("active") || t.speed <= 0 || t.total < 0) return -1;
    return double(std::max<qint64>(0, t.total - t.received)) / t.speed;
}

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
    case DirectionRole: return t.upload ? QStringLiteral("upload") : QStringLiteral("download");
    case CanRetryRole: return t.state == QLatin1String("failed") && !t.folder.isEmpty();
    case SpeedRole: return t.state == QLatin1String("active") ? t.speed : 0.0;
    case EtaRole: return etaOf(t);
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
        {DirectionRole, "direction"},
        {CanRetryRole, "canRetry"},
        {SpeedRole, "speed"},
        {EtaRole, "eta"},
    };
}

int TransferListModel::rowOf(quint32 id) const {
    const auto it = std::find_if(rows_.begin(), rows_.end(), [id](const Transfer& t) { return t.id == id; });
    return it == rows_.end() ? -1 : static_cast<int>(it - rows_.begin());
}

void TransferListModel::add(quint32 id, const QString& devId, const QString& remotePath,
                           const QString& localPath, bool upload, const QString& folder, bool replace) {
    beginInsertRows({}, static_cast<int>(rows_.size()), static_cast<int>(rows_.size()));
    rows_.push_back({id, devId, remotePath, localPath, 0, -1, QStringLiteral("active"), {}, upload, folder, replace});
    endInsertRows();
    emit activeCountChanged();
    bump();
}

void TransferListModel::progress(quint32 id, qint64 received, qint64 total) {
    const int row = rowOf(id);
    if (row < 0) return;
    Transfer& t = rows_[row];
    t.received = received;
    t.total    = total;
    // A smoothed rate from samples at least a quarter second apart. The first
    // sample only sets the start, which for a resumed download is not zero.
    const qint64 now = nowMs();
    if (t.sampledBytes < 0 || received < t.sampledBytes) {
        t.sampledBytes = received;
        t.sampledAt    = now;
    } else if (now - t.sampledAt >= 250) {
        const double rate = double(received - t.sampledBytes) * 1000.0 / double(now - t.sampledAt);
        t.speed        = t.speed <= 0 ? rate : 0.7 * t.speed + 0.3 * rate;
        t.sampledBytes = received;
        t.sampledAt    = now;
    }
    emit dataChanged(index(row), index(row), {ReceivedRole, TotalRole, SpeedRole, EtaRole});
    bump();
}

void TransferListModel::finish(quint32 id, const QString& localPath, const QString& error, bool canceled) {
    const int row = rowOf(id);
    if (row < 0) return;
    Transfer& t = rows_[row];
    if (!t.upload) t.localPath = localPath;
    else if (!localPath.isEmpty()) t.remotePath = localPath;
    t.error     = error;
    t.state     = canceled ? QStringLiteral("canceled")
                  : error.isEmpty() ? QStringLiteral("done")
                                    : QStringLiteral("failed");
    emit dataChanged(index(row), index(row));
    emit activeCountChanged();
    bump();
}

QString TransferListModel::fileName(quint32 id) const {
    const int row = rowOf(id);
    return row < 0 ? QString() : rows_.at(row).remotePath.section(QLatin1Char('/'), -1);
}

QString TransferListModel::devId(quint32 id) const {
    const int row = rowOf(id);
    return row < 0 ? QString() : rows_.at(row).devId;
}

void TransferListModel::remove(quint32 id) {
    const int row = rowOf(id);
    if (row < 0) return;
    beginRemoveRows({}, row, row);
    rows_.removeAt(row);
    endRemoveRows();
    emit activeCountChanged();
    bump();
}

std::optional<TransferListModel::Retry> TransferListModel::retryOf(quint32 id) const {
    const int row = rowOf(id);
    if (row < 0) return std::nullopt;
    const Transfer& t = rows_.at(row);
    if (t.state != QLatin1String("failed") || t.folder.isEmpty()) return std::nullopt;
    return Retry{t.devId, t.remotePath, t.localPath, t.folder, t.upload, t.replace};
}

int TransferListModel::activeCount() const {
    return static_cast<int>(std::count_if(rows_.begin(), rows_.end(), [](const Transfer& t) {
        return t.state == QLatin1String("active");
    }));
}

void TransferListModel::clearFinished(const QString& devId) {
    bool removed = false;
    for (int row = static_cast<int>(rows_.size()) - 1; row >= 0; --row) {
        const Transfer& t = rows_.at(row);
        if (t.state == QLatin1String("active") || (!devId.isEmpty() && t.devId != devId)) continue;
        beginRemoveRows({}, row, row);
        rows_.removeAt(row);
        endRemoveRows();
        removed = true;
    }
    if (removed) bump();
}

QVariantMap TransferListModel::summary(const QString& devId) const {
    int    count = 0, active = 0, failed = 0;
    qint64 done = 0, total = 0;
    double speed = 0;
    bool   totalKnown = true;
    for (const Transfer& t : rows_) {
        if (t.devId != devId) continue;
        ++count;
        if (t.state == QLatin1String("failed")) ++failed;
        if (t.state != QLatin1String("active")) continue;
        ++active;
        done += t.received;
        speed += t.speed;
        if (t.total < 0) totalKnown = false;
        else total += t.total;
    }
    return {
        {"count", count},
        {"active", active},
        {"failed", failed},
        {"done", double(done)},
        {"total", totalKnown ? double(total) : -1.0},
        {"speed", speed},
        {"eta", totalKnown && speed > 0 ? double(std::max<qint64>(0, total - done)) / speed : -1.0},
    };
}

void TransferListModel::bump() {
    ++revision_;
    emit revisionChanged();
}

}  // namespace ucd
