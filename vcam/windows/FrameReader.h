// Reads uConnect's newest frame from the shared channel (vcam_channel.h).
#pragma once

#include <windows.h>

#include <cstdint>
#include <mutex>

#include "uconnect/vcam_channel.h"

namespace uconnect::vcam {

// Unix time in milliseconds, the channel's clock.
int64_t unixMs();

class FrameReader {
public:
    FrameReader() = default;
    ~FrameReader();
    FrameReader(const FrameReader&) = delete;
    FrameReader& operator=(const FrameReader&) = delete;

    // Writes one NV12 frame of width x height: uConnect's newest frame when
    // it has a fresh one of that size, otherwise the idle picture. `y` and
    // `uv` are the planes, both `pitch` bytes per row. Says which it was.
    bool fill(uint8_t* y, uint8_t* uv, LONG pitch, uint32_t width, uint32_t height);

private:
    bool open();
    void close();
    static void idle(uint8_t* y, uint8_t* uv, LONG pitch, uint32_t width, uint32_t height);

    std::mutex mutex_;
    HANDLE     file_    = INVALID_HANDLE_VALUE;
    HANDLE     mapping_ = nullptr;
    void*      view_    = nullptr;
    int64_t    lastOpenTry_ = 0;
};

}  // namespace uconnect::vcam
