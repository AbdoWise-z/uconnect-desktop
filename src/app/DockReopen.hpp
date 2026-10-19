#pragma once
// macOS: a click on the app's Dock icon while it runs. Qt reports it only as
// the app becoming active, the same as starting it or switching to it, so
// this hooks the reopen message itself.

#include <functional>

namespace ucd {

// Calls `callback` on each Dock click from now on. Needs the QApplication;
// macOS only (DockReopen_mac.mm).
void onDockReopen(std::function<void()> callback);

}  // namespace ucd
