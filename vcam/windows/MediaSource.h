// The uConnect Camera media source, which Windows' Frame Server loads for the
// virtual camera uConnect registers (MFCreateVirtualCamera). One video stream,
// NV12 at a few sizes, fed from uConnect through the shared frame channel.
#pragma once

#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <ks.h>
#include <ksproxy.h>
#include <wrl.h>

#include <memory>
#include <mutex>

#include "FrameReader.h"

namespace uconnect::vcam {

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::FtmBase;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::ChainInterfaces;

class MediaSource;

class MediaStream
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>,
                          ChainInterfaces<IMFMediaStream2, IMFMediaStream, IMFMediaEventGenerator>,
                          IKsControl, FtmBase> {
public:
    HRESULT RuntimeClassInitialize(MediaSource* source, std::shared_ptr<FrameReader> reader);

    // IMFMediaEventGenerator
    IFACEMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override;
    IFACEMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override;
    IFACEMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override;
    IFACEMETHODIMP QueueEvent(MediaEventType type, REFGUID extendedType, HRESULT status,
                              const PROPVARIANT* v) override;
    // IMFMediaStream
    IFACEMETHODIMP GetMediaSource(IMFMediaSource** source) override;
    IFACEMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** descriptor) override;
    IFACEMETHODIMP RequestSample(IUnknown* token) override;
    // IMFMediaStream2
    IFACEMETHODIMP SetStreamState(MF_STREAM_STATE state) override;
    IFACEMETHODIMP GetStreamState(MF_STREAM_STATE* state) override;
    // IKsControl: no properties.
    IFACEMETHODIMP KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) override;
    IFACEMETHODIMP KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) override;
    IFACEMETHODIMP KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) override;

    // For the source.
    HRESULT start(const PROPVARIANT* startTime);
    HRESULT stop();
    HRESULT shutdown();
    bool    active() const { return state_ == MF_STREAM_STATE_RUNNING; }
    HRESULT attributes(IMFAttributes** attributes);

private:
    HRESULT makeSample(IMFSample** sample);

    std::recursive_mutex          mutex_;
    MediaSource*                  source_ = nullptr;  // owns this stream
    std::shared_ptr<FrameReader>  reader_;
    ComPtr<IMFMediaEventQueue>    queue_;
    ComPtr<IMFStreamDescriptor>   descriptor_;
    ComPtr<IMFAttributes>         attributes_;
    MF_STREAM_STATE               state_    = MF_STREAM_STATE_STOPPED;
    bool                          shutdown_ = false;
    LONGLONG                      lastSampleTime_ = 0;
};

class MediaSource
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>,
                          ChainInterfaces<IMFMediaSourceEx, IMFMediaSource, IMFMediaEventGenerator>,
                          IMFGetService, IKsControl, FtmBase> {
public:
    HRESULT RuntimeClassInitialize();
    ~MediaSource() override { Shutdown(); }  // the stream points back at it

    // IMFMediaEventGenerator
    IFACEMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override;
    IFACEMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override;
    IFACEMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override;
    IFACEMETHODIMP QueueEvent(MediaEventType type, REFGUID extendedType, HRESULT status,
                              const PROPVARIANT* v) override;
    // IMFMediaSource
    IFACEMETHODIMP GetCharacteristics(DWORD* characteristics) override;
    IFACEMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** descriptor) override;
    IFACEMETHODIMP Start(IMFPresentationDescriptor* descriptor, const GUID* timeFormat,
                         const PROPVARIANT* startPosition) override;
    IFACEMETHODIMP Stop() override;
    IFACEMETHODIMP Pause() override;
    IFACEMETHODIMP Shutdown() override;
    // IMFMediaSourceEx
    IFACEMETHODIMP GetSourceAttributes(IMFAttributes** attributes) override;
    IFACEMETHODIMP GetStreamAttributes(DWORD streamId, IMFAttributes** attributes) override;
    IFACEMETHODIMP SetD3DManager(IUnknown* manager) override;
    // IMFGetService
    IFACEMETHODIMP GetService(REFGUID service, REFIID riid, LPVOID* object) override;
    // IKsControl: no properties.
    IFACEMETHODIMP KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG*) override;
    IFACEMETHODIMP KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG*) override;
    IFACEMETHODIMP KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG*) override;

private:
    std::recursive_mutex              mutex_;
    ComPtr<IMFMediaEventQueue>        queue_;
    ComPtr<IMFAttributes>             attributes_;
    ComPtr<IMFPresentationDescriptor> descriptor_;
    ComPtr<MediaStream>               stream_;
    bool                              shutdown_ = false;
};

}  // namespace uconnect::vcam
