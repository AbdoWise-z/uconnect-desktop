#pragma once
// Downloads Cisco's OpenH264 binary, the software H.264 fallback, from Cisco
// (never bundled: Cisco's patent license covers its own downloads only),
// checks it against the pinned SHA-256 of Cisco's archive for this system,
// unpacks it (bzip2) and puts it with Cisco's license in uConnect's data
// folder. The same releases as scripts/install-openh264.py.
#include <QObject>
#include <QPointer>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

namespace ucd {

class OpenH264Installer : public QObject {
    Q_OBJECT
    // Whether Cisco publishes a build for this system, and its file name.
    Q_PROPERTY(bool supported READ supported CONSTANT)
    Q_PROPERTY(QString fileName READ fileName CONSTANT)
    // Cisco's license for the binary, to show before downloading.
    Q_PROPERTY(QString license READ license CONSTANT)
    // "", "downloading", "failed" or "done"; progress 0..1 while downloading.
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(double progress READ progress NOTIFY changed)
    Q_PROPERTY(QString error READ error NOTIFY changed)
public:
    explicit OpenH264Installer(QObject* parent = nullptr);

    bool    supported() const { return !fileName_.isEmpty(); }
    QString fileName() const { return fileName_; }
    QString license() const;
    QString state() const { return state_; }
    double  progress() const { return progress_; }
    QString error() const { return error_; }

    // Where it is installed: uConnect's data folder, codecs/.
    Q_INVOKABLE QString installPath() const;
    Q_INVOKABLE bool    exists(const QString& path) const;
    Q_INVOKABLE void    install();
    Q_INVOKABLE void    cancel();

    // The archive's file name and pinned SHA-256 for an OS ("windows",
    // "macos", "linux") and CPU ("x64", "arm64"); empty when Cisco has none.
    static std::pair<QString, QByteArray> release(const QString& os, const QString& cpu);

signals:
    void changed();
    void installed(const QString& path);

private:
    void fail(const QString& message);
    void finish(QNetworkReply* reply);

    QString                 fileName_;
    QByteArray              digest_;
    QString                 state_;
    double                  progress_ = 0;
    QString                 error_;
    QNetworkAccessManager*  network_ = nullptr;
    QPointer<QNetworkReply> reply_;
};

}  // namespace ucd
