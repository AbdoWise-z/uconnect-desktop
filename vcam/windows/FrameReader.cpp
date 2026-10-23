#include "FrameReader.h"

#include <shlobj.h>

#include <cstring>
#include <string>

namespace uconnect::vcam {

int64_t unixMs() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    const int64_t t = (static_cast<int64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return (t - 116444736000000000LL) / 10000;  // 100 ns since 1601 -> ms since 1970
}

namespace {

std::wstring channelPath() {
    // Tests point the source at a channel of their own.
    wchar_t custom[MAX_PATH];
    if (GetEnvironmentVariableW(L"UCONNECT_VCAM_CHANNEL", custom, MAX_PATH) > 0) return custom;
    PWSTR programData = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &programData))) path = programData;
    CoTaskMemFree(programData);
    if (path.empty()) path = L"C:\\ProgramData";
    return path + L"\\uConnect\\VirtualCamera\\frames";
}

}  // namespace

FrameReader::~FrameReader() { close(); }

bool FrameReader::open() {
    if (view_) return true;
    // uConnect creates the channel when it starts the camera; until then, look
    // again now and then rather than on every frame.
    const int64_t now = unixMs();
    if (now - lastOpenTry_ < 1000) return false;
    lastOpenTry_ = now;

    file_ = CreateFileW(channelPath().c_str(), GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file_, &size) || static_cast<uint64_t>(size.QuadPart) < kFileBytes) {
        close();
        return false;
    }
    mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READWRITE, 0, 0, nullptr);
    if (mapping_) view_ = MapViewOfFile(mapping_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, static_cast<SIZE_T>(kFileBytes));
    const auto* header = static_cast<const Header*>(view_);
    if (!header || header->magic != kMagic || header->version != kVersion || header->slot_bytes != kSlotBytes) {
        close();
        return false;
    }
    return true;
}

void FrameReader::close() {
    if (view_) UnmapViewOfFile(view_);
    if (mapping_) CloseHandle(mapping_);
    if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_);
    view_    = nullptr;
    mapping_ = nullptr;
    file_    = INVALID_HANDLE_VALUE;
}

bool FrameReader::fill(uint8_t* y, uint8_t* uv, LONG pitch, uint32_t width, uint32_t height) {
    std::lock_guard lock(mutex_);
    if (!open()) {
        idle(y, uv, pitch, width, height);
        return false;
    }
    auto*         header = static_cast<Header*>(view_);
    const int64_t now    = unixMs();
    header->want_width.store(width);
    header->want_height.store(height);
    header->heartbeat_ms.store(now);

    const int64_t seq = header->frame_seq.load();
    if (seq > 0) {
        const int       slot = static_cast<int>(seq % 2);
        const SlotInfo& info = header->frames[slot];
        if (info.width.load() == width && info.height.load() == height && now - info.time_ms.load() < kStaleMs) {
            const uint8_t* src = slotData(view_, slot);
            for (uint32_t row = 0; row < height; ++row)
                std::memcpy(y + static_cast<size_t>(row) * pitch, src + static_cast<size_t>(row) * width, width);
            src += static_cast<size_t>(width) * height;
            for (uint32_t row = 0; row < height / 2; ++row)
                std::memcpy(uv + static_cast<size_t>(row) * pitch, src + static_cast<size_t>(row) * width, width);
            return true;
        }
    }
    idle(y, uv, pitch, width, height);
    return false;
}

// Nothing to show: a dark frame with a lighter band across the middle, so it
// reads as "on, but no picture" rather than a broken camera.
void FrameReader::idle(uint8_t* y, uint8_t* uv, LONG pitch, uint32_t width, uint32_t height) {
    const uint32_t bandTop = height * 7 / 16, bandBottom = height * 9 / 16;
    for (uint32_t row = 0; row < height; ++row)
        std::memset(y + static_cast<size_t>(row) * pitch, row >= bandTop && row < bandBottom ? 40 : 24, width);
    for (uint32_t row = 0; row < height / 2; ++row)
        std::memset(uv + static_cast<size_t>(row) * pitch, 128, width);
}

}  // namespace uconnect::vcam
