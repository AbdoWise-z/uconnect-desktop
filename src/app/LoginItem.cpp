#include "LoginItem.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace ucd::login {
namespace {

// What runs: this executable, or on Linux the AppImage it was started from
// (the executable itself lives in a mount that is gone after it exits).
QString program() {
#if defined(Q_OS_LINUX)
    if (const QString image = qEnvironmentVariable("APPIMAGE"); !image.isEmpty()) return image;
#endif
    return QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
}

// The name, as a file name or label: "uConnect Desktop (work)" -> "uconnect-desktop-work".
[[maybe_unused]] QString slug(const QString& name) {
    QString s = name.toLower();
    s.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("-"));
    while (s.endsWith(QLatin1Char('-'))) s.chop(1);
    while (s.startsWith(QLatin1Char('-'))) s.remove(0, 1);
    return s.isEmpty() ? QStringLiteral("uconnect-desktop") : s;
}

#if defined(Q_OS_WIN)

const wchar_t* const RunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
// Where Task Manager's Startup apps keeps what was turned off there.
const wchar_t* const ApprovedKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";

QString quoted(const QString& arg) {
    return arg.isEmpty() || arg.contains(QLatin1Char(' ')) ? QLatin1Char('"') + arg + QLatin1Char('"') : arg;
}

QString valueName(const QString& name) {
    QString n = name;
    return n.replace(QLatin1Char('\\'), QLatin1Char('-'));
}

QString readRun(const QString& name) {
    const std::wstring value = valueName(name).toStdWString();
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, RunKey, value.c_str(), RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS)
        return {};
    std::wstring data(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, RunKey, value.c_str(), RRF_RT_REG_SZ, nullptr, data.data(), &size) != ERROR_SUCCESS)
        return {};
    return QString::fromWCharArray(data.c_str());
}

// Turned off in Task Manager: the first byte of its entry is odd (3, not 2).
bool turnedOff(const QString& name) {
    const std::wstring value = valueName(name).toStdWString();
    BYTE data[12] = {};
    DWORD size = sizeof(data);
    if (RegGetValueW(HKEY_CURRENT_USER, ApprovedKey, value.c_str(), RRF_RT_REG_BINARY, nullptr, data, &size)
        != ERROR_SUCCESS)
        return false;
    return size > 0 && (data[0] & 1);
}

#elif defined(Q_OS_MACOS)

QString plistPath(const QString& name) {
    return QDir::homePath() + QStringLiteral("/Library/LaunchAgents/com.uconnect.") + slug(name)
         + QStringLiteral(".plist");
}

QString escaped(const QString& text) { return text.toHtmlEscaped(); }

#else

QString desktopPath(const QString& name) {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/autostart/")
         + slug(name) + QStringLiteral(".desktop");
}

// An Exec= argument, as the desktop entry specification quotes it.
QString execArg(QString arg) {
    arg.replace(QLatin1Char('%'), QStringLiteral("%%"));
    static const QRegularExpression reserved(QStringLiteral("[\\s\"'\\\\><~|&;$*?#()`]"));
    if (!arg.isEmpty() && !arg.contains(reserved)) return arg;
    for (const char* c : {"\\", "\"", "`", "$"}) arg.replace(QLatin1String(c), QStringLiteral("\\") + QLatin1String(c));
    return QLatin1Char('"') + arg + QLatin1Char('"');
}

QString readDesktop(const QString& name) {
    QFile file(desktopPath(name));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    return QString::fromUtf8(file.readAll());
}

#endif

}  // namespace

