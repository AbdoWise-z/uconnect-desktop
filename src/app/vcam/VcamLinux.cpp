// Linux: a v4l2loopback device, which apps see as a webcam. The module is the
// system component: sudo modprobe v4l2loopback exclusive_caps=1 card_label="uConnect Camera"
#include "app/vcam/VcamOutput.hpp"

#include <QDir>
#include <QObject>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

namespace ucd::vcam {

namespace {

// Loopback devices take a fixed format: apps get this size.
constexpr int kWidth = 1280, kHeight = 720;

bool isLoopback(int fd) {
    v4l2_capability caps{};
    if (ioctl(fd, VIDIOC_QUERYCAP, &caps) != 0) return false;
    return std::strncmp(reinterpret_cast<const char*>(caps.driver), "v4l2 loopback", sizeof(caps.driver)) == 0;
}

// The first loopback device, or an empty string.
QString findDevice() {
    const QStringList nodes = QDir("/dev").entryList({"video*"}, QDir::System, QDir::Name);
    for (const QString& node : nodes) {
        const QByteArray path = ("/dev/" + node).toLocal8Bit();
        const int        fd   = ::open(path.constData(), O_RDWR | O_NONBLOCK);
        if (fd < 0) continue;
        const bool loopback = isLoopback(fd);
        ::close(fd);
        if (loopback) return "/dev/" + node;
    }
    return {};
}

class LinuxOutput : public Output {
public:
    ~LinuxOutput() override { stop(); }

    bool start(QString& error) override {
        const QString device = findDevice();
        if (device.isEmpty()) {
            error = QObject::tr("No v4l2loopback device. Load it with: sudo modprobe v4l2loopback exclusive_caps=1 "
                                "card_label=\"uConnect Camera\"");
            return false;
        }
        fd_ = ::open(device.toLocal8Bit().constData(), O_RDWR);
        if (fd_ < 0) {
            error = QObject::tr("Could not open %1: %2").arg(device, QString::fromLocal8Bit(std::strerror(errno)));
            return false;
        }
        v4l2_format format{};
        format.type                 = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        format.fmt.pix.width        = kWidth;
        format.fmt.pix.height       = kHeight;
        format.fmt.pix.pixelformat  = V4L2_PIX_FMT_YUV420;
        format.fmt.pix.field        = V4L2_FIELD_NONE;
        format.fmt.pix.bytesperline = kWidth;
        format.fmt.pix.sizeimage    = kWidth * kHeight * 3 / 2;
        format.fmt.pix.colorspace   = V4L2_COLORSPACE_SRGB;
        if (ioctl(fd_, VIDIOC_S_FMT, &format) != 0) {
            error = QObject::tr("%1 would not take the camera format: %2")
                        .arg(device, QString::fromLocal8Bit(std::strerror(errno)));
            stop();
            return false;
        }
        buffer_.resize(size_t(kWidth) * kHeight * 3 / 2);
        return true;
    }

    void stop() override {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

    // A loopback device cannot tell whether an app reads it: always convert.
    QSize wanted() const override { return fd_ >= 0 ? QSize(kWidth, kHeight) : QSize(); }

    void write(const QImage& frame) override {
        if (fd_ < 0 || frame.size() != QSize(kWidth, kHeight)) return;
        uint8_t* y = buffer_.data();
        uint8_t* u = y + size_t(kWidth) * kHeight;
        uint8_t* v = u + size_t(kWidth / 2) * (kHeight / 2);
        toYuv420(frame, y, kWidth, u, v, kWidth / 2, 1);
        [[maybe_unused]] const ssize_t written = ::write(fd_, buffer_.data(), buffer_.size());
    }

private:
    int                  fd_ = -1;
    std::vector<uint8_t> buffer_;
};

}  // namespace

std::unique_ptr<Output> makeOutput() { return std::make_unique<LinuxOutput>(); }

Support support() {
    Support s;
    s.supported = true;
    s.installed = !findDevice().isEmpty();
    if (!s.installed)
        s.hint = QObject::tr("Load v4l2loopback: sudo modprobe v4l2loopback exclusive_caps=1 card_label=\"uConnect Camera\"");
    return s;
}

}  // namespace ucd::vcam
