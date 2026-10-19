#pragma once
// Keeps the screen on and the computer awake while held: a screen showing an
// extended display is someone's monitor, so it must not dim or sleep.
// Windows: SetThreadExecutionState. macOS: an IOKit power assertion. Linux:
// an inhibit over D-Bus, from org.freedesktop.ScreenSaver (KDE and most
// desktops) or else GNOME's session manager. Each ends with the process too.

namespace ucd {

class KeepAwake {
public:
    KeepAwake() = default;
    KeepAwake(const KeepAwake&) = delete;
    KeepAwake& operator=(const KeepAwake&) = delete;
    ~KeepAwake() { set(false); }

    void set(bool on);
    // The system took the request (false where nothing could be asked).
    bool held() const { return held_; }

private:
    bool         on_ = false;
    bool         held_ = false;
    unsigned int handle_ = 0;  // macOS: the assertion; Linux: the inhibit cookie
    bool         gnome_ = false;  // Linux: the cookie is GNOME's session manager's
};

}  // namespace ucd
