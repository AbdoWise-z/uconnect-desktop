#pragma once
// Uploads and downloads, newest last.

#include <optional>

#include <QAbstractListModel>
#include <QString>
#include <QVariantMap>
#include <QVector>

namespace ucd {

class TransferListModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int activeCount READ activeCount NOTIFY activeCountChanged)
    // Bumped by every change, progress included; bindings that call summary()
    // read it so they re-evaluate.
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)
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
        DirectionRole,  // "upload" | "download"
        CanRetryRole,
    };
    Q_ENUM(Role)

    using QAbstractListModel::QAbstractListModel;

    int                    rowCount(const QModelIndex& parent = {}) const override;
    QVariant               data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // `folder`: where the file goes (a download's local folder, an upload's
    // remote one), so a failed transfer can be retried.
    void add(quint32 id, const QString& devId, const QString& remotePath,
             const QString& localPath = {}, bool upload = false, const QString& folder = {}, bool replace = false);
    void progress(quint32 id, qint64 received, qint64 total);
    void finish(quint32 id, const QString& localPath, const QString& error, bool canceled);
    QString fileName(quint32 id) const;
    QString devId(quint32 id) const;
    void    remove(quint32 id);

    // What it takes to try a failed transfer again.
    struct Retry {
        QString devId;
        QString remotePath;  // download: the file; upload: where it was going
        QString localPath;   // upload: the file
        QString folder;
        bool    upload  = false;
        bool    replace = false;
    };
    std::optional<Retry> retryOf(quint32 id) const;

    int activeCount() const;
    int revision() const { return revision_; }
    // Totals for one device's transfers: count, active, failed, and the bytes
    // done and expected across the active ones (total -1 while any is unknown).
    Q_INVOKABLE QVariantMap summary(const QString& devId) const;
    // Removes finished, failed and canceled rows; only one device's if given.
    Q_INVOKABLE void clearFinished(const QString& devId = {});

signals:
    void activeCountChanged();
    void revisionChanged();

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
        bool upload = false;
        QString folder;
        bool replace = false;
    };
    int rowOf(quint32 id) const;

    void bump();

    QVector<Transfer> rows_;
    int               revision_ = 0;
};

}  // namespace ucd
