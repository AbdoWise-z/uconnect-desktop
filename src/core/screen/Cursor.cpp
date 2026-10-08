#include "Cursor.hpp"
#include <QtEndian>

namespace ucd::screen {
QByteArray cursorPacket(const CursorPosition& cursor) {
    if (!cursor.session) return {};
    QByteArray result(16, '\0'); result[0] = 2;
    auto* bytes = reinterpret_cast<uchar*>(result.data());
    qToBigEndian(cursor.session, bytes + 1); qToBigEndian(cursor.sequence, bytes + 3);
    qToBigEndian(qint32(cursor.point.x()), bytes + 7); qToBigEndian(qint32(cursor.point.y()), bytes + 11);
    result[15] = cursor.visible ? 1 : 0; return result;
}
std::optional<CursorPosition> parseCursor(const QByteArray& packet) {
    if (packet.size() != 16 || packet[0] != 2 || (packet[15] != 0 && packet[15] != 1)) return std::nullopt;
    const auto* bytes = reinterpret_cast<const uchar*>(packet.constData());
    const auto session = qFromBigEndian<quint16>(bytes + 1); if (!session) return std::nullopt;
    return CursorPosition{session, qFromBigEndian<quint32>(bytes + 3),
        {qFromBigEndian<qint32>(bytes + 7), qFromBigEndian<qint32>(bytes + 11)}, bool(packet[15])};
}
bool cursorNewer(quint32 next, quint32 previous) { return qint32(next - previous) > 0; }
}
