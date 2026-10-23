#pragma once

#include <QImage>
#include <QString>
#include <QVideoFrame>
#include <functional>
#include <memory>
#include <optional>
#include "MediaTransport.hpp"

namespace ucd::screen {

struct CodecOptions {
    int width = 1920, fps = 30, bitrate = 4000000;
    QString encoder;  // empty selects the first usable hardware encoder
    QString ciscoLibrary;  // separately installed Cisco binary, only if enabled
    int threads = 1;  // the software encoder's threads (slices)
};

// What the encoder takes in, once it is open: its picture size and FFmpeg
// pixel format (an AVPixelFormat). A frame converted to it ahead of time is
// only copied in.
struct EncoderInput {
    QSize size;
    int format = -1;
    bool valid() const { return format >= 0 && !size.isEmpty(); }
    bool operator==(const EncoderInput&) const = default;
};

// A captured frame converted into an EncoderInput ahead of encoding, on any
// thread (Encoder::prepare): conversions of several frames can run at once
// while the encoder takes them one by one, in order.
struct PreparedFrame {
    EncoderInput input;
    std::shared_ptr<void> frame;  // the converted AVFrame
};

class Encoder {
public:
    Encoder();
    ~Encoder();
    std::optional<EncodedFrame> encode(const QImage& image, const CodecOptions& options,
        quint16 session, quint32 number, quint32 captureMs, bool keyframe);
    // The frame in its own pixel layout (BGRA, NV12, ...), converted once
    // into the encoder: cheaper than going through a QImage.
    std::optional<EncodedFrame> encode(const QVideoFrame& frame, const CodecOptions& options,
        quint16 session, quint32 number, quint32 captureMs, bool keyframe);
    // A frame already converted by prepare(): copied into the encoder.
    std::optional<EncodedFrame> encode(const PreparedFrame& frame, const CodecOptions& options,
        quint16 session, quint32 number, quint32 captureMs, bool keyframe);
    // Converts a captured frame (or image) into `input`. Thread-safe: runs on
    // any thread, several at once. Nothing when it cannot convert it.
    static std::optional<PreparedFrame> prepare(const QVideoFrame& frame, const QImage& image, const EncoderInput& input);
    // The input the encoder took last; invalid until it opens.
    EncoderInput input() const;
    QString name() const;
    QString error() const;
    void reset();
    static QStringList candidates();
private:
    struct Impl;
    struct Source;  // pixels in an FFmpeg layout
    std::optional<EncodedFrame> encodeSource(const Source& source, const CodecOptions& options,
        quint16 session, quint32 number, quint32 captureMs, bool keyframe);
    std::unique_ptr<Impl> impl_;
};

class Decoder {
public:
    Decoder();
    ~Decoder();
    // The picture as RGBA, converted on the CPU.
    QImage decode(const EncodedFrame& frame);
    // The picture in its decoded YUV layout, for a video sink to convert on
    // the GPU: far cheaper than decode() per frame. Without `picture`, the
    // frame only advances the decoder (it is a reference for the next one).
    // `decoded` tells whether a picture came out either way.
    QVideoFrame decodeFrame(const EncodedFrame& frame, bool picture = true, bool* decoded = nullptr);
    QString error() const;
    void reset();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

bool lgplCodecBuild();

// Whether a hardware encoder running at `applied` should move to `wanted`,
// `sinceMs` after its last change. Hardware encoders behind FFmpeg (NVENC at
// least) restart with a keyframe on any bitrate change, so the congestion
// control's small steps are batched: cuts go through at most once a second,
// raises in larger steps every few seconds, and small drift waits.
bool bitrateWorthChanging(int applied, int wanted, qint64 sinceMs);

}  // namespace ucd::screen
