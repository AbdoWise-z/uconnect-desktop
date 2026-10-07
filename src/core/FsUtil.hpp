#pragma once
// Filesystem helpers shared by the services that let other devices reach
// this one's files (storage, sync). They exist to keep every access inside
// the folder it was meant for.

#include <QFileInfo>
#include <QString>

namespace ucd {

#ifdef Q_OS_WIN
inline constexpr Qt::CaseSensitivity kPathCase = Qt::CaseInsensitive;
#else
inline constexpr Qt::CaseSensitivity kPathCase = Qt::CaseSensitive;
#endif

// CON, NUL, COM1, ... with or without an extension.
bool isReservedDeviceName(const QString& name);
// One component of a relative path. Rejects anything that could step outside
// a folder or name something other than a plain file: "..", NUL, and on
// Windows backslashes, alternate data streams ("file:stream") and devices.
bool isValidComponent(const QString& c);
// Where a path really leads, with every link along it followed; empty if it
// does not exist.
QString realPath(const QString& path);
// `path` is `root` or inside it (both real paths).
bool isWithin(const QString& path, const QString& root);
// `name` in `folder`, or "name (1).ext" and so on if that is taken.
QString uniquePath(const QString& folder, const QString& name);
// Removes a file, folder or link without ever following a link.
bool removeEntry(const QFileInfo& fi);
// Moves `source` onto `target` in one step, replacing a file already there.
bool replaceWith(const QString& source, const QString& target);

}  // namespace ucd
