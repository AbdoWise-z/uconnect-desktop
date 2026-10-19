#pragma once
// Starting the app when the user signs in, the way each system expects:
// the Run key on Windows, a LaunchAgent on macOS, an autostart entry on
// Linux (XDG). The registration itself is the setting: it is read back
// from the system, so turning it off there (Task Manager's Startup apps,
// say) shows here too.

#include <QString>
#include <QStringList>

namespace ucd::login {

// Whether `name` (the application name, one per settings profile) is set
// to start at sign-in, and enabled.
bool isEnabled(const QString& name);

// Registers this executable to start at sign-in with `arguments`, replacing
// any earlier registration of `name`, or removes it. False if that failed.
bool setEnabled(const QString& name, bool enabled, const QStringList& arguments = {});

// The command registered for `name`, as the system will run it; empty if none.
QString command(const QString& name);

// Where the registration of `name` lives: a file (macOS, Linux) or a
// registry value (Windows, as "key\value").
QString location(const QString& name);

}  // namespace ucd::login
