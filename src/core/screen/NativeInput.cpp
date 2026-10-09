#include "Input.hpp"
#include <array>
#include <algorithm>
#include <tuple>
#ifdef Q_OS_WIN
#include <windows.h>
#elif defined(Q_OS_MACOS)
#include <ApplicationServices/ApplicationServices.h>
#elif defined(UCD_HAS_XTEST)
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>
#undef None
#endif

namespace ucd::screen {
namespace {
class NativeInput : public InputBackend {
public:
#ifdef Q_OS_WIN
    QSet<ushort> unicodeHeld;
    int pendingText() const override { return unicodeHeld.size(); }
    void releaseText() override {
        const auto held = unicodeHeld;
        for (ushort character : held) {
            INPUT event{}; event.type = INPUT_KEYBOARD; event.ki.wScan = character;
            event.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
            if (SendInput(1, &event, sizeof(event)) == 1) unicodeHeld.remove(character);
        }
    }
    QString available() const override { return {}; }
    bool key(quint16 usage, bool down) override {
        const auto* code = mapping(usage); if (!code) return false;
        INPUT event{}; event.type = INPUT_KEYBOARD;
        event.ki.wScan = WORD(code->windowsScan & 255);
        event.ki.dwFlags = KEYEVENTF_SCANCODE | (code->windowsScan & 0x100 ? KEYEVENTF_EXTENDEDKEY : 0) | (down ? 0 : KEYEVENTF_KEYUP);
        if (usage == 0x48) { event.ki.wVk = VK_PAUSE; event.ki.wScan = 0; event.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP; }
        return SendInput(1, &event, sizeof(event)) == 1;
    }
    bool mouse(const QPoint& point, int previous, int buttons) override {
        INPUT event{}; event.type = INPUT_MOUSE;
        const int left = GetSystemMetrics(SM_XVIRTUALSCREEN), top = GetSystemMetrics(SM_YVIRTUALSCREEN);
        event.mi.dx = LONG(qint64(point.x() - left) * 65535 / std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1));
        event.mi.dy = LONG(qint64(point.y() - top) * 65535 / std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1));
        event.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        const DWORD down[]{MOUSEEVENTF_LEFTDOWN,MOUSEEVENTF_RIGHTDOWN,MOUSEEVENTF_MIDDLEDOWN};
        const DWORD up[]{MOUSEEVENTF_LEFTUP,MOUSEEVENTF_RIGHTUP,MOUSEEVENTF_MIDDLEUP};
        for (int i = 0; i < 3; ++i) if ((previous ^ buttons) & (1 << i)) event.mi.dwFlags |= buttons & (1 << i) ? down[i] : up[i];
        return SendInput(1, &event, sizeof(event)) == 1;
    }
    bool wheel(int dx, int dy) override {
        INPUT events[2]{}; int count = 0;
        for (const auto& [amount, flag] : {std::pair{dy, DWORD(MOUSEEVENTF_WHEEL)}, std::pair{dx, DWORD(MOUSEEVENTF_HWHEEL)}})
            if (amount) { auto& event = events[count++]; event.type = INPUT_MOUSE; event.mi.dwFlags = flag; event.mi.mouseData = DWORD(amount); }
        return !count || SendInput(count, events, sizeof(INPUT)) == UINT(count);
    }
    bool text(const QString& text) override {
        std::vector<INPUT> events;
        for (QChar character : text) unicodeHeld.insert(character.unicode());
        for (const QChar character : text) for (bool down : {true, false}) {
            INPUT event{}; event.type = INPUT_KEYBOARD; event.ki.wScan = character.unicode();
            event.ki.dwFlags = KEYEVENTF_UNICODE | (down ? 0 : KEYEVENTF_KEYUP); events.push_back(event);
        }
        const bool delivered = SendInput(events.size(), events.data(), sizeof(INPUT)) == events.size();
        if (delivered) unicodeHeld.clear(); else releaseText();
        return delivered;
    }
#elif defined(Q_OS_MACOS)
    std::array<bool, 8> modifiers{};
    CGEventFlags flags() const {
        CGEventFlags result = 0;
        const CGEventFlags masks[]{kCGEventFlagMaskControl,kCGEventFlagMaskShift,kCGEventFlagMaskAlternate,kCGEventFlagMaskCommand};
        for (int i = 0; i < 8; ++i) if (modifiers[i]) result |= masks[i % 4];
        return result;
    }
    mutable bool prompted = false;
    QString available() const override {
        if (AXIsProcessTrusted()) return {};
        if (!prompted) {
            // Once per run, let macOS show its own prompt, which leads to the switch.
            prompted = true;
            const void* keys[]{kAXTrustedCheckOptionPrompt}; const void* values[]{kCFBooleanTrue};
            const auto options = CFDictionaryCreate(nullptr, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
            AXIsProcessTrustedWithOptions(options); CFRelease(options);
        }
        return QString("Allow uConnect in System Settings > Privacy & Security > Accessibility, then request control again.");
    }
    bool post(CGEventRef event) {
        if (!event) return false;
        CGEventSetFlags(event, flags()); CGEventPost(kCGHIDEventTap, event); CFRelease(event); return true;
    }
    bool key(quint16 usage, bool down) override {
        const auto* code = mapping(usage); if (!code || code->macCode < 0 || !available().isEmpty()) return false;
        if (usage >= 0xe0 && usage <= 0xe7) modifiers[usage - 0xe0] = down;
        return post(CGEventCreateKeyboardEvent(nullptr, CGKeyCode(code->macCode), down));
    }
    bool mouse(const QPoint& point, int previous, int buttons) override {
        if (!available().isEmpty()) return false;
        const CGPoint at{double(point.x()), double(point.y())};
        const CGEventType downs[]{kCGEventLeftMouseDown,kCGEventRightMouseDown,kCGEventOtherMouseDown};
        const CGEventType ups[]{kCGEventLeftMouseUp,kCGEventRightMouseUp,kCGEventOtherMouseUp};
        const CGMouseButton native[]{kCGMouseButtonLeft,kCGMouseButtonRight,kCGMouseButtonCenter};
        bool delivered = true;
        for (int i = 0; i < 3; ++i) if ((previous ^ buttons) & (1 << i)) delivered &= post(CGEventCreateMouseEvent(nullptr, buttons & (1 << i) ? downs[i] : ups[i], at, native[i]));
        const auto motion = buttons & 1 ? kCGEventLeftMouseDragged : buttons & 2 ? kCGEventRightMouseDragged : buttons & 4 ? kCGEventOtherMouseDragged : kCGEventMouseMoved;
        return post(CGEventCreateMouseEvent(nullptr, motion, at, buttons & 2 ? kCGMouseButtonRight : buttons & 4 ? kCGMouseButtonCenter : kCGMouseButtonLeft)) && delivered;
    }
    bool wheel(int dx, int dy) override {
        if (!available().isEmpty()) return false;
        return post(CGEventCreateScrollWheelEvent(nullptr, kCGScrollEventUnitPixel, 2, dy / 4, dx / 4));
    }
    bool text(const QString& text) override {
        if (!available().isEmpty()) return false;
        for (qsizetype offset = 0; offset < text.size();) {
            const int count = text[offset].isHighSurrogate() && offset + 1 < text.size() ? 2 : 1;
            for (bool down : {true, false}) {
                auto event = CGEventCreateKeyboardEvent(nullptr, 0, down);
                if (!event) return false;
                CGEventKeyboardSetUnicodeString(event, count, reinterpret_cast<const UniChar*>(text.utf16() + offset)); post(event);
            }
            offset += count;
        }
        return true;
    }
#elif defined(UCD_HAS_XTEST)
    mutable Display* display = nullptr;
    int wheelX = 0, wheelY = 0;
    std::array<bool, 2> shifts{};
    ~NativeInput() override { if (display) XCloseDisplay(display); }
    QString available() const override {
        if (qEnvironmentVariable("XDG_SESSION_TYPE") == "wayland" ||
            (!qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY") && qEnvironmentVariable("XDG_SESSION_TYPE") != "x11"))
            return "Remote input is not supported on Wayland.";
        if (!display) display = XOpenDisplay(nullptr);
        if (!display) return "Cannot open the X11 display.";
        int event, error, major, minor;
        return XTestQueryExtension(display, &event, &error, &major, &minor) ? QString() : QString("XTest is unavailable.");
    }
    bool key(quint16 usage, bool down) override {
        const auto* code = mapping(usage); if (!code || !available().isEmpty()) return false;
        if (usage == 0xe1 || usage == 0xe5) shifts[usage == 0xe5] = down;
        const bool result = XTestFakeKeyEvent(display, code->linuxCode + 8, down, CurrentTime); XFlush(display); return result;
    }
    bool mouse(const QPoint& point, int previous, int buttons) override {
        if (!available().isEmpty()) return false;
        bool result = XTestFakeMotionEvent(display, -1, point.x(), point.y(), CurrentTime);
        const int native[]{1,3,2};
        for (int i = 0; i < 3; ++i) if ((previous ^ buttons) & (1 << i)) result &= bool(XTestFakeButtonEvent(display, native[i], bool(buttons & (1 << i)), CurrentTime));
        XFlush(display); return result;
    }
    bool wheel(int dx, int dy) override {
        if (!available().isEmpty()) return false;
        wheelX += dx; wheelY += dy;
        for (const auto& [amount, positive, negative] : {std::tuple{&wheelY,4,5}, std::tuple{&wheelX,7,6}}) {
            while (std::abs(*amount) >= 120) {
                const int button = *amount > 0 ? positive : negative;
                XTestFakeButtonEvent(display, button, True, CurrentTime); XTestFakeButtonEvent(display, button, False, CurrentTime);
                *amount += *amount > 0 ? -120 : 120;
            }
        }
        XFlush(display); return true;
    }
    bool text(const QString& text) override {
        if (!available().isEmpty()) return false;
        // XTest has no Unicode text primitive. Serve characters already in the
        // host keymap; fail explicitly for others rather than altering its map.
        for (char32_t cp : text.toUcs4()) {
            const KeySym symbol = cp < 256 ? KeySym(cp) : KeySym(0x01000000 | cp);
            const auto key = XKeysymToKeycode(display, symbol); if (!key) return false;
            int symbols = 0; auto* map = XGetKeyboardMapping(display, key, 1, &symbols);
            int column = -1; for (int i = 0; i < std::min(symbols, 2); ++i) if (map[i] == symbol) column = i;
            XFree(map); if (column < 0) return false;
            const auto shift = XKeysymToKeycode(display, XK_Shift_L);
            const auto rightShift = XKeysymToKeycode(display, XK_Shift_R);
            if (shifts[0]) XTestFakeKeyEvent(display, shift, False, CurrentTime);
            if (shifts[1]) XTestFakeKeyEvent(display, rightShift, False, CurrentTime);
            if (column) XTestFakeKeyEvent(display, shift, True, CurrentTime);
            XTestFakeKeyEvent(display, key, True, CurrentTime); XTestFakeKeyEvent(display, key, False, CurrentTime);
            if (column) XTestFakeKeyEvent(display, shift, False, CurrentTime);
            if (shifts[0]) XTestFakeKeyEvent(display, shift, True, CurrentTime);
            if (shifts[1]) XTestFakeKeyEvent(display, rightShift, True, CurrentTime);
        }
        XFlush(display); return true;
    }
#else
    QString available() const override { return "Remote input is unsupported on this platform."; }
    bool key(quint16, bool) override { return false; }
    bool mouse(const QPoint&, int, int) override { return false; }
    bool wheel(int, int) override { return false; }
    bool text(const QString&) override { return false; }
#endif
};
}
std::unique_ptr<InputBackend> nativeInputBackend() { return std::make_unique<NativeInput>(); }
}
