#include "Input.hpp"
#include <algorithm>
#include <cmath>

namespace ucd::screen {
const std::vector<KeyMapping>& keyMappings() {
    static const auto table = [] {
        std::vector<KeyMapping> out;
        constexpr int scans[]{30,48,46,32,18,33,34,35,23,36,37,38,50,49,24,25,16,19,31,20,22,47,17,45,21,44};
        constexpr int mac[]{0,11,8,2,14,3,5,4,34,38,40,37,46,45,31,35,12,15,1,17,32,9,13,7,16,6};
        for (int i = 0; i < 26; ++i) out.push_back({quint16(4 + i), quint16(scans[i]), mac[i], scans[i]});
        constexpr int digits[]{18,19,20,21,23,22,26,28,25,29};
        for (int i = 0; i < 10; ++i) out.push_back({quint16(0x1e + i), quint16(2 + i), digits[i], 2 + i});
        const KeyMapping rest[]{
            {0x28,28,36,28},{0x29,1,53,1},{0x2a,14,51,14},{0x2b,15,48,15},{0x2c,57,49,57},
            {0x2d,12,27,12},{0x2e,13,24,13},{0x2f,26,33,26},{0x30,27,30,27},{0x31,43,42,43},
            {0x33,39,41,39},{0x34,40,39,40},{0x35,41,50,41},{0x36,51,43,51},{0x37,52,47,52},{0x38,53,44,53},{0x39,58,57,58},
            {0x3a,59,122,59},{0x3b,60,120,60},{0x3c,61,99,61},{0x3d,62,118,62},{0x3e,63,96,63},{0x3f,64,97,64},
            {0x40,65,98,65},{0x41,66,100,66},{0x42,67,101,67},{0x43,68,109,68},{0x44,87,103,87},{0x45,88,111,88},
            {0x46,0x137,105,99},{0x47,70,107,70},{0x48,0x145,113,119},
            {0x49,0x152,114,110},{0x4a,0x147,115,102},{0x4b,0x149,116,104},{0x4c,0x153,117,111},
            {0x4d,0x14f,119,107},{0x4e,0x151,121,109},{0x4f,0x14d,124,106},{0x50,0x14b,123,105},
            {0x51,0x150,125,108},{0x52,0x148,126,103},{0x53,69,71,69},{0x54,0x135,75,98},
            {0x55,55,67,55},{0x56,74,78,74},{0x57,78,69,78},{0x58,0x11c,76,96},
            {0x59,79,83,79},{0x5a,80,84,80},{0x5b,81,85,81},{0x5c,75,86,75},{0x5d,76,87,76},
            {0x5e,77,88,77},{0x5f,71,89,71},{0x60,72,91,72},{0x61,73,92,73},{0x62,82,82,82},{0x63,83,65,83},
            {0x64,86,10,86},{0x65,0x15d,-1,127},
            {0xe0,29,59,29},{0xe1,42,56,42},{0xe2,56,58,56},{0xe3,0x15b,55,125},
            {0xe4,0x11d,62,97},{0xe5,54,60,54},{0xe6,0x138,61,100},{0xe7,0x15c,54,126}};
        out.insert(out.end(), std::begin(rest), std::end(rest)); return out;
    }();
    return table;
}
const KeyMapping* mapping(quint16 usage) {
    for (const auto& key : keyMappings()) if (key.usage == usage) return &key;
    return nullptr;
}
KeyboardPlatform keyboardPlatform() {
#ifdef Q_OS_WIN
    return KeyboardPlatform::Windows;
#elif defined(Q_OS_MACOS)
    return KeyboardPlatform::Mac;
#else
    return KeyboardPlatform::X11;
#endif
}
quint16 hidUsage(const QKeyEvent& event, KeyboardPlatform platform) {
    quint32 code = event.nativeScanCode();
    if ((code & 0xff00) == 0xe000) code = 0x100 | (code & 0xff);
    for (const auto& key : keyMappings()) {
        if (platform == KeyboardPlatform::Windows && code && code == key.windowsScan) return key.usage;
        if (platform == KeyboardPlatform::X11 && code && int(code) == key.linuxCode + 8) return key.usage;
        if (platform == KeyboardPlatform::Mac && int(event.nativeVirtualKey()) == key.macCode) return key.usage;
    }
    // Synthetic/offscreen events have no native code. Real layout-dependent
    // events always use physical codes above, including keypad/extended keys.
    if (event.key() >= Qt::Key_A && event.key() <= Qt::Key_Z) return quint16(4 + event.key() - Qt::Key_A);
    if (event.key() >= Qt::Key_1 && event.key() <= Qt::Key_9) return quint16(0x1e + event.key() - Qt::Key_1);
    if (event.key() == Qt::Key_0) return 0x27;
    const std::pair<int, quint16> special[]{
        {Qt::Key_Return,0x28},{Qt::Key_Enter,0x58},{Qt::Key_Escape,0x29},{Qt::Key_Backspace,0x2a},
        {Qt::Key_Tab,0x2b},{Qt::Key_Space,0x2c},{Qt::Key_Control,0xe0},{Qt::Key_Shift,0xe1},
        {Qt::Key_Alt,0xe2},{Qt::Key_Meta,0xe3},{Qt::Key_Left,0x50},{Qt::Key_Right,0x4f},
        {Qt::Key_Up,0x52},{Qt::Key_Down,0x51},{Qt::Key_Delete,0x4c}};
    for (const auto& [qt, usage] : special) if (event.key() == qt) return usage;
    if (event.key() >= Qt::Key_F1 && event.key() <= Qt::Key_F12) return quint16(0x3a + event.key() - Qt::Key_F1);
    return 0;
}

InputState::InputState(std::unique_ptr<InputBackend> backend) : backend_(std::move(backend)) {}
InputState::~InputState() { release(); }
bool InputState::apply(const QJsonObject& msg) {
    const auto type = msg.value("t").toString();
    if (type == "in.release") { release(); return true; }
    if (type == "in.key") {
        const int usage = msg.value("usage").toInt(-1);
        if (usage < 0 || usage > 255 || !mapping(quint16(usage)) || !msg.value("down").isBool()) return false;
        const bool down = msg.value("down").toBool();
        if (!down && !keys_.contains(quint16(usage))) return true;
        if (down) keys_.insert(quint16(usage));  // retain even after a partially failed native send
        const bool result = backend_->key(quint16(usage), down);
        if (!down && result) keys_.remove(quint16(usage));
        return result;
    }
    if (type == "in.mouse") {
        const auto x = msg.value("x"), y = msg.value("y"), buttons = msg.value("buttons");
        if (!x.isDouble() || !y.isDouble() || !buttons.isDouble() || !std::isfinite(x.toDouble()) || !std::isfinite(y.toDouble()) ||
            x.toDouble() < 0 || x.toDouble() > 1 || y.toDouble() < 0 || y.toDouble() > 1 ||
            buttons.toInt(-1) < 0 || buttons.toInt() > 7 || bounds_.isEmpty()) return false;
        point_ = {bounds_.x() + int(x.toDouble() * (bounds_.width() - 1)), bounds_.y() + int(y.toDouble() * (bounds_.height() - 1))};
        const int previous = buttons_; buttons_ = buttons.toInt();
        const bool delivered = backend_->mouse(point_, previous, buttons_);
        if (!delivered) buttons_ |= previous;
        return delivered;
    }
    if (type == "in.wheel") {
        const int dx = msg.value("dx").toInt(100000), dy = msg.value("dy").toInt(100000);
        if (dx < -2400 || dx > 2400 || dy < -2400 || dy > 2400) return false;
        return backend_->wheel(dx, dy);
    }
    if (type == "in.text") {
        const auto text = msg.value("text").toString();
        if (text.isEmpty() || text.size() > 256 || text.contains(QChar(0))) return false;
        return backend_->text(text);
    }
    return false;
}
void InputState::release() {
    backend_->releaseText();
    const auto keys = keys_;
    for (quint16 key : keys) if (backend_->key(key, false)) keys_.remove(key);
    if (buttons_ && backend_->mouse(point_, buttons_, 0)) buttons_ = 0;
}
}
