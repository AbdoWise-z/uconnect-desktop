#pragma once
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QJsonObject>
#include "core/screen/Input.hpp"

namespace ucd {
class InputCapture : public QObject {
    Q_OBJECT
public:
    InputCapture(QObject* parent = nullptr) : QObject(parent) {}
    ~InputCapture() override { release(); }
    void attach(QObject* window);
    void setCaptured(bool captured);
    bool captured() const { return captured_; }
    void setReleaseUsage(quint16 usage) { releaseUsage_ = usage; }
signals:
    void input(const QJsonObject& message);
    void captureChanged(bool captured);
protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
private:
    void release();
    QPointer<QObject> window_;
    bool captured_ = false;
    quint16 releaseUsage_ = 0xe4;  // Right Ctrl
    QSet<quint16> textKeys_;
};
}
