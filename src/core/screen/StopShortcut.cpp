#include "StopShortcut.hpp"
#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <QKeySequence>
#include <QSocketNotifier>
#ifdef Q_OS_WIN
#include <windows.h>
#elif defined(Q_OS_MACOS)
#include <Carbon/Carbon.h>
#elif defined(UCD_HAS_X11)
#include <X11/Xlib.h>
#include <X11/keysym.h>
#undef None
#endif

namespace ucd::screen {
#ifdef UCD_HAS_X11
namespace {
Display* grabbingDisplay = nullptr;
bool grabFailed = false;
XErrorHandler previousHandler = nullptr;
int grabError(Display* display, XErrorEvent* error) {
    if (display == grabbingDisplay) { grabFailed = true; return 0; }
    return previousHandler ? previousHandler(display, error) : 0;
}
}
#endif
struct StopShortcut::Impl : QAbstractNativeEventFilter {
    std::function<void()> stop;
    bool active = false;
#ifdef Q_OS_MACOS
    EventHotKeyRef key = nullptr;
    EventHandlerRef handler = nullptr;
    static OSStatus invoked(EventHandlerCallRef, EventRef, void* context) {
        static_cast<Impl*>(context)->stop(); return noErr;
    }
#elif defined(UCD_HAS_X11)
    Display* display = nullptr;
    std::unique_ptr<QSocketNotifier> notifier;
    int key = 0, modifiers = 0;
#endif
    bool nativeEventFilter(const QByteArray&, void* event, qintptr*) override {
#ifdef Q_OS_WIN
        const auto* msg = static_cast<MSG*>(event);
        if (active && msg->message == WM_HOTKEY && msg->wParam == 0x5543) { stop(); return true; }
#else
        Q_UNUSED(event);
#endif
        return false;
    }
};
StopShortcut::StopShortcut(std::function<void()> stop, QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>()) { impl_->stop = std::move(stop); }
StopShortcut::~StopShortcut() { disable(); }
void StopShortcut::disable() {
    if (!impl_->active) return;
#ifdef Q_OS_WIN
    UnregisterHotKey(nullptr, 0x5543); QCoreApplication::instance()->removeNativeEventFilter(impl_.get());
#elif defined(Q_OS_MACOS)
    UnregisterEventHotKey(impl_->key); RemoveEventHandler(impl_->handler);
    impl_->key = nullptr; impl_->handler = nullptr;
#elif defined(UCD_HAS_X11)
    impl_->notifier.reset();
    for (int locks : {0, int(LockMask), int(Mod2Mask), int(LockMask | Mod2Mask)})
        XUngrabKey(impl_->display, impl_->key, impl_->modifiers | locks, DefaultRootWindow(impl_->display));
    XCloseDisplay(impl_->display); impl_->display = nullptr;
#endif
    impl_->active = false;
}
QString StopShortcut::enable(const QString& sequence) {
    disable();
    const QKeySequence parsed(sequence, QKeySequence::PortableText);
    if (parsed.count() != 1) return "Use one shortcut with Ctrl/Alt/Shift and a letter.";
    const auto chord = parsed[0]; const auto key = chord.key(); const auto mods = chord.keyboardModifiers();
    if (key < Qt::Key_A || key > Qt::Key_Z || !mods) return "The stop shortcut needs modifiers and a letter.";
#ifdef Q_OS_WIN
    UINT native = MOD_NOREPEAT;
    if (mods & Qt::ControlModifier) native |= MOD_CONTROL;
    if (mods & Qt::AltModifier) native |= MOD_ALT;
    if (mods & Qt::ShiftModifier) native |= MOD_SHIFT;
    if (mods & Qt::MetaModifier) native |= MOD_WIN;
    if (!RegisterHotKey(nullptr, 0x5543, native, UINT(key))) return "Cannot register stop shortcut; it may be in use.";
    QCoreApplication::instance()->installNativeEventFilter(impl_.get());
#elif defined(Q_OS_MACOS)
    const int codes[]{0,11,8,2,14,3,5,4,34,38,40,37,46,45,31,35,12,15,1,17,32,9,13,7,16,6};
    UInt32 native = 0;
    if (mods & Qt::ControlModifier) native |= controlKey;
    if (mods & Qt::AltModifier) native |= optionKey;
    if (mods & Qt::ShiftModifier) native |= shiftKey;
    if (mods & Qt::MetaModifier) native |= cmdKey;
    const EventTypeSpec type{kEventClassKeyboard, kEventHotKeyPressed};
    if (InstallApplicationEventHandler(Impl::invoked, 1, &type, impl_.get(), &impl_->handler) != noErr)
        return "Cannot install stop shortcut handler.";
    if (RegisterEventHotKey(codes[key - Qt::Key_A], native, {'UCST', 1}, GetApplicationEventTarget(), 0, &impl_->key) != noErr) {
        RemoveEventHandler(impl_->handler); return "Cannot register stop shortcut.";
    }
#elif defined(UCD_HAS_X11)
    if (qEnvironmentVariable("XDG_SESSION_TYPE") == "wayland") return "Global stop shortcuts are unavailable on Wayland; use the sharing bar.";
    impl_->display = XOpenDisplay(nullptr);
    if (!impl_->display) return "Cannot open X11 display for the stop shortcut.";
    impl_->key = XKeysymToKeycode(impl_->display, XK_a + key - Qt::Key_A);
    impl_->modifiers = 0;
    if (mods & Qt::ControlModifier) impl_->modifiers |= ControlMask;
    if (mods & Qt::AltModifier) impl_->modifiers |= Mod1Mask;
    if (mods & Qt::ShiftModifier) impl_->modifiers |= ShiftMask;
    if (mods & Qt::MetaModifier) impl_->modifiers |= Mod4Mask;
    XSync(impl_->display, False);
    grabbingDisplay = impl_->display; grabFailed = false;
    previousHandler = XSetErrorHandler(grabError);
    for (int locks : {0, int(LockMask), int(Mod2Mask), int(LockMask | Mod2Mask)})
        XGrabKey(impl_->display, impl_->key, impl_->modifiers | locks, DefaultRootWindow(impl_->display), False, GrabModeAsync, GrabModeAsync);
    XSync(impl_->display, False); XSetErrorHandler(previousHandler); grabbingDisplay = nullptr;
    if (grabFailed) {
        XCloseDisplay(impl_->display); impl_->display = nullptr;
        return "Cannot register stop shortcut; it may be in use.";
    }
    impl_->notifier = std::make_unique<QSocketNotifier>(ConnectionNumber(impl_->display), QSocketNotifier::Read);
    connect(impl_->notifier.get(), &QSocketNotifier::activated, this, [this] {
        while (XPending(impl_->display)) { XEvent event; XNextEvent(impl_->display, &event); if (event.type == KeyPress) impl_->stop(); }
    });
#else
    return "Global stop shortcuts are unavailable on this platform; use the sharing bar.";
#endif
    impl_->active = true; return {};
}
}
