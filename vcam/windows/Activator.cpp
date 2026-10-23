#include "Activator.h"

namespace uconnect::vcam {

HRESULT Activator::RuntimeClassInitialize() { return MFCreateAttributes(&a_, 4); }

IFACEMETHODIMP Activator::ActivateObject(REFIID riid, void** object) {
    if (!object) return E_POINTER;
    *object = nullptr;
    std::lock_guard lock(mutex_);
    if (!source_) {
        const HRESULT hr = Microsoft::WRL::MakeAndInitialize<MediaSource>(&source_);
        if (FAILED(hr)) return hr;
    }
    return source_->QueryInterface(riid, object);
}

IFACEMETHODIMP Activator::ShutdownObject() {
    std::lock_guard lock(mutex_);
    if (source_) source_->Shutdown();
    source_.Reset();
    return S_OK;
}

IFACEMETHODIMP Activator::DetachObject() {
    std::lock_guard lock(mutex_);
    source_.Reset();
    return S_OK;
}

}  // namespace uconnect::vcam
