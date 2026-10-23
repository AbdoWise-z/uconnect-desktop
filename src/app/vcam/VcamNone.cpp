// Platforms without a virtual camera yet (macOS needs a signed camera extension).
#include "app/vcam/VcamOutput.hpp"

#include <QObject>

namespace ucd::vcam {

std::unique_ptr<Output> makeOutput() { return nullptr; }

Support support() {
    return {false, false, QObject::tr("The virtual camera is not available on this system yet.")};
}

}  // namespace ucd::vcam
