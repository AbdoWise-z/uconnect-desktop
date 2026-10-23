#pragma once
// Shows a camera this device is watching to the operating system as a webcam
// ("uConnect Camera"), so any app can use it. One at a time; it lasts while
// that camera is watched. Frames are fitted and converted on a worker thread,
// and only while an app is reading the camera (where the system says so).
// The system side is per platform (app/vcam/VcamOutput.hpp).
#include <QObject>
#include <QVideoFrame>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace ucd {

namespace vcam { class Output; }

class VirtualCamera : public QObject {
    Q_OBJECT
    // Whether this system can have one, and whether its component is installed
    // (`hint` says what is missing).
    Q_PROPERTY(bool supported READ supported NOTIFY changed)
    Q_PROPERTY(bool installed READ installed NOTIFY changed)
    Q_PROPERTY(bool canInstall READ canInstall CONSTANT)
    Q_PROPERTY(bool installing READ installing NOTIFY changed)
    Q_PROPERTY(QString hint READ hint NOTIFY changed)
    // The device whose camera it shows, empty when off; `live` once the system
    // has it, `inUse` while an app reads it.
    Q_PROPERTY(QString peer READ peer NOTIFY changed)
    Q_PROPERTY(bool live READ live NOTIFY changed)
    Q_PROPERTY(bool inUse READ inUse NOTIFY changed)
public:
    explicit VirtualCamera(QObject* parent = nullptr);
    ~VirtualCamera() override;

    bool    supported() const { return supported_; }
    bool    installed() const { return installed_; }
    bool    canInstall() const;
    bool    installing() const { return installing_; }
    QString hint() const { return hint_; }
    QString peer() const { return peer_; }
    bool    live() const { return live_; }
    bool    inUse() const { return inUse_; }

    // Starts showing `peer`'s camera (stopping any other), turned by
    // `rotation` degrees clockwise as its viewer shows it.
    Q_INVOKABLE void start(const QString& peer, int rotation = 0);
    Q_INVOKABLE void stop();
    Q_INVOKABLE void setRotation(int rotation);
    // Installs the system component (Windows: asks for administrator rights).
    Q_INVOKABLE void install();

    // A decoded frame from a camera being watched.
    void push(const QString& peer, const QVideoFrame& frame);

signals:
    void changed();
    void notify(const QString& text);

private:
    void refreshSupport();
    void run();
    void setState(bool live, bool inUse);

    bool    supported_ = false, installed_ = false, installing_ = false;
    QString hint_;
    QString peer_;
    bool    live_ = false, inUse_ = false;

    // Shared with the worker.
    std::mutex              mutex_;
    std::condition_variable wake_;
    bool                    quit_ = false, wantRunning_ = false;
    int                     rotation_ = 0;
    QVideoFrame             frame_;
    bool                    hasFrame_ = false;
    std::thread             worker_;
};

}  // namespace ucd
