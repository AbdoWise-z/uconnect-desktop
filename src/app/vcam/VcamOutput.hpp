#pragma once
// Where the virtual camera's frames go: the operating system's virtual camera
// mechanism for this platform (see VirtualCamera.hpp). Driven from the
// virtual camera's worker thread only.
#include <QImage>
#include <QSize>
#include <QString>
#include <cstdint>
#include <memory>

namespace ucd::vcam {

class Output {
public:
    virtual ~Output() = default;
    // Makes the camera appear to apps; false with `error` set when it cannot.
    virtual bool start(QString& error) = 0;
    virtual void stop() = 0;
    // The size apps currently want, or empty when no app is watching (frames
    // need not be converted then).
    virtual QSize wanted() const = 0;
    // Sends one frame, already fitted to wanted().
    virtual void write(const QImage& frame) = 0;
};

// This platform's output, or null where there is none.
std::unique_ptr<Output> makeOutput();

// Whether this platform can have a virtual camera at all, and whether its
// system component is installed; `hint` says what is missing.
struct Support { bool supported = false; bool installed = false; QString hint; };
Support support();

// Fits `image` (rotated by `rotation` degrees clockwise) into `size`, keeping
// its aspect ratio with black bars, as RGB32.
QImage fit(const QImage& image, int rotation, QSize size);

// RGB32 -> 8-bit YUV 4:2:0 (BT.601, limited range). `u`/`v` advance by
// `uvStep` per chroma sample: 2 with v = u + 1 gives NV12, 1 gives I420.
void toYuv420(const QImage& rgb, uint8_t* y, int yStride, uint8_t* u, uint8_t* v, int uvStride, int uvStep);

}  // namespace ucd::vcam
