#include "SingleInstance.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QLocalServer>
#include <QLocalSocket>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace ucd {
namespace {

// Per user and profile. Windows pipe names are machine-wide, hence the home
// folder; Unix socket paths have a length limit, hence the hash.
QString serverName() {
    const QByteArray id = (QCoreApplication::organizationName() + QLatin1Char('/')
                           + QCoreApplication::applicationName() + QLatin1Char('/') + QDir::homePath())
                              .toUtf8();
    return QStringLiteral("uconnect-desktop-")
         + QString::fromLatin1(QCryptographicHash::hash(id, QCryptographicHash::Sha256).toHex().left(24));
}

}  // namespace

SingleInstance::SingleInstance(QObject* parent) : QObject(parent) {
    const QString name = serverName();
    QLocalSocket running;
    running.connectToServer(name);
    if (running.waitForConnected(1000)) {
#ifdef Q_OS_WIN
        // Windows only lets the foreground process raise a window; this
        // launch is it, so hand that on to the running instance.
        AllowSetForegroundWindow(ASFW_ANY);
#endif
        running.write("show\n");
        running.waitForBytesWritten(1000);
        running.disconnectFromServer();
        forwarded_ = true;
        return;
    }
    server_ = new QLocalServer(this);
    server_->setSocketOptions(QLocalServer::UserAccessOption);
    // Nothing answered, so a socket left behind is from an instance that crashed.
    if (!server_->listen(name)) {
        QLocalServer::removeServer(name);
        server_->listen(name);
    }
    connect(server_, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket* socket = server_->nextPendingConnection()) {
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            emit activated();
        }
    });
}

}  // namespace ucd
