#include "QrImage.hpp"

#include <QPainter>
#include <algorithm>

#include "third_party/qrcodegen/qrcodegen.hpp"

namespace ucd {

QrImage::QrImage(QQuickItem* parent) : QQuickPaintedItem(parent) {}

void QrImage::setText(const QString& t) {
    if (t == text_) return;
    text_ = t;
    emit textChanged();
    update();
}

void QrImage::setColor(const QColor& c) {
    if (c == color_) return;
    color_ = c;
    emit colorChanged();
    update();
}

void QrImage::setBackground(const QColor& c) {
    if (c == background_) return;
    background_ = c;
    emit backgroundChanged();
    update();
}

void QrImage::paint(QPainter* painter) {
    const qreal side = std::min(width(), height());
    if (side <= 0) return;
    painter->fillRect(QRectF(0, 0, side, side), background_);
    if (text_.isEmpty()) return;

    using qrcodegen::QrCode;
    QrCode qr = QrCode::encodeText("", QrCode::Ecc::MEDIUM);
    try {
        qr = QrCode::encodeText(text_.toUtf8().constData(), QrCode::Ecc::MEDIUM);
    } catch (...) {
        return;  // too long to encode; leave the background
    }

    const int n = qr.getSize();
    const int quiet = 2;  // the required quiet zone around the symbol
    const qreal scale = side / (n + 2 * quiet);
    painter->setPen(Qt::NoPen);
    painter->setBrush(color_);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
            if (qr.getModule(x, y))
                // The small overlap keeps modules seamless at fractional scale.
                painter->drawRect(QRectF((x + quiet) * scale, (y + quiet) * scale,
                                         scale + 0.6, scale + 0.6));
}

}  // namespace ucd
