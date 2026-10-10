#pragma once
// Extended display sessions: a host asks a device it trusts to act as one of
// its monitors. This service runs the handshake (dsp.request / wait / err /
// accept) and owns the virtual monitor; the video itself is a screen session
// in the display ScreenService (dsp.* messages, datagram kinds 4 and 5).
//
// The rule it exists to keep: a virtual monitor never outlives its session.
// Every way a session ends goes through end(), which removes the monitor
// before anything else.

#include <QObject>
#include <QThread>
#include <QTimer>
#include <QVariantList>
#include <functional>
#include <map>
#include <memory>

#include "VirtualDisplay.hpp"
#include "core/screen/ScreenService.hpp"

namespace ucd::display {

// What the host asks for: a fixed mode, or (width 0) the display device's
// own screen.
struct Request {
    int width = 0, height = 0, fps = 60;
    int maxBitrate = 40000000;
};

// The display device's screen chosen to show the session.
struct LocalScreen {
    int width = 1920, height = 1080, fps = 60;
    double dpi = 96;
};

// The mode to create: the request (or the screen it is shown on), within
// what this host can encode and the display device can decode.
Mode resolveMode(const Request& request, const LocalScreen& screen, int maxWidth, int maxFps, bool hiDpiCapable);

class DisplayService : public QObject {
    Q_OBJECT
public:
    using Peer = uconnect::DevId;
    using Send = std::function<bool(const Peer&, const QJsonObject&)>;
    using Authorize = screen::ScreenService::Authorize;
    using Factory = std::function<std::unique_ptr<VirtualDisplay>()>;
    using StatusCheck = std::function<Status()>;
    using Now = std::function<qint64()>;

    DisplayService(Send send, Authorize authorize, screen::ScreenService* media, Factory factory,
        StatusCheck availability, QObject* parent = nullptr, Now now = {});
    ~DisplayService() override;

    // --- the host: this device gains a monitor --------------------------------
    void offer(const Peer& peer, const Request& request);
    void stop(const Peer& peer, const QString& reason = QStringLiteral("stopped"));

    // --- the display device: this device shows another's monitor --------------
    // Whether this device may be used as a display at all (hello "display").
    void setOffered(bool offered);
    // This device's screens, for the consent prompt: name, width, height
    // (pixels), fps, dpi.
    void setLocalScreens(const QVariantList& screens);
    void consent(const QString& peerId, quint32 request, bool allow, bool remember, int screen);

    bool handle(const Peer& peer, const QJsonObject& message);
    void peerGone(const Peer& peer);
    void policyChanged();
    void reset();

    // Where monitors come from (tests replace the platform's).
    void setBackend(Factory factory, StatusCheck availability) { factory_ = std::move(factory); availability_ = std::move(availability); }
    // Native capture of the monitor (off in tests, which feed synthetic frames).
    void setNativeCapture(bool native) { nativeCapture_ = native; }
    // The codec choices shared with screen sharing (the software codec library).
    void setCodecSettings(const screen::ScreenSettings& settings) { codec_ = settings; }
    // How many virtual monitors exist now.
    int monitorCount() const;

signals:
    // Host: offering, waiting, creating, live, ended (with detail), plus the mode.
    void hostStateChanged(const QString& peer, const QVariantMap& state);
    // Display device: a host asks to use a screen of this device.
    void consentRequested(const QString& peer, quint32 request, const QVariantMap& info);
    void consentClosed(const QString& peer, quint32 request);
    // Display device: show the session from `peer` full screen on `screen`.
    void showRequested(const QString& peer, int screen);
    void rememberPermission(const QString& key, const QString& permission);
    void activity(const QString& line);

private:
    struct Hosted {  // this device's side of a session it asked for
        quint32 request = 0;
        Request asked;
        Mode mode;
        qint64 expires = 0;
        QString state = "offering";
        std::shared_ptr<VirtualDisplay> monitor;
        quint64 generation = 0;  // which creation a finished worker belongs to
        bool live = false;
    };
    struct Shown {  // a session this device shows for another
        quint32 request = 0;
        qint64 expires = 0;
        bool accepted = false;
    };
    void tick();
    void end(const Peer& peer, const QString& state, const QString& detail, bool tellPeer);
    void accept(const Peer& peer, quint32 request, int screen);
    void accepted(const Peer& peer, quint32 request, const LocalScreen& screen, int maxWidth, int maxFps);
    void created(const Peer& peer, quint64 generation, std::shared_ptr<VirtualDisplay> monitor, const QString& error);
    void showHost(const Peer& peer, const QVariantMap& extra = {});
    void refuse(const Peer& peer, quint32 request, const QString& code, const QString& detail = {});
    LocalScreen localScreen(int index) const;

    Send send_;
    Authorize authorize_;
    screen::ScreenService* media_;
    Factory factory_;
    StatusCheck availability_;
    Now now_;
    bool offered_ = false;
    QVariantList screens_;
    std::map<Peer, Hosted> hosted_;
    std::map<Peer, Shown> shown_;
    quint32 nextRequest_ = 1;
    quint64 nextGeneration_ = 1;
    bool nativeCapture_ = true;
    screen::ScreenSettings codec_;
    QVector<QThread*> workers_;  // creating monitors; waited for on destruction
    QTimer timer_;
};

}  // namespace ucd::display
