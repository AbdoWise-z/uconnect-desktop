#include "MediaSource.h"

namespace uconnect::vcam {

HRESULT MediaSource::RuntimeClassInitialize() {
    HRESULT hr = MFCreateEventQueue(&queue_);
    if (SUCCEEDED(hr)) hr = MFCreateAttributes(&attributes_, 4);
    if (SUCCEEDED(hr)) hr = Microsoft::WRL::MakeAndInitialize<MediaStream>(&stream_, this, std::make_shared<FrameReader>());
    ComPtr<IMFStreamDescriptor> streamDescriptor;
    if (SUCCEEDED(hr)) hr = stream_->GetStreamDescriptor(&streamDescriptor);
    IMFStreamDescriptor* descriptors[] = {streamDescriptor.Get()};
    if (SUCCEEDED(hr)) hr = MFCreatePresentationDescriptor(1, descriptors, &descriptor_);
    if (SUCCEEDED(hr)) hr = descriptor_->SelectStream(0);
    return hr;
}

// --- events ------------------------------------------------------------------

IFACEMETHODIMP MediaSource::BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) {
    std::lock_guard lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return queue_->BeginGetEvent(callback, state);
}

IFACEMETHODIMP MediaSource::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) {
    std::lock_guard lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return queue_->EndGetEvent(result, event);
}

IFACEMETHODIMP MediaSource::GetEvent(DWORD flags, IMFMediaEvent** event) {
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::lock_guard lock(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        queue = queue_;
    }
    return queue->GetEvent(flags, event);  // may block: not under the lock
}

IFACEMETHODIMP MediaSource::QueueEvent(MediaEventType type, REFGUID extendedType, HRESULT status,
                                       const PROPVARIANT* v) {
    std::lock_guard lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return queue_->QueueEventParamVar(type, extendedType, status, v);
}

// --- IMFMediaSource ----------------------------------------------------------

IFACEMETHODIMP MediaSource::GetCharacteristics(DWORD* characteristics) {
    if (!characteristics) return E_POINTER;
    std::lock_guard lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    *characteristics = MFMEDIASOURCE_IS_LIVE;
    return S_OK;
}

IFACEMETHODIMP MediaSource::CreatePresentationDescriptor(IMFPresentationDescriptor** descriptor) {
    if (!descriptor) return E_POINTER;
    std::lock_guard lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return descriptor_->Clone(descriptor);
}

IFACEMETHODIMP MediaSource::Start(IMFPresentationDescriptor* descriptor, const GUID* timeFormat,
                                  const PROPVARIANT* startPosition) {
    if (!descriptor) return E_INVALIDARG;
    if (timeFormat && *timeFormat != GUID_NULL) return MF_E_UNSUPPORTED_TIME_FORMAT;
    std::lock_guard lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;

    DWORD count = 0;
    HRESULT hr = descriptor->GetStreamDescriptorCount(&count);
    if (SUCCEEDED(hr) && count != 1) hr = E_INVALIDARG;
    BOOL selected = FALSE;
    ComPtr<IMFStreamDescriptor> chosen;
    if (SUCCEEDED(hr)) hr = descriptor->GetStreamDescriptorByIndex(0, &selected, &chosen);

    // The size the app chose arrives as the descriptor's current media type.
    if (SUCCEEDED(hr) && selected) {
        ComPtr<IMFMediaTypeHandler> chosenHandler, ownHandler;
        ComPtr<IMFMediaType>        type;
        ComPtr<IMFStreamDescriptor> own;
        if (SUCCEEDED(chosen->GetMediaTypeHandler(&chosenHandler)) && SUCCEEDED(chosenHandler->GetCurrentMediaType(&type))
            && SUCCEEDED(stream_->GetStreamDescriptor(&own)) && SUCCEEDED(own->GetMediaTypeHandler(&ownHandler)))
            ownHandler->SetCurrentMediaType(type.Get());
    }

    PROPVARIANT now;
    PropVariantInit(&now);
    now.vt            = VT_I8;
    now.hVal.QuadPart = MFGetSystemTime();
    if (SUCCEEDED(hr)) {
        if (selected) {
            const MediaEventType event = stream_->active() ? MEUpdatedStream : MENewStream;
            hr = queue_->QueueEventParamUnk(event, GUID_NULL, S_OK, static_cast<IMFMediaStream*>(stream_.Get()));
            if (SUCCEEDED(hr)) hr = stream_->start(&now);
        } else if (stream_->active()) {
            hr = stream_->stop();
        }
    }
    if (SUCCEEDED(hr)) hr = queue_->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, &now);
    (void)startPosition;  // live: always starts now
    return hr;
}

IFACEMETHODIMP MediaSource::Stop() {
    std::lock_guard lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    HRESULT hr = stream_->active() ? stream_->stop() : S_OK;
    if (SUCCEEDED(hr)) hr = queue_->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, nullptr);
    return hr;
}

IFACEMETHODIMP MediaSource::Pause() { return MF_E_INVALID_STATE_TRANSITION; }

IFACEMETHODIMP MediaSource::Shutdown() {
    std::lock_guard lock(mutex_);
    if (shutdown_) return S_OK;
    shutdown_ = true;
    if (stream_) stream_->shutdown();
    if (queue_) queue_->Shutdown();
    return S_OK;
}

// --- IMFMediaSourceEx --------------------------------------------------------

IFACEMETHODIMP MediaSource::GetSourceAttributes(IMFAttributes** attributes) {
    if (!attributes) return E_POINTER;
    std::lock_guard lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return attributes_.CopyTo(attributes);
}

IFACEMETHODIMP MediaSource::GetStreamAttributes(DWORD streamId, IMFAttributes** attributes) {
    if (!attributes) return E_POINTER;
    std::lock_guard lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    if (streamId != 0) return MF_E_INVALIDSTREAMNUMBER;
    return stream_->attributes(attributes);
}

// Frames are made in system memory, so there is no use for Direct3D.
IFACEMETHODIMP MediaSource::SetD3DManager(IUnknown*) { return S_OK; }

// --- IMFGetService, IKsControl -----------------------------------------------

IFACEMETHODIMP MediaSource::GetService(REFGUID, REFIID, LPVOID* object) {
    if (object) *object = nullptr;
    return MF_E_UNSUPPORTED_SERVICE;
}

IFACEMETHODIMP MediaSource::KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) {
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
IFACEMETHODIMP MediaSource::KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) {
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
IFACEMETHODIMP MediaSource::KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) {
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

}  // namespace uconnect::vcam
