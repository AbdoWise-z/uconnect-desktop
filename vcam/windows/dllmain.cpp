// uconnect-vcam.dll: the COM server for the uConnect Camera media source.
// Registered machine-wide (Frame Server, a service, loads it), which takes an
// administrator: regsvr32 uconnect-vcam.dll, or uConnect's install step.
#include "Activator.h"

#include <string>

using namespace uconnect::vcam;

namespace {

HMODULE           g_module = nullptr;
std::atomic<long> g_locks{0};

CLSID sourceClsid() {
    CLSID clsid{};
    CLSIDFromString(kSourceClsid, &clsid);
    return clsid;
}

class ClassFactory : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IClassFactory, FtmBase> {
public:
    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        ComPtr<Activator> activator;
        const HRESULT hr = Microsoft::WRL::MakeAndInitialize<Activator>(&activator);
        return FAILED(hr) ? hr : activator->QueryInterface(riid, object);
    }
    IFACEMETHODIMP LockServer(BOOL lock) override {
        lock ? ++g_locks : --g_locks;
        return S_OK;
    }
};

std::wstring keyPath() { return std::wstring(L"Software\\Classes\\CLSID\\") + kSourceClsid; }

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* object) {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (clsid != sourceClsid()) return CLASS_E_CLASSNOTAVAILABLE;
    auto factory = Microsoft::WRL::Make<ClassFactory>();
    return factory ? factory->QueryInterface(riid, object) : E_OUTOFMEMORY;
}

// Frame Server may load and unload it per use; staying loaded is harmless.
STDAPI DllCanUnloadNow() { return S_FALSE; }

STDAPI DllRegisterServer() {
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(g_module, path, MAX_PATH)) return HRESULT_FROM_WIN32(GetLastError());
    HKEY key = nullptr;
    LSTATUS status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, (keyPath() + L"\\InprocServer32").c_str(), 0, nullptr, 0,
                                     KEY_WRITE, nullptr, &key, nullptr);
    if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);
    status = RegSetValueExW(key, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(path),
                            static_cast<DWORD>((wcslen(path) + 1) * sizeof(wchar_t)));
    static constexpr wchar_t kModel[] = L"Both";
    if (status == ERROR_SUCCESS)
        status = RegSetValueExW(key, L"ThreadingModel", 0, REG_SZ, reinterpret_cast<const BYTE*>(kModel), sizeof(kModel));
    RegCloseKey(key);
    if (status == ERROR_SUCCESS) {
        static constexpr wchar_t kName[] = L"uConnect Camera Source";
        RegSetKeyValueW(HKEY_LOCAL_MACHINE, keyPath().c_str(), nullptr, REG_SZ, kName, sizeof(kName));
    }
    return HRESULT_FROM_WIN32(status);
}

STDAPI DllUnregisterServer() {
    const LSTATUS status = RegDeleteTreeW(HKEY_LOCAL_MACHINE, keyPath().c_str());
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND ? S_OK : HRESULT_FROM_WIN32(status);
}
