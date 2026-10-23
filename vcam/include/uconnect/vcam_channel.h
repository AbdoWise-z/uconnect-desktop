// The frame channel between uConnect and its virtual camera source (Windows).
//
// The virtual camera's media source runs inside Windows' camera service
// (Frame Server, as LocalService), not in uConnect, so frames cross processes
// through a memory-mapped file both can open:
//
//     %ProgramData%\uConnect\VirtualCamera\frames
//
// uConnect creates it (readable and writable by itself and LocalService) and
// writes NV12 frames; the source reads the newest one each time an app asks
// for a frame. The source says which size it streams (`want_*`, from the
// format the app chose) and when it last read (`heartbeat_ms`), so uConnect
// converts frames only while something watches, and to the right size.
//
// Two slots: the writer fills the one `frame_seq` does not point at, then
// publishes it by incrementing `frame_seq` (odd seq = slot 1). Shared by the
// MSVC-built source and the MinGW-built app: fixed-width fields only, and
// lock-free 64-bit atomics, which are address-free on x64.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace uconnect::vcam {

inline constexpr uint32_t kMagic   = 0x43564355;  // "UCVC"
inline constexpr uint32_t kVersion = 1;

// Largest frame a slot holds: 1920x1080 NV12.
inline constexpr uint32_t kMaxWidth  = 1920;
inline constexpr uint32_t kMaxHeight = 1080;
inline constexpr uint32_t kSlotBytes = kMaxWidth * kMaxHeight * 3 / 2;

inline constexpr uint32_t kHeaderBytes = 4096;
inline constexpr uint64_t kFileBytes   = kHeaderBytes + 2ull * kSlotBytes;

// A frame older than this is stale: the source shows its idle picture.
inline constexpr int64_t kStaleMs = 2000;

// The COM class of the source, registered by its DLL.
// {783E47B3-C847-4886-9318-D3B3B716D732}
inline constexpr wchar_t kSourceClsid[] = L"{783E47B3-C847-4886-9318-D3B3B716D732}";
inline constexpr wchar_t kFriendlyName[] = L"uConnect Camera";

struct SlotInfo {
    std::atomic<uint32_t> width;
    std::atomic<uint32_t> height;
    std::atomic<int64_t>  time_ms;  // when it was written (wall clock, ms)
};

struct Header {
    uint32_t magic;
    uint32_t version;
    uint32_t slot_bytes;
    uint32_t reserved0;

    // Written by the source.
    std::atomic<uint32_t> want_width;
    std::atomic<uint32_t> want_height;
    std::atomic<int64_t>  heartbeat_ms;  // last time it read a frame

    // Written by uConnect.
    std::atomic<int64_t> frame_seq;  // 0 = no frame yet; newest is slot seq % 2
    SlotInfo             frames[2];
};

static_assert(sizeof(Header) <= kHeaderBytes, "header must fit its page");
// The same layout from both compilers.
static_assert(sizeof(SlotInfo) == 16, "slot info layout");
static_assert(offsetof(Header, want_width) == 16 && offsetof(Header, heartbeat_ms) == 24
              && offsetof(Header, frame_seq) == 32 && offsetof(Header, frames) == 40,
              "header layout");
static_assert(std::atomic<int64_t>::is_always_lock_free, "needs lock-free 64-bit atomics");
static_assert(std::atomic<uint32_t>::is_always_lock_free, "needs lock-free 32-bit atomics");

inline uint8_t* slotData(void* base, int slot) {
    return static_cast<uint8_t*>(base) + kHeaderBytes + static_cast<uint64_t>(slot) * kSlotBytes;
}

}  // namespace uconnect::vcam
