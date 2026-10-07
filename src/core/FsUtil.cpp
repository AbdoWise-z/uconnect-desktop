#include "FsUtil.hpp"

#include <cstdio>

#include <QDir>
#include <QFile>
#include <QStringList>

#ifdef Q_OS_WIN
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ucd {

bool isReservedDeviceName(const QString& name) {
    static const QStringList reserved = [] {
        QStringList r{"CON", "PRN", "AUX", "NUL"};
        for (int i = 1; i <= 9; ++i) r << QStringLiteral("COM%1").arg(i) << QStringLiteral("LPT%1").arg(i);
        return r;
    }();
    return reserved.contains(name.section(QLatin1Char('.'), 0, 0).trimmed().toUpper());
}

// One component of a virtual path. Rejects anything that could step outside
// a folder or name something other than a plain file: "..", NUL, and on
// Windows backslashes, alternate data streams ("file:stream") and devices.
bool isValidComponent(const QString& c) {
    if (c.isEmpty() || c == QLatin1String(".") || c == QLatin1String("..") || c.contains(QChar(0)))
        return false;
#ifdef Q_OS_WIN
    if (c.contains(QLatin1Char('\\')) || c.contains(QLatin1Char(':')) || isReservedDeviceName(c))
        return false;
#endif
    return true;
}

// Where a path really leads, with every link along it followed; empty if it
// does not exist. On Windows QFileInfo::canonicalFilePath() does not follow
// junctions, so a junction inside a shared folder could point anywhere; ask
// the filesystem for the final path of an open handle instead.
QString realPath(const QString& path) {
#ifdef Q_OS_WIN
    const std::wstring native = QDir::toNativeSeparators(QDir::cleanPath(path)).toStdWString();
    HANDLE h = CreateFileW(native.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD n = GetFinalPathNameByHandleW(h, buffer.data(), static_cast<DWORD>(buffer.size()),
                                        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (n >= buffer.size()) {
        buffer.resize(n + 1);
        n = GetFinalPathNameByHandleW(h, buffer.data(), static_cast<DWORD>(buffer.size()),
                                      FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    }
    CloseHandle(h);
    if (n == 0 || n >= buffer.size()) return {};
    QString out = QString::fromWCharArray(buffer.data(), static_cast<qsizetype>(n));
    if (out.startsWith(QLatin1String("\\\\?\\UNC\\"))) out = QLatin1String("\\\\") + out.mid(8);
    else if (out.startsWith(QLatin1String("\\\\?\\"))) out = out.mid(4);
    return QDir::fromNativeSeparators(out);
#else
    return QFileInfo(path).canonicalFilePath();
#endif
}

bool isWithin(const QString& path, const QString& root) {
    if (path.compare(root, kPathCase) == 0) return true;
    const QString prefix = root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/');
    return path.startsWith(prefix, kPathCase);
}

QString uniquePath(const QString& folder, const QString& name) {
    const QDir dir(folder);
    QString candidate = dir.filePath(name);
    if (!QFileInfo::exists(candidate)) return candidate;
    const QFileInfo fi(name);
    const QString base   = fi.completeBaseName();
    const QString suffix = fi.suffix();
    for (int n = 1;; ++n) {
        candidate = dir.filePath(suffix.isEmpty() ? QStringLiteral("%1 (%2)").arg(base).arg(n)
                                                  : QStringLiteral("%1 (%2).%3").arg(base).arg(n).arg(suffix));
        if (!QFileInfo::exists(candidate)) return candidate;
    }
}

// Removes a file, folder or link without ever following a link: a link (or
// junction) is removed itself, so a delete cannot reach what it points to.
// QDir::removeRecursively() descends into junctions on Windows, so it is not
// used here.
bool removeEntry(const QFileInfo& fi) {
    const QString path = fi.absoluteFilePath();
    if (fi.isSymLink() || fi.isJunction()) return QFile::remove(path) || QDir().rmdir(path);
    if (fi.isDir()) {
        const QFileInfoList children = QDir(path).entryInfoList(
            QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
        for (const QFileInfo& child : children)
            if (!removeEntry(child)) return false;
        return QDir().rmdir(path);
    }
    QFile file(path);
    if (file.remove()) return true;
    file.setPermissions(file.permissions() | QFileDevice::WriteOwner);  // read-only files on Windows
    return file.remove();
}

// Moves `source` onto `target` in one step, replacing a file already there.
bool replaceWith(const QString& source, const QString& target) {
#ifdef Q_OS_WIN
    return MoveFileExW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(source).utf16()),
                       reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(target).utf16()),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return std::rename(QFile::encodeName(source).constData(), QFile::encodeName(target).constData()) == 0;
#endif
}

}  // namespace ucd
