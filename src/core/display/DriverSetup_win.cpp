#include "DriverSetup.hpp"

#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <newdev.h>
#include <initguid.h>
#include <devpkey.h>
#include <shellapi.h>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <cstring>
#include <cwchar>
#include <functional>
#include <vector>

#include "uconnect-vdd.h"

namespace ucd::display {
namespace {

QString lastError(DWORD code = GetLastError()) {
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    const QString out = text ? QString::fromWCharArray(text).trimmed() : QStringLiteral("error %1").arg(code);
    if (text) LocalFree(text);
    return out;
}

bool fail(QString* error, const QString& text) { if (error) *error = text; return false; }

// Devices whose hardware IDs include the driver's.
void forEachDevice(const std::function<void(HDEVINFO, SP_DEVINFO_DATA&)>& f) {
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES);
    if (set == INVALID_HANDLE_VALUE) return;
    SP_DEVINFO_DATA dev{}; dev.cbSize = sizeof(dev);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); ++i) {
        wchar_t ids[1024] = {};
        if (!SetupDiGetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, nullptr,
                reinterpret_cast<BYTE*>(ids), sizeof(ids) - 2 * sizeof(wchar_t), nullptr)) continue;
        for (const wchar_t* id = ids; *id; id += wcslen(id) + 1) {
            if (_wcsicmp(id, UCVDD_HARDWARE_ID) == 0) { f(set, dev); break; }
        }
    }
    SetupDiDestroyDeviceInfoList(set);
}

}  // namespace

QString bundledDriverInf() {
    const QString inf = QDir(QCoreApplication::applicationDirPath()).filePath("drivers/uconnect-vdd/UConnectVDD.inf");
    return QFileInfo::exists(inf) ? inf : QString();
}

bool installDriver(const QString& infPath, QString* error) {
    const QString inf = QDir::toNativeSeparators(QFileInfo(infPath).absoluteFilePath());
    const auto* infW = reinterpret_cast<const wchar_t*>(inf.utf16());
    bool exists = false;
    forEachDevice([&](HDEVINFO, SP_DEVINFO_DATA&) { exists = true; });
    if (!exists) {
        // A root-enumerated device for the driver to bind to.
        GUID classGuid; wchar_t className[MAX_CLASS_NAME_LEN];
        if (!SetupDiGetINFClassW(infW, &classGuid, className, MAX_CLASS_NAME_LEN, nullptr))
            return fail(error, QStringLiteral("Cannot read the driver package: %1").arg(lastError()));
        HDEVINFO set = SetupDiCreateDeviceInfoList(&classGuid, nullptr);
        SP_DEVINFO_DATA dev{}; dev.cbSize = sizeof(dev);
        wchar_t hardwareId[64] = {};  // a multi-string: zeros past the id end it
        wcsncpy(hardwareId, UCVDD_HARDWARE_ID, 60);
        const DWORD bytes = DWORD((wcslen(hardwareId) + 2) * sizeof(wchar_t));
        const bool created = SetupDiCreateDeviceInfoW(set, className, &classGuid, nullptr, nullptr, DICD_GENERATE_ID, &dev) &&
            SetupDiSetDeviceRegistryPropertyW(set, &dev, SPDRP_HARDWAREID, reinterpret_cast<BYTE*>(hardwareId), bytes) &&
            SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, &dev);
        const QString why = created ? QString() : lastError();
        SetupDiDestroyDeviceInfoList(set);
        if (!created) return fail(error, QStringLiteral("Cannot create the display device: %1").arg(why));
    }
    BOOL reboot = FALSE;
    if (!UpdateDriverForPlugAndPlayDevicesW(nullptr, UCVDD_HARDWARE_ID, infW, INSTALLFLAG_FORCE, &reboot))
        return fail(error, QStringLiteral("Cannot install the display driver: %1").arg(lastError()));
    return true;
}

bool uninstallDriver(QString* error) {
    // DiUninstallDevice is missing from some MinGW import libraries.
    using Uninstall = BOOL(WINAPI*)(HWND, HDEVINFO, PSP_DEVINFO_DATA, DWORD, PBOOL);
    const auto uninstallDevice = reinterpret_cast<Uninstall>(
        reinterpret_cast<void*>(GetProcAddress(LoadLibraryW(L"newdev.dll"), "DiUninstallDevice")));
    if (!uninstallDevice) return fail(error, QStringLiteral("This Windows cannot remove devices this way."));
    std::vector<std::wstring> packages;
    QString failure;
    forEachDevice([&](HDEVINFO set, SP_DEVINFO_DATA& dev) {
        wchar_t infPath[MAX_PATH] = {}; DEVPROPTYPE type = 0;
        if (SetupDiGetDevicePropertyW(set, &dev, &DEVPKEY_Device_DriverInfPath, &type,
                reinterpret_cast<BYTE*>(infPath), sizeof(infPath), nullptr, 0)) packages.push_back(infPath);
        BOOL reboot = FALSE;
        if (!uninstallDevice(nullptr, set, &dev, 0, &reboot)) failure = lastError();
    });
    for (const auto& oem : packages) SetupUninstallOEMInfW(oem.c_str(), SUOI_FORCEDELETE, nullptr);
    if (!failure.isEmpty()) return fail(error, QStringLiteral("Cannot remove the display device: %1").arg(failure));
    return true;
}

bool runElevated(const QStringList& arguments, QString* error) {
    const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    QStringList quoted;
    for (const auto& argument : arguments) quoted << '"' + QString(argument).replace('"', "\\\"") + '"';
    const QString parameters = quoted.join(' ');
    SHELLEXECUTEINFOW info{}; info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    info.lpVerb = L"runas";
    info.lpFile = reinterpret_cast<const wchar_t*>(exe.utf16());
    info.lpParameters = reinterpret_cast<const wchar_t*>(parameters.utf16());
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info)) {
        const DWORD code = GetLastError();
        return fail(error, code == ERROR_CANCELLED ? QStringLiteral("Administrator permission was not given.") : lastError(code));
    }
    if (!info.hProcess) return fail(error, QStringLiteral("The installer did not start."));
    WaitForSingleObject(info.hProcess, 120000);
    DWORD exitCode = 1;
    GetExitCodeProcess(info.hProcess, &exitCode);
    CloseHandle(info.hProcess);
    if (exitCode != 0) return fail(error, QStringLiteral("The display driver could not be changed (code %1).").arg(exitCode));
    return true;
}

int handleDriverCommand(int argc, char* argv[]) {
    if (argc < 3 || std::strcmp(argv[1], "--display-driver") != 0) return -1;
    QCoreApplication app(argc, argv);  // for paths and strings
    QString error;
    bool ok = false;
    if (std::strcmp(argv[2], "install") == 0) {
        const QString inf = argc > 3 ? QString::fromLocal8Bit(argv[3]) : bundledDriverInf();
        ok = !inf.isEmpty() && installDriver(inf, &error);
        if (inf.isEmpty()) error = QStringLiteral("No driver package next to the application.");
    } else if (std::strcmp(argv[2], "remove") == 0) {
        ok = uninstallDriver(&error);
    }
    if (!ok) fprintf(stderr, "%s\n", qPrintable(error));
    return ok ? 0 : 2;
}

}  // namespace ucd::display
