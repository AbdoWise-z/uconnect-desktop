// Windows 11: a Media Foundation virtual camera ("uConnect Camera") whose
// media source, uconnect-vcam.dll (vcam/windows), runs in Frame Server and
// reads frames from the channel this side writes (uconnect/vcam_channel.h).
#include "app/vcam/VcamOutput.hpp"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <devpropdef.h>
#include <sddl.h>
#include <shlobj.h>

#include <QDateTime>
#include <QFileInfo>

#include "uconnect/vcam_channel.h"

namespace ucd::vcam {

namespace channel = uconnect::vcam;

namespace {

// From the Windows SDK's mfvirtualcamera.h, which MinGW lacks. The layout must
// match it exactly: only the pointer comes from Windows, never a QueryInterface.
struct IMFCameraSyncObject;
struct IMFVirtualCamera : public IMFAttributes {
    virtual HRESULT STDMETHODCALLTYPE AddDeviceSourceInfo(LPCWSTR info) = 0;
    virtual HRESULT STDMETHODCALLTYPE AddProperty(const DEVPROPKEY* key, DEVPROPTYPE type, const BYTE* data, ULONG size) = 0;
    virtual HRESULT STDMETHODCALLTYPE AddRegistryEntry(LPCWSTR name, LPCWSTR subkey, DWORD type, const BYTE* data, ULONG size) = 0;
    virtual HRESULT STDMETHODCALLTYPE Start(IMFAsyncCallback* callback) = 0;
    virtual HRESULT STDMETHODCALLTYPE Stop() = 0;
    virtual HRESULT STDMETHODCALLTYPE Remove() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMediaSource(IMFMediaSource** source) = 0;
    virtual HRESULT STDMETHODCALLTYPE SendCameraProperty(REFGUID set, ULONG id, ULONG flags, void* payload, ULONG payloadSize,
                                                         void* data, ULONG dataSize, ULONG* written) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateSyncEvent(REFGUID set, ULONG id, ULONG flags, HANDLE event,
                                                      IMFCameraSyncObject** object) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateSyncSemaphore(REFGUID set, ULONG id, ULONG flags, HANDLE semaphore,
                                                          LONG adjustment, IMFCameraSyncObject** object) = 0;
    virtual HRESULT STDMETHODCALLTYPE Shutdown() = 0;
};
enum { kSoftwareCameraSource = 0, kLifetimeSession = 0, kAccessCurrentUser = 0 };
using CreateVirtualCamera = HRESULT(WINAPI*)(int type, int lifetime, int access, LPCWSTR friendlyName,
                                             LPCWSTR sourceId, const GUID* categories, ULONG categoryCount,
                                             IMFVirtualCamera** camera);

CreateVirtualCamera createFunction() {
    static const auto fn = [] {
        HMODULE module = LoadLibraryExW(L"mfsensorgroup.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return module ? reinterpret_cast<CreateVirtualCamera>(GetProcAddress(module, "MFCreateVirtualCamera")) : nullptr;
    }();
    return fn;
}

// The source's DLL as registered, if it is and the file is there.
QString registeredSource() {
    const std::wstring key = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + channel::kSourceClsid + L"\\InprocServer32";
    wchar_t path[MAX_PATH];
    DWORD   size = sizeof(path);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, path, &size) != ERROR_SUCCESS)
        return {};
    const QString file = QString::fromWCharArray(path);
    return QFileInfo::exists(file) ? file : QString();
}

QString hresult(HRESULT hr) { return QStringLiteral("0x%1").arg(quint32(hr), 8, 16, QLatin1Char('0')); }

std::wstring userSid() {
    HANDLE token = nullptr;
    std::wstring sid;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return sid;
    BYTE  buffer[256];
    DWORD size = 0;
    if (GetTokenInformation(token, TokenUser, buffer, sizeof(buffer), &size)) {
        LPWSTR text = nullptr;
        if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer)->User.Sid, &text)) {
            sid = text;
            LocalFree(text);
        }
    }
    CloseHandle(token);
    return sid;
}

// Security for what this side creates: this user and SYSTEM in full, and the
// camera service (LocalService) only what it needs.
struct Security {
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
    explicit Security(const std::wstring& sddl) {
        ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                             &attributes.lpSecurityDescriptor, nullptr);
    }
    ~Security() { LocalFree(attributes.lpSecurityDescriptor); }
    SECURITY_ATTRIBUTES* get() { return attributes.lpSecurityDescriptor ? &attributes : nullptr; }
};

class WindowsOutput : public Output {
public:
    ~WindowsOutput() override { stop(); }

