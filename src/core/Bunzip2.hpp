#pragma once
// A bzip2 decompressor, for the one bzip2 file uConnect handles: Cisco's
// OpenH264 binary (OpenH264Installer). Neither Qt nor the build's libraries
// read bzip2. Checks every block's CRC and the stream's.
#include <QByteArray>
#include <QString>
#include <optional>

namespace ucd {

// The decompressed bytes of a whole .bz2 stream (concatenated streams too),
// or nullopt with `error` set. Stops past `maxSize` bytes of output.
std::optional<QByteArray> bunzip2(const QByteArray& compressed, qsizetype maxSize, QString* error = nullptr);

}  // namespace ucd
