#include "InputCapture.hpp"
#include <QGuiApplication>
#include <QInputMethodEvent>

namespace ucd {
void InputCapture::attach(QObject* window) {
    release(); if (window_) window_->removeEventFilter(this);
    window_ = window; if (window_) window_->installEventFilter(this);
}
void InputCapture::setCaptured(bool captured) {
    if (!captured) { release(); return; }
    if (!window_ || captured_) return;
    captured_ = true; emit captureChanged(true);
}
void InputCapture::release() {
    if (!captured_) return;
    captured_ = false; altGr_ = false; textKeys_.clear(); emit input({{"t", "in.release"}}); emit captureChanged(false);
}
bool InputCapture::eventFilter(QObject* watched, QEvent* event) {
    if (!captured_ || watched != window_) return false;
    if (event->type() == QEvent::FocusOut || event->type() == QEvent::WindowDeactivate || event->type() == QEvent::Hide || event->type() == QEvent::Close) {
        release(); return false;
    }
    if (event->type() == QEvent::ShortcutOverride) { event->accept(); return true; }
    if (event->type() == QEvent::InputMethod) {
        const auto* method = static_cast<QInputMethodEvent*>(event);
        if (!method->commitString().isEmpty()) emit input({{"t", "in.text"}, {"text", method->commitString().left(256)}});
        event->accept(); return true;
    }
    if (event->type() != QEvent::KeyPress && event->type() != QEvent::KeyRelease) return false;
    const auto* key = static_cast<QKeyEvent*>(event);
    const auto usage = screen::hidUsage(*key, screen::keyboardPlatform());
    const bool down = event->type() == QEvent::KeyPress;
    if (usage == releaseUsage_) { if (down) release(); event->accept(); return true; }
    if (key->key() == Qt::Key_AltGr) {
        altGr_ = down;
        // Windows may precede AltGr with a synthesized left Ctrl press.
        // The resulting character is sent as text, without remote modifiers.
        if (down) emit input({{"t", "in.key"}, {"usage", 0xe0}, {"down", false}});
        event->accept(); return true;
    }
    if (key->key() >= Qt::Key_Dead_Grave && key->key() <= Qt::Key_Dead_Longsolidusoverlay) {
        if (down && usage) textKeys_.insert(usage);
        event->accept(); return true;
    }
    if (!down && key->isAutoRepeat()) return true;
    const bool printable = !key->text().isEmpty() && key->text().front().isPrint() &&
        (altGr_ || (key->modifiers() & Qt::GroupSwitchModifier) ||
         !(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)));
    if (down && printable) {
        emit input({{"t", "in.text"}, {"text", key->text().left(256)}});
        if (usage) textKeys_.insert(usage);
    } else if (!down && textKeys_.remove(usage)) {
        // Unicode text uses a down/up pair at the host; no physical key was held.
    } else if (usage) emit input({{"t", "in.key"}, {"usage", usage}, {"down", down}});
    event->accept(); return true;
}
}
