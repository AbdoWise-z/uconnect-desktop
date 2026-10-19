#pragma once
// Installing the uConnect virtual display driver (Windows only). The app runs
// itself elevated with --display-driver install|remove for the parts that
// need an administrator.

#include <QString>
#include <QStringList>

namespace ucd::display {

// The driver package shipped next to the executable, or empty.
QString bundledDriverInf();

// Elevated: creates the driver's root device if missing and installs the
// package's driver on it.
bool installDriver(const QString& infPath, QString* error);
// Elevated: removes the device and the driver package.
bool uninstallDriver(QString* error);

// Runs this executable elevated with `arguments` (the UAC prompt) and waits
// for it. False if the user declined or it failed; `error` says which.
bool runElevated(const QStringList& arguments, QString* error);

// main() calls this first: handles --display-driver and returns its exit
// code, or -1 when the arguments are not for it.
int handleDriverCommand(int argc, char* argv[]);

}  // namespace ucd::display
