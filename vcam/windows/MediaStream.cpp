#include "MediaSource.h"

#include <ksmedia.h>

namespace uconnect::vcam {

namespace {

constexpr UINT32   kFps           = 30;
constexpr LONGLONG kFrameDuration = 10'000'000 / kFps;  // 100 ns units

struct Size { UINT32 width, height; };
// What apps can choose from; uConnect scales the remote camera to the choice.
constexpr Size kSizes[] = {{1280, 720}, {1920, 1080}, {640, 480}};

HRESULT makeMediaType(Size size, IMFMediaType** out) {
    ComPtr<IMFMediaType> type;
    HRESULT hr = MFCreateMediaType(&type);
    if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    if (SUCCEEDED(hr)) hr = MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, size.width, size.height);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, kFps, 1);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_DEFAULT_STRIDE, size.width);
    if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_SAMPLE_SIZE, size.width * size.height * 3 / 2);
    if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_AVG_BITRATE, size.width * size.height * 12 * kFps);
    if (SUCCEEDED(hr)) *out = type.Detach();
    return hr;
}

}  // namespace

HRESULT MediaStream::RuntimeClassInitialize(MediaSource* source, std::shared_ptr<FrameReader> reader) {
    source_ = source;
    reader_ = std::move(reader);
    HRESULT hr = MFCreateEventQueue(&queue_);
    if (SUCCEEDED(hr)) hr = MFCreateAttributes(&attributes_, 8);
    // What Frame Server needs to treat it as a shared color capture stream.
    if (SUCCEEDED(hr)) hr = attributes_->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE);
    if (SUCCEEDED(hr)) hr = attributes_->SetUINT32(MF_DEVICESTREAM_STREAM_ID, 0);
    if (SUCCEEDED(hr)) hr = attributes_->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1);
    if (SUCCEEDED(hr)) hr = attributes_->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color);

    IMFMediaType* types[std::size(kSizes)] = {};
    for (size_t i = 0; SUCCEEDED(hr) && i < std::size(kSizes); ++i) hr = makeMediaType(kSizes[i], &types[i]);
    if (SUCCEEDED(hr)) hr = MFCreateStreamDescriptor(0, static_cast<DWORD>(std::size(kSizes)), types, &descriptor_);
    ComPtr<IMFMediaTypeHandler> handler;
    if (SUCCEEDED(hr)) hr = descriptor_->GetMediaTypeHandler(&handler);
    if (SUCCEEDED(hr)) hr = handler->SetCurrentMediaType(types[0]);
    if (SUCCEEDED(hr)) hr = attributes_->CopyAllItems(descriptor_.Get());
    for (IMFMediaType* type : types)
        if (type) type->Release();
    return hr;
}

// --- events ------------------------------------------------------------------

IFACEMETHODIMP MediaStream::BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) {
    std::lock_guard lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return queue_->BeginGetEvent(callback, state);
}

IFACEMETHODIMP MediaStream::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) {
    std::lock_guard lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return queue_->EndGetEvent(result, event);
}

IFACEMETHODIMP MediaStream::GetEvent(DWORD flags, IMFMediaEvent** event) {
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::lock_guard lock(mutex_);
        if (shutdown_) return MF_E_SHUTDOWN;
        queue = queue_;
    }
    return queue->GetEvent(flags, event);  // may block: not under the lock
}

IFACEMETHODIMP MediaStream::QueueEvent(MediaEventType type, REFGUID extendedType, HRESULT status,
                                       const PROPVARIANT* v) {
    std::lock_guard lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return queue_->QueueEventParamVar(type, extendedType, status, v);
}

// --- IMFMediaStream ----------------------------------------------------------

IFACEMETHODIMP MediaStream::GetMediaSource(IMFMediaSource** source) {
    if (!source) return E_POINTER;
    std::lock_guard lock(mutex_);
    if (shutdown_ || !source_) return MF_E_SHUTDOWN;
    return source_->QueryInterface(IID_PPV_ARGS(source));
}

IFACEMETHODIMP MediaStream::GetStreamDescriptor(IMFStreamDescriptor** descriptor) {
    if (!descriptor) return E_POINTER;
    std::lock_guard lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return descriptor_.CopyTo(descriptor);
}

