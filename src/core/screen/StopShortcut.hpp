#pragma once
#include <QObject>
#include <functional>
#include <memory>

namespace ucd::screen {
class StopShortcut : public QObject {
public:
    explicit StopShortcut(std::function<void()> stop, QObject* parent = nullptr);
    ~StopShortcut() override;
    QString enable(const QString& sequence);
    void disable();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
