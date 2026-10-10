#pragma once
// A monitor that exists only for uConnect: the host of an extended display
// session creates one, captures it like any other screen and streams it to
// the display device. Windows uses the uConnect virtual display driver
// (github.com/AbdoWise-z/uconnect-vdd), macOS the system's CGVirtualDisplay.
//
// The monitor must never outlive its session: destroying the object removes
// it, and the platforms remove it on their own when the process dies (the
// driver when its handle closes, macOS with the process). On Windows a
// process that hangs loses it too, once keepAlive() stops arriving.

#include <QByteArray>
#include <QRect>
#include <QString>
#include <QVariantMap>
#include <memory>

namespace ucd::display {

struct Mode {
    int width = 1920, height = 1080;
    int refreshMilliHz = 60000;
    bool hiDpi = false;  // macOS: a 2x mode, `width` x `height` in pixels
};

enum class Availability { Ready, NotInstalled, Outdated, NotSupported };

struct Status {
    Availability availability = Availability::NotSupported;
    QString detail;  // shown to the user when not Ready
};

class VirtualDisplay {
public:
    virtual ~VirtualDisplay() = default;  // implementations remove the monitor
    // Plugs in the monitor and waits until the OS shows it (a few seconds at
    // most), so call it off the GUI and session threads. `identity` is
    // stable per display device (monitorIdentity), so the OS remembers where
    // the user put that device's screen.
    virtual bool create(const QByteArray& identity, const QString& name, const Mode& mode, QString* error) = 0;
    virtual bool setMode(const Mode& mode, QString* error) = 0;
    virtual void destroy() = 0;
    // About once a second while the monitor exists (Windows' driver watchdog).
    virtual void keepAlive() = 0;
    // What DesktopEncoder::encode takes as `monitor`: the GDI device name on
    // Windows, the CGDirectDisplayID on macOS.
    virtual QString captureId() const = 0;
    // Where it sits, in the coordinates the cursor and input use (physical
    // pixels on Windows, points on macOS), and its size in pixels.
    virtual QRect geometry() const = 0;
    virtual QSize pixels() const = 0;
};

Status status();
// A backend for this platform, or null where there is none.
std::unique_ptr<VirtualDisplay> create();

// The stable 16-byte id of the monitor shown on the device with this public
// key (hex), so the same device gets back the same screen.
QByteArray monitorIdentity(const QString& publicKeyHex);

// The screen service's description of a monitor (see ScreenSettings::monitors).
QVariantMap monitorInfo(const VirtualDisplay& display, const QString& name);

}  // namespace ucd::display
