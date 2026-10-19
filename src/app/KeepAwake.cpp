#include "KeepAwake.hpp"

#include <QtGlobal>  // first: it defines the Q_OS_ macros below

#ifdef Q_OS_WIN
#include <windows.h>
#elif defined(Q_OS_MACOS)
#include <IOKit/pwr_mgt/IOPMLib.h>
#elif defined(UCD_HAS_DBUS)
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#endif

namespace ucd {

void KeepAwake::set(bool on) {
    if (on == on_) return;
    on_ = on;
#ifdef Q_OS_WIN
    held_ = SetThreadExecutionState(on ? ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED : ES_CONTINUOUS) != 0 && on;
#elif defined(Q_OS_MACOS)
    if (on) {
        IOPMAssertionID id = 0;
        held_ = IOPMAssertionCreateWithName(kIOPMAssertPreventUserIdleDisplaySleep, kIOPMAssertionLevelOn,
                                            CFSTR("uConnect is showing an extended display"), &id) == kIOReturnSuccess;
        handle_ = held_ ? id : 0;
    } else if (held_) {
        IOPMAssertionRelease(handle_);
        handle_ = 0; held_ = false;
    }
#elif defined(UCD_HAS_DBUS)
    const auto bus = QDBusConnection::sessionBus();
    QDBusInterface saver(QStringLiteral("org.freedesktop.ScreenSaver"), QStringLiteral("/org/freedesktop/ScreenSaver"),
                         QStringLiteral("org.freedesktop.ScreenSaver"), bus);
    QDBusInterface gnome(QStringLiteral("org.gnome.SessionManager"), QStringLiteral("/org/gnome/SessionManager"),
                         QStringLiteral("org.gnome.SessionManager"), bus);
    const QString app = QStringLiteral("uConnect"), why = QStringLiteral("Showing an extended display");
    if (on) {
        if (const QDBusReply<uint> cookie = saver.call(QStringLiteral("Inhibit"), app, why); cookie.isValid()) {
            handle_ = cookie.value(); gnome_ = false; held_ = true;
        } else if (const QDBusReply<uint> gnomeCookie = gnome.call(QStringLiteral("Inhibit"), app, uint(0), why, uint(8));  // 8: idle
                   gnomeCookie.isValid()) {
            handle_ = gnomeCookie.value(); gnome_ = true; held_ = true;
        }
    } else if (held_) {
        if (gnome_) gnome.call(QStringLiteral("Uninhibit"), handle_);
        else saver.call(QStringLiteral("UnInhibit"), handle_);
        handle_ = 0; held_ = false;
    }
#else
    Q_UNUSED(handle_); Q_UNUSED(gnome_);
#endif
}

}  // namespace ucd