IFACEMETHODIMP MediaStream::RequestSample(IUnknown* token) {
    std::lock_guard lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    if (state_ != MF_STREAM_STATE_RUNNING) return MF_E_MEDIA_SOURCE_WRONGSTATE;

    // Keep to the frame rate: apps ask again as soon as a sample arrives.
    const LONGLONG due = lastSampleTime_ + kFrameDuration;
    const LONGLONG now = MFGetSystemTime();
    if (lastSampleTime_ != 0 && now < due) Sleep(static_cast<DWORD>((due - now) / 10'000));

    ComPtr<IMFSample> sample;
    HRESULT hr = makeSample(&sample);
    if (SUCCEEDED(hr)) {
        lastSampleTime_ = MFGetSystemTime();
        hr = sample->SetSampleTime(lastSampleTime_);
    }
    if (SUCCEEDED(hr)) hr = sample->SetSampleDuration(kFrameDuration);
    if (SUCCEEDED(hr) && token) hr = sample->SetUnknown(MFSampleExtension_Token, token);
    if (SUCCEEDED(hr)) hr = queue_->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.Get());
    return hr;
}

HRESULT MediaStream::makeSample(IMFSample** out) {
    ComPtr<IMFMediaTypeHandler> handler;
    ComPtr<IMFMediaType>        type;
    UINT32                      width = 0, height = 0;
    HRESULT hr = descriptor_->GetMediaTypeHandler(&handler);
    if (SUCCEEDED(hr)) hr = handler->GetCurrentMediaType(&type);
    if (SUCCEEDED(hr)) hr = MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height);
    if (SUCCEEDED(hr) && (width == 0 || height == 0 || width > kMaxWidth || height > kMaxHeight)) hr = MF_E_INVALIDMEDIATYPE;

    ComPtr<IMFMediaBuffer> buffer;
    if (SUCCEEDED(hr)) hr = MFCreate2DMediaBuffer(width, height, MFVideoFormat_NV12.Data1, FALSE, &buffer);
    ComPtr<IMF2DBuffer2> buffer2d;
    if (SUCCEEDED(hr)) hr = buffer.As(&buffer2d);
    BYTE* scanline0 = nullptr;
    LONG  pitch     = 0;
    BYTE* start     = nullptr;
    DWORD length    = 0;
    if (SUCCEEDED(hr)) hr = buffer2d->Lock2DSize(MF2DBuffer_LockFlags_Write, &scanline0, &pitch, &start, &length);
    if (SUCCEEDED(hr)) {
        reader_->fill(scanline0, scanline0 + static_cast<size_t>(pitch) * height, pitch, width, height);
        buffer2d->Unlock2D();
        DWORD capacity = 0;
        hr = buffer->GetMaxLength(&capacity);
        if (SUCCEEDED(hr)) hr = buffer->SetCurrentLength(capacity);
    }
    ComPtr<IMFSample> sample;
    if (SUCCEEDED(hr)) hr = MFCreateSample(&sample);
    if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer.Get());
    if (SUCCEEDED(hr)) *out = sample.Detach();
    return hr;
}

// --- IMFMediaStream2 ---------------------------------------------------------

IFACEMETHODIMP MediaStream::SetStreamState(MF_STREAM_STATE state) {
    std::lock_guard lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    switch (state) {
    case MF_STREAM_STATE_RUNNING: return start(nullptr);
    case MF_STREAM_STATE_STOPPED: return stop();
    default: return MF_E_INVALID_STATE_TRANSITION;  // no pausing a live camera
    }
}

IFACEMETHODIMP MediaStream::GetStreamState(MF_STREAM_STATE* state) {
    if (!state) return E_POINTER;
    std::lock_guard lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    *state = state_;
    return S_OK;
}

// --- for the source ----------------------------------------------------------

HRESULT MediaStream::start(const PROPVARIANT* startTime) {
    std::lock_guard lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    state_          = MF_STREAM_STATE_RUNNING;
    lastSampleTime_ = 0;
    PROPVARIANT now;
    PropVariantInit(&now);
    now.vt            = VT_I8;
    now.hVal.QuadPart = MFGetSystemTime();
    return queue_->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, startTime ? startTime : &now);
}

HRESULT MediaStream::stop() {
    std::lock_guard lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    state_ = MF_STREAM_STATE_STOPPED;
    return queue_->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr);
}

HRESULT MediaStream::shutdown() {
    std::lock_guard lock(mutex_);
    if (shutdown_) return S_OK;
    shutdown_ = true;
    state_    = MF_STREAM_STATE_STOPPED;
    source_   = nullptr;
    if (queue_) queue_->Shutdown();
    return S_OK;
}

HRESULT MediaStream::attributes(IMFAttributes** attributes) {
    std::lock_guard lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    return attributes_.CopyTo(attributes);
}

// --- IKsControl --------------------------------------------------------------

IFACEMETHODIMP MediaStream::KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) {
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
IFACEMETHODIMP MediaStream::KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) {
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
IFACEMETHODIMP MediaStream::KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) {
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

}  // namespace uconnect::vcam
