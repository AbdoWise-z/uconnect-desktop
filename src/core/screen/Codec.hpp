#pragma once

#include <QImage>
#include <QString>
#include <memory>
#include <optional>
#include "MediaTransport.hpp"

namespace ucd::screen {

struct CodecOptions {
    int width = 1920, fps = 30, bitrate = 4000000;
    QString encoder;  // empty selects the first usable hardware encoder
    QString ciscoLibrary;  // separately installed Cisco binary, only if enabled
};

class Encoder {
public:
    Encoder();
    ~Encoder();
    std::optional<EncodedFrame> encode(const QImage& image, const CodecOptions& options,
        quint16 session, quint32 number, quint32 captureMs, bool keyframe);
    QString name() const;
    QString error() const;
    void reset();
    static QStringList candidates();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class Decoder {
public:
    Decoder();
    ~Decoder();
    QImage decode(const EncodedFrame& frame);
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
