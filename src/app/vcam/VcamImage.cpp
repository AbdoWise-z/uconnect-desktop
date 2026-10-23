#include "app/vcam/VcamOutput.hpp"

#include <QPainter>
#include <QTransform>

namespace ucd::vcam {

QImage fit(const QImage& image, int rotation, QSize size) {
    QImage canvas(size, QImage::Format_RGB32);
    canvas.fill(Qt::black);
    if (image.isNull() || size.isEmpty()) return canvas;
    const QImage turned = rotation % 360 == 0 ? image : image.transformed(QTransform().rotate(rotation));
    const QSize  scaled = turned.size().scaled(size, Qt::KeepAspectRatio);
    QPainter     painter(&canvas);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawImage(QRect(QPoint((size.width() - scaled.width()) / 2, (size.height() - scaled.height()) / 2), scaled),
                      turned);
    return canvas;
}

void toYuv420(const QImage& rgb, uint8_t* y, int yStride, uint8_t* u, uint8_t* v, int uvStride, int uvStep) {
    const int width = rgb.width() & ~1, height = rgb.height() & ~1;
    for (int row = 0; row < height; row += 2) {
        const auto* top    = reinterpret_cast<const QRgb*>(rgb.constScanLine(row));
        const auto* bottom = reinterpret_cast<const QRgb*>(rgb.constScanLine(row + 1));
        uint8_t*    y0     = y + static_cast<size_t>(row) * yStride;
        uint8_t*    y1     = y0 + yStride;
        uint8_t*    uRow   = u + static_cast<size_t>(row / 2) * uvStride;
        uint8_t*    vRow   = v + static_cast<size_t>(row / 2) * uvStride;
        for (int col = 0; col < width; col += 2) {
            int r = 0, g = 0, b = 0;
            const QRgb quad[4] = {top[col], top[col + 1], bottom[col], bottom[col + 1]};
            uint8_t*   out[4]  = {y0 + col, y0 + col + 1, y1 + col, y1 + col + 1};
            for (int i = 0; i < 4; ++i) {
                const int pr = qRed(quad[i]), pg = qGreen(quad[i]), pb = qBlue(quad[i]);
                *out[i] = static_cast<uint8_t>(((66 * pr + 129 * pg + 25 * pb + 128) >> 8) + 16);
                r += pr; g += pg; b += pb;
            }
            r /= 4; g /= 4; b /= 4;
            uRow[(col / 2) * uvStep] = static_cast<uint8_t>(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
            vRow[(col / 2) * uvStep] = static_cast<uint8_t>(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
        }
    }
}

}  // namespace ucd::vcam
