// Virtual monitors on Windows, through the uConnect virtual display driver.
#include "VirtualDisplay.hpp"

#include <windows.h>
#include <setupapi.h>
#include <dxgi.h>

#include <QElapsedTimer>
#include <QThread>
#include <cstring>
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

QString interfacePath() {
    QString path;
    HDEVINFO set = SetupDiGetClassDevsW(&UCVDD_INTERFACE_GUID, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return path;
    SP_DEVICE_INTERFACE_DATA iface{};
    iface.cbSize = sizeof(iface);
    if (SetupDiEnumDeviceInterfaces(set, nullptr, &UCVDD_INTERFACE_GUID, 0, &iface)) {
        DWORD size = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &iface, nullptr, 0, &size, nullptr);
        std::vector<BYTE> buffer(size);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (size && SetupDiGetDeviceInterfaceDetailW(set, &iface, detail, size, nullptr, nullptr))
            path = QString::fromWCharArray(detail->DevicePath);
    }
    SetupDiDestroyDeviceInfoList(set);
    return path;
}

HANDLE openDriver() {
    const QString path = interfacePath();
    if (path.isEmpty()) return INVALID_HANDLE_VALUE;
    return CreateFileW(reinterpret_cast<const wchar_t*>(path.utf16()), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
}

bool ioctl(HANDLE handle, DWORD code, const void* in, DWORD inSize, void* out, DWORD outSize) {
    DWORD returned = 0;
    return DeviceIoControl(handle, code, const_cast<void*>(in), inSize, out, outSize, &returned, nullptr);
}

// The GPU with the most dedicated memory: on a laptop the discrete one, which
// has the hardware encoder. Rendering the monitor there keeps its frames on
// the encoder's GPU.
bool encoderAdapter(LUID& luid) {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) return false;
    SIZE_T best = 0; bool found = false;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && desc.DedicatedVideoMemory > best) {
            best = desc.DedicatedVideoMemory; luid = desc.AdapterLuid; found = true;
        }
        adapter->Release();
    }
    factory->Release();
    return found;
}

struct Path { bool found = false; QString gdiName; };

// The active display path of the target the driver created.
Path findPath(const LUID& adapter, UINT32 target) {
    Path out;
    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) return out;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS)
        return out;
    for (UINT32 i = 0; i < pathCount; ++i) {
        const auto& path = paths[i];
        if (path.targetInfo.adapterId.LowPart != adapter.LowPart || path.targetInfo.adapterId.HighPart != adapter.HighPart ||
            path.targetInfo.id != target) continue;
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) continue;
        out.found = true;
        out.gdiName = QString::fromWCharArray(source.viewGdiDeviceName);
        return out;
    }
    return out;
}

class WindowsDisplay final : public VirtualDisplay {
public:
    ~WindowsDisplay() override { destroy(); }

    bool create(const QByteArray& identity, const QString& name, const Mode& mode, QString* error) override {
        destroy();
        handle_ = openDriver();
        if (handle_ == INVALID_HANDLE_VALUE) return fail(error, QStringLiteral("The uConnect display driver is not installed."));
        UCVDD_VERSION version{};
        if (!ioctl(handle_, IOCTL_UCVDD_GET_VERSION, nullptr, 0, &version, sizeof(version)) || version.Major != UCVDD_PROTOCOL_MAJOR)
            return fail(error, QStringLiteral("The uConnect display driver needs an update."));
        LUID adapter{};
        if (encoderAdapter(adapter)) {
            UCVDD_RENDER_ADAPTER_PARAMS render{adapter};
            ioctl(handle_, IOCTL_UCVDD_SET_RENDER_ADAPTER, &render, sizeof(render), nullptr, 0);
        }

        UCVDD_ADD_PARAMS params{};
        std::memcpy(&params.MonitorId, identity.leftJustified(16, '\0').constData(), 16);
        params.Width = UINT(mode.width); params.Height = UINT(mode.height); params.RefreshMilliHz = UINT(mode.refreshMilliHz);
        const QByteArray ascii = name.toLatin1().left(13);
        std::memcpy(params.Name, ascii.constData(), size_t(ascii.size()));
        const QByteArray serial = identity.left(6).toHex().toUpper();  // 12 characters
        std::memcpy(params.Serial, serial.constData(), size_t(serial.size()));
        if (!ioctl(handle_, IOCTL_UCVDD_ADD, &params, sizeof(params), &added_, sizeof(added_)))
            return fail(error, QStringLiteral("The display driver could not add a monitor: %1").arg(lastError()));
        id_ = params.MonitorId; haveMonitor_ = true;

        // Windows shows a new monitor within a second or two. If it does not
        // (the user's projection mode is "PC screen only" or "Duplicate"),
        // extend the desktop onto it once.
        QElapsedTimer timer; timer.start();
        bool extended = false;
        for (;;) {
            const auto path = findPath(added_.AdapterLuid, added_.TargetId);
            if (path.found) { gdiName_ = path.gdiName; break; }
            if (!extended && timer.elapsed() > 1000) {
                extended = true;
                SetDisplayConfig(0, nullptr, 0, nullptr, SDC_TOPOLOGY_EXTEND | SDC_APPLY);
            }
            if (timer.elapsed() > 6000) return fail(error, QStringLiteral("Windows did not show the new monitor."));
            ioctl(handle_, IOCTL_UCVDD_PING, nullptr, 0, nullptr, 0);
            QThread::msleep(50);
        }
        if (!setMode(mode, error)) { destroy(); return false; }
        return true;
    }