    bool start(QString& error) override {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        comInitialized_ = SUCCEEDED(com);
        mfStarted_      = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
        const auto create = createFunction();
        if (!create) {
            error = QObject::tr("The virtual camera needs Windows 11.");
            return false;
        }
        if (registeredSource().isEmpty()) {
            error = QObject::tr("The uConnect Camera component is not installed.");
            return false;
        }
        if (!openChannel(error)) return false;
        HRESULT hr = create(kSoftwareCameraSource, kLifetimeSession, kAccessCurrentUser, channel::kFriendlyName,
                            channel::kSourceClsid, nullptr, 0, &camera_);
        if (SUCCEEDED(hr)) hr = camera_->Start(nullptr);
        if (FAILED(hr)) {
            error = QObject::tr("Windows would not start the virtual camera (%1).").arg(hresult(hr));
            stop();
            return false;
        }
        return true;
    }

    void stop() override {
        if (camera_) {
            camera_->Remove();
            camera_->Shutdown();
            camera_->Release();
            camera_ = nullptr;
        }
        closeChannel();
        if (mfStarted_) MFShutdown();
        if (comInitialized_) CoUninitialize();
        mfStarted_ = comInitialized_ = false;
    }

    QSize wanted() const override {
        if (!header()) return {};
        const qint64 heartbeat = header()->heartbeat_ms.load();
        if (QDateTime::currentMSecsSinceEpoch() - heartbeat > 3000) return {};
        const QSize size(int(header()->want_width.load()), int(header()->want_height.load()));
        if (size.width() <= 0 || size.height() <= 0 || size.width() > int(channel::kMaxWidth)
            || size.height() > int(channel::kMaxHeight) || size.width() % 2 || size.height() % 2)
            return {};
        return size;
    }

    void write(const QImage& frame) override {
        if (!header() || frame.width() > int(channel::kMaxWidth) || frame.height() > int(channel::kMaxHeight)) return;
        const int64_t seq  = header()->frame_seq.load() + 1;
        const int     slot = int(seq % 2);
        uint8_t*      y    = channel::slotData(view_, slot);
        uint8_t*      uv   = y + size_t(frame.width()) * frame.height();
        toYuv420(frame, y, frame.width(), uv, uv + 1, frame.width(), 2);
        header()->frames[slot].width.store(uint32_t(frame.width()));
        header()->frames[slot].height.store(uint32_t(frame.height()));
        header()->frames[slot].time_ms.store(QDateTime::currentMSecsSinceEpoch());
        header()->frame_seq.store(seq);
    }

private:
    channel::Header* header() const { return static_cast<channel::Header*>(view_); }

    bool openChannel(QString& error) {
        PWSTR        known = nullptr;
        std::wstring dir;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &known))) dir = known;
        CoTaskMemFree(known);
        if (dir.empty()) dir = L"C:\\ProgramData";
        const std::wstring sid = userSid();
        dir += L"\\uConnect";
        CreateDirectoryW(dir.c_str(), nullptr);
        dir += L"\\VirtualCamera";
        Security dirSecurity(L"D:(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;" + sid + L")(A;;0x1200a9;;;LS)");
        CreateDirectoryW(dir.c_str(), dirSecurity.get());

        Security fileSecurity(L"D:P(A;;FA;;;SY)(A;;FA;;;" + sid + L")(A;;FRFW;;;LS)");
        file_ = CreateFileW((dir + L"\\frames").c_str(), GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, fileSecurity.get(), OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) {
            error = QObject::tr("Could not create the virtual camera's frame file (error %1).").arg(GetLastError());
            return false;
        }
        mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READWRITE, DWORD(channel::kFileBytes >> 32),
                                      DWORD(channel::kFileBytes), nullptr);
        view_ = mapping_ ? MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, SIZE_T(channel::kFileBytes)) : nullptr;
        if (!view_) {
            error = QObject::tr("Could not map the virtual camera's frame file (error %1).").arg(GetLastError());
            closeChannel();
            return false;
        }
        header()->frame_seq.store(0);
        header()->slot_bytes = channel::kSlotBytes;
        header()->version    = channel::kVersion;
        header()->magic      = channel::kMagic;
        return true;
    }

    void closeChannel() {
        if (view_) UnmapViewOfFile(view_);
        if (mapping_) CloseHandle(mapping_);
        if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
        view_    = nullptr;
        mapping_ = nullptr;
        file_    = INVALID_HANDLE_VALUE;
    }

    IMFVirtualCamera* camera_  = nullptr;
    HANDLE            file_    = INVALID_HANDLE_VALUE;
    HANDLE            mapping_ = nullptr;
    void*             view_    = nullptr;
    bool              comInitialized_ = false, mfStarted_ = false;
};

}  // namespace

std::unique_ptr<Output> makeOutput() { return std::make_unique<WindowsOutput>(); }

Support support() {
    Support s;
    s.supported = createFunction() != nullptr;
    s.installed = s.supported && !registeredSource().isEmpty();
    s.hint      = !s.supported ? QObject::tr("The virtual camera needs Windows 11.")
                 : !s.installed ? QObject::tr("Install the uConnect Camera component (needs administrator rights).")
                                : QString();
    return s;
}

}  // namespace ucd::vcam
