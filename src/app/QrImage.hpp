#pragma once
// A QML item that paints a QR code of `text`, so the desktop can show the
// topic invite link for a phone to scan. Uses the vendored Nayuki qrcodegen
// (MIT). Registered as UConnect.QrCode in main.cpp.

#include <QColor>
#include <QQuickPaintedItem>
#include <QString>

namespace ucd {

class QrImage : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(QString text READ text WRITE setText NOTIFY textChanged)
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY colorChanged)
    Q_PROPERTY(QColor background READ background WRITE setBackground NOTIFY backgroundChanged)

public:
    explicit QrImage(QQuickItem* parent = nullptr);

    QString text() const { return text_; }
    void setText(const QString& t);
    QColor color() const { return color_; }
    void setColor(const QColor& c);
    QColor background() const { return background_; }
    void setBackground(const QColor& c);

    void paint(QPainter* painter) override;

signals:
    void textChanged();
    void colorChanged();
    void backgroundChanged();

private:
    QString text_;
    QColor color_ = Qt::black;
    QColor background_ = Qt::white;
};

}  // namespace ucd
