#pragma once

#include <QObject>
#include <QThread>
#include <QTimer>
#include <QVideoFrame>
#include <QVariantMap>
#include <atomic>
#include "Codec.hpp"
#include "DesktopEncoder.hpp"
#include "Input.hpp"
#include "Cursor.hpp"
#include "core/Permissions.hpp"
#include "uconnect/uconnect.hpp"

namespace ucd::screen {

struct PeerAccess {
    QString key, name, fingerprint;
    PermissionSet permissions;
    bool trusted = false;
};
struct ScreenSettings {
    bool offered = false;
    bool gpuCapture = true;
    bool cursorEmbedded = true;
    bool controlOffered = false;
    int monitor = 0, maxFps = 30, maxWidth = 1920, maxBitrate = 8000000, maxViewers = 1;
    QString ciscoLibrary;
    QVariantList monitors;
};

// Session-worker state machine. Capture and UI use signals; codec work has
// dedicated threads with bounded submissions. The transport only moves bytes.
class ScreenService : public QObject {
    Q_OBJECT
public:
    using Peer = uconnect::DevId;
    using Send = std::function<bool(const Peer&, const QJsonObject&)>;
    using Authorize = std::function<PeerAccess(const Peer&)>;
    using Open = std::function<bool(const Peer&)>;
    using Close = std::function<void(const Peer&)>;
    using Datagram = std::function<bool(const Peer&, const QByteArray&)>;
    using Now = std::function<qint64()>;
    ScreenService(Send send, Authorize authorize, Open open, Close close, Datagram datagram,
        QObject* parent = nullptr, Now now = {}, std::unique_ptr<InputBackend> input = {}, bool camera = false);
    ~ScreenService() override;
    void configure(const ScreenSettings& settings);
    void setOffered(bool offered);
    void setControlOffered(bool offered);
    void requestControl(const Peer& peer, bool on);
    void stopControl();
    void sendInput(const Peer& peer, QJsonObject message);
    void submitCursor(const QPoint& position, bool visible, const QByteArray& shape, const QPoint& hotspot);
    void request(const Peer& peer, bool control = false, int maxWidth = 1920, int fps = 30);
    void consent(const QString& peerId, quint32 request, bool allow, bool remember);
    void stop(const Peer& peer, const QString& reason = "ended");
    void stopHosting(const QString& reason = "stopped by host");
    void peerGone(const Peer& peer);
    void reset();
    void policyChanged();
    bool handle(const Peer& peer, const QJsonObject& message);
    void receive(const Peer& peer, const QByteArray& packet);
    void pathChanged(const Peer& peer, const QString& path);
    void submitCapture(const QVideoFrame& frame);  // only thread-safe entry point
    void submitImage(const QImage& image);  // synthetic source for tests
    void tick();
    void monitor(const Peer& peer, int index);
    static qint64 monotonicMs();
signals:
    void consentRequested(const QString& peer, quint32 request, const QVariantMap& device);
    void consentClosed(const QString& peer, quint32 request);
    void rememberPermission(const QString& key, const QString& permission);
    void viewerChanged(const QString& peer, const QVariantMap& state);
    void imageReady(const QString& peer, const QImage& image);
    void hostingChanged(const QVariantList& viewers);
    void captureChanged(bool active, int monitor);
    void activity(const QString& line);
    void nativeCaptureChanged(bool active);
private:
    struct Host {
        quint32 request = 0;
        quint16 token = 0;
        qint64 expires = 0, lastReport = 0;
        int width = 1920, fps = 30, monitor = 0;
        bool live = false, wantControl = false, forceKey = true;
        bool control = false;
        quint32 controlRequest = 0;
        qint64 controlExpires = 0;
        quint32 cursorVersion = 0;
        std::optional<quint32> nackedFrame;
        QString path = "opening";
        PacedSender sender;
        CongestionController congestion;
    };
    struct Viewer {
        quint32 request = 0;
        quint16 token = 0;
        qint64 expires = 0, reportAt = 0;
        int width = 1920, fps = 30;
        QString state = "requesting", path = "opening";
        Reassembler receiver;
        quint64 bytes = 0;
        int decoded = 0;
        double decodeMs = 0;
        bool decoding = false, channel = false;
        bool wantControl = false;
        bool control = false;
        quint32 controlRequest = 0;
        quint32 cursorSequence = 0;
        bool haveCursor = false;
    };
    void startHost(const Peer& peer);
    bool allowed(const Peer& peer) const;
    bool controlAllowed(const Peer& peer) const;
    void setControl(const Peer& peer, bool on, quint32 request = 0, const QString& detail = {});
    void requestHostControl(const Peer& peer, quint32 request);
    void fail(const Peer& peer, quint32 request, const QString& code);
    void showViewer(const Peer& peer, const QString& state, const QString& detail = {}, const QVariantMap& extra = {});
    void showHosting();
    void encodeCapture(const QVideoFrame& frame, const QImage& image, quint32 captureMs);
    Send send_;
    Authorize authorize_;
    Open open_;
    Close close_;
    Datagram datagram_;
    Now now_;
    ScreenSettings settings_;
    bool camera_ = false;
    std::map<Peer, Host> hosts_;
    std::map<Peer, Viewer> viewers_;
    quint32 nextRequest_ = 1, nextFrame_ = 0;
    quint64 generation_ = 0;
    qint64 lastCapture_ = -1000;
    qint64 lastReleaseRetry_ = -1000;
    QTimer timer_;
    QThread encoderThread_, decoderThread_;
    QObject* encoderWorker_;
    QObject* decoderWorker_;
    std::unique_ptr<Encoder> encoder_;
    std::unique_ptr<DesktopEncoder> desktop_;
    bool nativeActive_ = false, nativeDisabled_ = false;
    QString lastPipeline_;
    std::map<Peer, std::unique_ptr<Decoder>> decoders_;
    std::atomic_bool capturePending_{false};
    InputState input_;
    QByteArray cursorShape_;
    QPoint cursorHotspot_;
    quint32 cursorVersion_ = 0, cursorSequence_ = 0;
};

}  // namespace ucd::screen
