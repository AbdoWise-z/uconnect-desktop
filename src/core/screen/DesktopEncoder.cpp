#include "DesktopEncoder.hpp"
#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
namespace ucd::screen {
std::unique_ptr<DesktopEncoder> desktopEncoder() { return {}; }
}
#endif