QString command(const QString& name) {
#if defined(Q_OS_WIN)
    return readRun(name);
#elif defined(Q_OS_MACOS)
    QFile file(plistPath(name));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    const QString plist = QString::fromUtf8(file.readAll());
    const qsizetype from = plist.indexOf(QStringLiteral("<array>")), to = plist.indexOf(QStringLiteral("</array>"));
    if (from < 0 || to < from) return {};
    QStringList parts;
    static const QRegularExpression string(QStringLiteral("<string>([^<]*)</string>"));
    for (const auto& m : string.globalMatch(plist.mid(from, to - from))) parts << m.captured(1);
    return parts.join(QLatin1Char(' '));
#else
    static const QRegularExpression exec(QStringLiteral("^Exec=(.*)$"), QRegularExpression::MultilineOption);
    return exec.match(readDesktop(name)).captured(1);
#endif
}

QString location(const QString& name) {
#if defined(Q_OS_WIN)
    return QStringLiteral("HKEY_CURRENT_USER\\") + QString::fromWCharArray(RunKey) + QLatin1Char('\\') + valueName(name);
#elif defined(Q_OS_MACOS)
    return plistPath(name);
#else
    return desktopPath(name);
#endif
}

bool isEnabled(const QString& name) {
#if defined(Q_OS_WIN)
    return !readRun(name).isEmpty() && !turnedOff(name);
#elif defined(Q_OS_MACOS)
    return QFile::exists(plistPath(name));
#else
    const QString entry = readDesktop(name);
    return !entry.isEmpty() && !entry.contains(QStringLiteral("\nHidden=true"))
        && !entry.contains(QStringLiteral("\nX-GNOME-Autostart-enabled=false"));
#endif
}

bool setEnabled(const QString& name, bool enabled, const QStringList& arguments) {
#if defined(Q_OS_WIN)
    const std::wstring value = valueName(name).toStdWString();
    // Clears a "turned off" left by Task Manager either way: on is on again.
    RegDeleteKeyValueW(HKEY_CURRENT_USER, ApprovedKey, value.c_str());
    if (!enabled) {
        const LSTATUS status = RegDeleteKeyValueW(HKEY_CURRENT_USER, RunKey, value.c_str());
        return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
    }
    QStringList parts{quoted(program())};
    for (const QString& arg : arguments) parts << quoted(arg);
    const std::wstring data = parts.join(QLatin1Char(' ')).toStdWString();
    return RegSetKeyValueW(HKEY_CURRENT_USER, RunKey, value.c_str(), REG_SZ, data.c_str(),
                           DWORD((data.size() + 1) * sizeof(wchar_t)))
        == ERROR_SUCCESS;
#elif defined(Q_OS_MACOS)
    const QString path = plistPath(name);
    if (!enabled) return !QFile::exists(path) || QFile::remove(path);
    // Loaded by launchd at the next sign-in; RunAtLoad starts it then, once.
    QString args;
    for (const QString& arg : QStringList{program()} + arguments)
        args += QStringLiteral("        <string>%1</string>\n").arg(escaped(arg));
    const QString plist = QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n<dict>\n"
        "    <key>Label</key>\n    <string>com.uconnect.%1</string>\n"
        "    <key>ProgramArguments</key>\n    <array>\n%2    </array>\n"
        "    <key>RunAtLoad</key>\n    <true/>\n"
        "    <key>ProcessType</key>\n    <string>Interactive</string>\n"
        "</dict>\n</plist>\n").arg(slug(name), args);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Text) && file.write(plist.toUtf8()) >= 0 && file.commit();
#else
    const QString path = desktopPath(name);
    if (!enabled) return !QFile::exists(path) || QFile::remove(path);
    QStringList exec{execArg(program())};
    for (const QString& arg : arguments) exec << execArg(arg);
    const QString entry = QStringLiteral(
        "[Desktop Entry]\nType=Application\nName=%1\nComment=Start uConnect when you sign in\n"
        "Exec=%2\nIcon=uconnect-desktop\nTerminal=false\nX-GNOME-Autostart-enabled=true\n")
        .arg(name, exec.join(QLatin1Char(' ')));
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Text) && file.write(entry.toUtf8()) >= 0 && file.commit();
#endif
}

}  // namespace ucd::login
