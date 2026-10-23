#include "core/OpenH264Installer.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSysInfo>

#include "core/Bunzip2.hpp"
#include "openh264_license.h"  // generated from docs/OpenH264-BINARY-LICENSE.txt

namespace ucd {

namespace {

constexpr qint64 kMaxArchive = 4 * 1024 * 1024;   // Cisco's archives are ~1 MB
constexpr qint64 kMaxLibrary = 16 * 1024 * 1024;  // and the libraries ~2 MB

QString currentOs() {
#if defined(Q_OS_WIN)
    return QStringLiteral("windows");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("macos");
#elif defined(Q_OS_LINUX)
    return QStringLiteral("linux");
#else
    return {};
#endif
}

QString currentCpu() {
    const QString cpu = QSysInfo::currentCpuArchitecture();
    return cpu == "x86_64" ? QStringLiteral("x64") : cpu == "arm64" ? QStringLiteral("arm64") : cpu;
}

}  // namespace

std::pair<QString, QByteArray> OpenH264Installer::release(const QString& os, const QString& cpu) {
    // Cisco's 2.6.0 binaries and the SHA-256 of their .bz2 archives.
    struct Release { const char* os; const char* cpu; const char* name; const char* sha256; };
    static constexpr Release releases[] = {
        {"windows", "x64", "openh264-2.6.0-win64.dll", "dab5f2a872777f9a58b69bfa9fbcf20d9f82f2d6ec91383fd70bff49bd34ac9f"},
        {"windows", "arm64", "openh264-2.6.0-win-arm64.dll", "6b57d3ecd06bd80b5f3ee1abbb31d48798ca6d89ef1bc98cf2e6d1ebddf5cf45"},
        {"macos", "arm64", "libopenh264-2.6.0-mac-arm64.dylib", "6db362ee5abdab572311aeadb96d3f44b0617d9a4a4b9f4db4cb5ac4d968da71"},
        {"macos", "x64", "libopenh264-2.6.0-mac-x64.dylib", "38b2ed6d1d45b6a3e408c734173f2d67ab44a10d0e154ff3489b89877cd60e7e"},
        {"linux", "x64", "libopenh264-2.6.0-linux64.8.so", "27ab53323c110b76214c1c72222f459d17febbcd1e252136cadc292b0308d75b"},
        {"linux", "arm64", "libopenh264-2.6.0-linux-arm64.8.so", "a78aea7970150f46bcd3bb7994c9e6dd90bd7a9ea785920f5a73f6964e3fcda7"},
    };
    for (const Release& r : releases)
        if (os == QLatin1String(r.os) && cpu == QLatin1String(r.cpu))
            return {QString::fromLatin1(r.name), QByteArray::fromHex(r.sha256)};
    return {};
}

OpenH264Installer::OpenH264Installer(QObject* parent) : QObject(parent) {
    std::tie(fileName_, digest_) = release(currentOs(), currentCpu());
}

QString OpenH264Installer::license() const { return QString::fromUtf8(kOpenH264License); }

QString OpenH264Installer::installPath() const {
    if (fileName_.isEmpty()) return {};
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath("codecs/" + fileName_);
}

bool OpenH264Installer::exists(const QString& path) const { return !path.isEmpty() && QFileInfo(path).isFile(); }

void OpenH264Installer::install() {
    if (!supported() || state_ == "downloading") return;
    if (!network_) network_ = new QNetworkAccessManager(this);
    QNetworkRequest request(QUrl("https://ciscobinary.openh264.org/" + fileName_ + ".bz2"));
    request.setTransferTimeout(30000);
    reply_    = network_->get(request);
    state_    = QStringLiteral("downloading");
    progress_ = 0;
    error_.clear();
    emit changed();
    connect(reply_, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64 total) {
        if (received > kMaxArchive) {
            fail(tr("The download is larger than Cisco's archive should be."));
            return;
        }
        progress_ = total > 0 ? double(received) / double(total) : 0;
        emit changed();
    });
    connect(reply_, &QNetworkReply::finished, this, [this, reply = reply_.data()] { finish(reply); });
}

void OpenH264Installer::cancel() {
    if (state_ != "downloading") return;
    if (reply_) {
        reply_->disconnect(this);
        reply_->abort();
        reply_->deleteLater();
    }
    state_ = QString();
    emit changed();
}

void OpenH264Installer::fail(const QString& message) {
    if (reply_) {
        reply_->disconnect(this);
        reply_->abort();
        reply_->deleteLater();
    }
    state_ = QStringLiteral("failed");
    error_ = message;
    emit changed();
}

void OpenH264Installer::finish(QNetworkReply* reply) {
    reply->deleteLater();
    if (reply != reply_) return;
    if (reply->error() != QNetworkReply::NoError) {
        fail(tr("Could not download it from Cisco: %1").arg(reply->errorString()));
        return;
    }
    const QByteArray archive = reply->read(kMaxArchive + 1);
    if (archive.size() > kMaxArchive) {
        fail(tr("The download is larger than Cisco's archive should be."));
        return;
    }
    if (QCryptographicHash::hash(archive, QCryptographicHash::Sha256) != digest_) {
        fail(tr("The download does not match Cisco's published archive, so it was not installed."));
        return;
    }
    QString    problem;
    const auto library = bunzip2(archive, kMaxLibrary, &problem);
    if (!library) {
        fail(tr("Could not unpack Cisco's archive (%1).").arg(problem));
        return;
    }

    const QString   path = installPath();
    const QFileInfo target(path);
    QDir().mkpath(target.absolutePath());
    QSaveFile file(path);  // all or nothing: the codec may be in use
    if (!file.open(QIODevice::WriteOnly) || file.write(*library) != library->size() || !file.commit()) {
        fail(tr("Could not save it to %1: %2").arg(QDir::toNativeSeparators(path), file.errorString()));
        return;
    }
    QFile notice(target.absoluteDir().filePath("OpenH264-BINARY-LICENSE.txt"));
    if (notice.open(QIODevice::WriteOnly)) notice.write(kOpenH264License);

    state_    = QStringLiteral("done");
    progress_ = 1;
    emit changed();
    emit installed(path);
}

}  // namespace ucd
