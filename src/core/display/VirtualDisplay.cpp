#include "VirtualDisplay.hpp"
#include "DriverSetup.hpp"

#include <QCryptographicHash>

namespace ucd::display {

QByteArray monitorIdentity(const QString& publicKeyHex) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArrayView("uconnect-display v1"));
    hash.addData(QByteArray::fromHex(publicKeyHex.toLatin1()));
    return hash.result().left(16);
}

QVariantMap monitorInfo(const VirtualDisplay& display, const QString& name) {
    const QRect input = display.geometry();
    const QSize pixels = display.pixels();
    return {{"nativeId", display.captureId()}, {"name", name}, {"width", pixels.width()}, {"height", pixels.height()},
        {"inputX", input.x()}, {"inputY", input.y()}, {"inputWidth", input.width()}, {"inputHeight", input.height()}};
}

#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
Status status() {
    return {Availability::NotSupported, QStringLiteral("Extended displays need Windows or macOS on this side.")};
}
std::unique_ptr<VirtualDisplay> create() { return {}; }
#endif

#ifndef Q_OS_WIN
// Only Windows needs a driver.
QString bundledDriverInf() { return {}; }
bool installDriver(const QString&, QString* error) { if (error) *error = QStringLiteral("No driver is needed here."); return false; }
bool uninstallDriver(QString* error) { if (error) *error = QStringLiteral("No driver is needed here."); return false; }
bool runElevated(const QStringList&, QString* error) { if (error) *error = QStringLiteral("Not supported here."); return false; }
int handleDriverCommand(int, char*[]) { return -1; }
#endif

}  // namespace ucd::display