    bool setMode(const Mode& mode, QString* error) override {
        // A failure here leaves the monitor as it was; the caller decides.
        const auto refuse = [&](const QString& text) { if (error) *error = text; return false; };
        if (gdiName_.isEmpty()) return refuse(QStringLiteral("No monitor."));
        const auto* device = reinterpret_cast<const wchar_t*>(gdiName_.utf16());
        DEVMODEW current{}; current.dmSize = sizeof(current);
        const int hz = (mode.refreshMilliHz + 500) / 1000;
        const bool same = EnumDisplaySettingsExW(device, ENUM_CURRENT_SETTINGS, &current, 0) &&
            int(current.dmPelsWidth) == mode.width && int(current.dmPelsHeight) == mode.height && int(current.dmDisplayFrequency) == hz;
        if (!same) {
            // Windows remembers the last mode of a monitor (by its EDID), which
            // may not be the one asked for now.
            DEVMODEW wanted{}; wanted.dmSize = sizeof(wanted);
            wanted.dmPelsWidth = DWORD(mode.width); wanted.dmPelsHeight = DWORD(mode.height); wanted.dmDisplayFrequency = DWORD(hz);
            wanted.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
            const LONG result = ChangeDisplaySettingsExW(device, &wanted, nullptr, CDS_UPDATEREGISTRY, nullptr);
            if (result != DISP_CHANGE_SUCCESSFUL)
                return refuse(QStringLiteral("Windows refused the mode %1x%2 at %3 Hz (%4).").arg(mode.width).arg(mode.height).arg(hz).arg(result));
        }
        DEVMODEW now{}; now.dmSize = sizeof(now);
        if (EnumDisplaySettingsExW(device, ENUM_CURRENT_SETTINGS, &now, 0))
            geometry_ = QRect(now.dmPosition.x, now.dmPosition.y, int(now.dmPelsWidth), int(now.dmPelsHeight));
        else
            geometry_ = QRect(0, 0, mode.width, mode.height);
        return true;
    }

    void destroy() override {
        if (handle_ == INVALID_HANDLE_VALUE) return;
        if (haveMonitor_) {
            UCVDD_REMOVE_PARAMS params{id_};
            ioctl(handle_, IOCTL_UCVDD_REMOVE, &params, sizeof(params), nullptr, 0);
        }
        CloseHandle(handle_);  // removes anything left with it
        handle_ = INVALID_HANDLE_VALUE; haveMonitor_ = false; gdiName_.clear(); geometry_ = {};
    }

    void keepAlive() override {
        if (handle_ != INVALID_HANDLE_VALUE) ioctl(handle_, IOCTL_UCVDD_PING, nullptr, 0, nullptr, 0);
    }

    QString captureId() const override { return gdiName_; }
    // Read live: Windows can place the monitor after it appears, and people
    // move it around in Settings > Display.
    QRect geometry() const override {
        DEVMODEW now{}; now.dmSize = sizeof(now);
        if (!gdiName_.isEmpty() && EnumDisplaySettingsExW(reinterpret_cast<const wchar_t*>(gdiName_.utf16()), ENUM_CURRENT_SETTINGS, &now, 0))
            return QRect(now.dmPosition.x, now.dmPosition.y, int(now.dmPelsWidth), int(now.dmPelsHeight));
        return geometry_;
    }
    QSize pixels() const override { return geometry().size(); }

private:
    bool fail(QString* error, const QString& text) {
        if (error) *error = text;
        destroy();
        return false;
    }

    HANDLE handle_ = INVALID_HANDLE_VALUE;
    GUID id_{};
    bool haveMonitor_ = false;
    UCVDD_ADD_RESULT added_{};
    QString gdiName_;
    QRect geometry_;
};

}  // namespace

Status status() {
    const HANDLE handle = openDriver();
    if (handle == INVALID_HANDLE_VALUE)
        return {Availability::NotInstalled, QStringLiteral("Install the uConnect display driver (Settings > Extended display).")};
    UCVDD_VERSION version{};
    const bool ok = ioctl(handle, IOCTL_UCVDD_GET_VERSION, nullptr, 0, &version, sizeof(version));
    CloseHandle(handle);
    if (!ok || version.Major != UCVDD_PROTOCOL_MAJOR)
        return {Availability::Outdated, QStringLiteral("Update the uConnect display driver (Settings > Extended display).")};
    return {Availability::Ready, {}};
}

std::unique_ptr<VirtualDisplay> create() { return std::make_unique<WindowsDisplay>(); }

}  // namespace ucd::display
