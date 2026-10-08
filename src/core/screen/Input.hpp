#pragma once

#include <QJsonObject>
#include <QKeyEvent>
#include <QRect>
#include <QSet>
#include <functional>
#include <memory>
#include <vector>

namespace ucd::screen {
enum class KeyboardPlatform { Windows, Mac, X11 };
struct KeyMapping { quint16 usage; quint16 windowsScan; int macCode; int linuxCode; };
const std::vector<KeyMapping>& keyMappings();
const KeyMapping* mapping(quint16 usage);
quint16 hidUsage(const QKeyEvent& event, KeyboardPlatform platform);
KeyboardPlatform keyboardPlatform();

class InputBackend {
public:
    virtual ~InputBackend() = default;
    virtual QString available() const = 0;  // empty means supported/authorized
    virtual bool key(quint16 usage, bool down) = 0;
    virtual bool mouse(const QPoint& point, int previousButtons, int buttons) = 0;
    virtual bool wheel(int dx, int dy) = 0;
    virtual bool text(const QString& text) = 0;
};
std::unique_ptr<InputBackend> nativeInputBackend();

// Tracks only remotely pressed keys/buttons. Every stop path calls release();
// invalid input cannot add pressed state or reach the native APIs.
class InputState {
public:
    explicit InputState(std::unique_ptr<InputBackend> backend);
    ~InputState();
    QString available() const { return backend_->available(); }
    void setBounds(const QRect& bounds) { bounds_ = bounds; }
    bool apply(const QJsonObject& message);
    void release();
    int pressedCount() const { return keys_.size() + (buttons_ != 0); }
private:
    std::unique_ptr<InputBackend> backend_;
    QRect bounds_;
    QSet<quint16> keys_;
    QPoint point_;
    int buttons_ = 0;
};
}
