#pragma once
#include <QByteArray>
#include <QImage>
#include <QPoint>
#include <optional>

namespace ucd::screen {
struct CursorPosition {
    quint16 session = 0;
    quint32 sequence = 0;
    QPoint point;
    bool visible = false;
};
QByteArray cursorPacket(const CursorPosition& cursor);
std::optional<CursorPosition> parseCursor(const QByteArray& packet);
bool cursorNewer(quint32 next, quint32 previous);
struct NativeCursor {
    QPoint position;
    QPoint hotspot;
    QImage image;
    quintptr shape = 0;
    bool visible = true;
};
NativeCursor nativeCursor(quintptr previousShape);
}
