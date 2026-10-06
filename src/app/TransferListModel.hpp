#pragma once
// Downloads from other devices' storage, newest last.

#include <QAbstractListModel>
#include <QString>
#include <QVector>

namespace ucd {

class TransferListModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int activeCount READ activeCount NOTIFY activeCountChanged)
public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        DevIdRole,
        FileNameRole,
        RemotePathRole,
        LocalPathRole,
        ReceivedRole,
        TotalRole,
        StateRole,  // "active" | "done" | "failed" | "canceled"
        ErrorRole,
    };
    Q_ENUM(Role)

    using QAbstractListModel::QAbstractListModel;

    int                    rowCount(const QModelIndex& parent = {}) const override;
    QVariant               data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void add(quint32 id, const QString& devId, const QString& remotePath);
    void progress(quint32 id, qint64 received, qint64 total);
    void finish(quint32 id, const QString& localPath, const QString& error, bool canceled);
    QString fileName(quint32 id) const;

    int activeCount() const;
    // Removes finished, failed and canceled rows.
    Q_INVOKABLE void clearFinished();

signals:
    void activeCountChanged();

private:
    struct Transfer {
        quint32 id = 0;
        QString devId;
        QString remotePath;
        QString localPath;
        qint64  received = 0;
        qint64  total    = -1;
        QString state;
        QString error;
    };
    int rowOf(quint32 id) const;

    QVector<Transfer> rows_;
};

}  // namespace ucd
