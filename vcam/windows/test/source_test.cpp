// Drives the uConnect Camera source the way a camera app does, without
// registering it: loads the DLL, activates the source through its class
// factory and reads frames with a Source Reader. Frames come from a channel
// file this test writes, as uConnect would.
#include <windows.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include "uconnect/vcam_channel.h"

using Microsoft::WRL::ComPtr;
using namespace uconnect::vcam;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}

int64_t unixMs() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    return ((static_cast<int64_t>(ft.dwHighDateTime) << 32 | ft.dwLowDateTime) - 116444736000000000LL) / 10000;
}

struct Channel {
    HANDLE  file = INVALID_HANDLE_VALUE, mapping = nullptr;
    void*   view = nullptr;
    Header* header() { return static_cast<Header*>(view); }

    bool create(const std::wstring& path) {
        file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        mapping = CreateFileMappingW(file, nullptr, PAGE_READWRITE, static_cast<DWORD>(kFileBytes >> 32),
                                     static_cast<DWORD>(kFileBytes), nullptr);
        view = mapping ? MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0) : nullptr;
        if (!view) return false;
        header()->magic      = kMagic;
        header()->version    = kVersion;
        header()->slot_bytes = kSlotBytes;
        return true;
    }
    // Publishes a frame whose luma is a gradient with this offset.
    void publish(uint32_t width, uint32_t height, uint8_t offset) {
        const int64_t seq  = header()->frame_seq.load() + 1;
        const int     slot = static_cast<int>(seq % 2);
        uint8_t*      data = slotData(view, slot);
        for (uint32_t row = 0; row < height; ++row)
            for (uint32_t col = 0; col < width; ++col) data[row * width + col] = static_cast<uint8_t>(offset + col / 8);
        std::memset(data + width * height, 128, width * height / 2);
        header()->frames[slot].width.store(width);
        header()->frames[slot].height.store(height);
        header()->frames[slot].time_ms.store(unixMs());
        header()->frame_seq.store(seq);
    }
    ~Channel() {
        if (view) UnmapViewOfFile(view);
        if (mapping) CloseHandle(mapping);
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
};

// Reads one sample and returns the luma of pixel (x, 0), or -1.
int readLuma(IMFSourceReader* reader, int x, UINT32* width, UINT32* height) {
    ComPtr<IMFMediaType> type;
    if (FAILED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &type))) return -1;
    MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, width, height);
    for (int attempt = 0; attempt < 10; ++attempt) {
        DWORD stream = 0, flags = 0;
        LONGLONG time = 0;
        ComPtr<IMFSample> sample;
        if (FAILED(reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream, &flags, &time, &sample)))
            return -1;
        if (!sample) continue;  // a stream tick without a sample
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) return -1;
        BYTE* data = nullptr;
        DWORD length = 0;
        if (FAILED(buffer->Lock(&data, nullptr, &length))) return -1;
        const int value = length > static_cast<DWORD>(x) ? data[x] : -1;
        buffer->Unlock();
        return value;
    }
    return -1;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::printf("usage: vcam-source-test <path to uconnect-vcam.dll>\n");
        return 2;
    }
    const std::wstring channelPath =
        (std::filesystem::temp_directory_path() / L"uconnect-vcam-test-frames").wstring();
    SetEnvironmentVariableW(L"UCONNECT_VCAM_CHANNEL", channelPath.c_str());
    Channel channel;
    check(channel.create(channelPath), "creates the channel");
    channel.publish(1280, 720, 50);

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    HMODULE dll = LoadLibraryW(argv[1]);
    check(dll != nullptr, "loads the DLL");
    if (!dll) return 1;
    using GetClassObject = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, LPVOID*);
    auto getClassObject = reinterpret_cast<GetClassObject>(GetProcAddress(dll, "DllGetClassObject"));
    CLSID clsid{};
    CLSIDFromString(kSourceClsid, &clsid);
    ComPtr<IClassFactory> factory;
    check(getClassObject && SUCCEEDED(getClassObject(clsid, IID_PPV_ARGS(&factory))), "gets the class factory");
    ComPtr<IMFActivate> activate;
    check(factory && SUCCEEDED(factory->CreateInstance(nullptr, IID_PPV_ARGS(&activate))), "creates the activator");
    ComPtr<IMFMediaSource> source;
    check(activate && SUCCEEDED(activate->ActivateObject(IID_PPV_ARGS(&source))), "activates the media source");
    if (!source) return 1;

    ComPtr<IMFMediaSourceEx> sourceEx;
    ComPtr<IMFAttributes>    streamAttributes;
    GUID                     category{};
    check(SUCCEEDED(source.As(&sourceEx)) && SUCCEEDED(sourceEx->GetStreamAttributes(0, &streamAttributes))
              && SUCCEEDED(streamAttributes->GetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, &category)),
          "describes its stream as a capture pin");

    ComPtr<IMFSourceReader> reader;
    check(SUCCEEDED(MFCreateSourceReaderFromMediaSource(source.Get(), nullptr, &reader)), "opens a source reader");
    if (!reader) return 1;

    UINT32 width = 0, height = 0;
    int luma = readLuma(reader.Get(), 8 * 10, &width, &height);
    check(width == 1280 && height == 720, "streams 1280x720 by default");
    check(luma == 50 + 10, "delivers uConnect's frame");
    check(channel.header()->want_width.load() == 1280 && channel.header()->want_height.load() == 720,
          "tells uConnect the size it streams");
    check(unixMs() - channel.header()->heartbeat_ms.load() < 2000, "keeps a heartbeat");

    channel.publish(1280, 720, 90);
    luma = readLuma(reader.Get(), 0, &width, &height);
    check(luma == 90, "follows new frames");

    // Another size: uConnect has no frame of that size yet, so it shows its idle picture.
    ComPtr<IMFMediaType> other;
    bool found = false;
    for (DWORD i = 0; SUCCEEDED(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &other)); ++i) {
        UINT32 w = 0, h = 0;
        MFGetAttributeSize(other.Get(), MF_MT_FRAME_SIZE, &w, &h);
        if (w == 640 && h == 480) {
            found = SUCCEEDED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, other.Get()));
            break;
        }
        other.Reset();
    }
    check(found, "offers 640x480");
    luma = readLuma(reader.Get(), 0, &width, &height);
    check(width == 640 && luma == 24, "shows the idle picture without a frame of that size");
    check(channel.header()->want_width.load() == 640, "asks uConnect for the new size");
    channel.publish(640, 480, 120);
    luma = readLuma(reader.Get(), 0, &width, &height);
    check(luma == 120, "delivers the frame once uConnect sends that size");

    reader.Reset();
    source->Shutdown();
    source.Reset();
    activate->ShutdownObject();
    MFShutdown();
    std::printf("%s\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
