#pragma once
#include "Codec.hpp"

namespace ucd::screen {
struct DesktopResult {
    bool active = false;
    std::optional<EncodedFrame> frame;
    QString pipeline, detail;
};
// Owned and called on the encoder thread. Hardware surfaces stay native;
// unsupported adapters/rotations/permissions fall back to Qt's CPU pipeline.
class DesktopEncoder {
public:
    virtual ~DesktopEncoder() = default;
    virtual DesktopResult encode(const QString& monitor, const CodecOptions& options,
        quint32 number, quint32 captureMs, bool keyframe) = 0;
};
std::unique_ptr<DesktopEncoder> desktopEncoder();
}
