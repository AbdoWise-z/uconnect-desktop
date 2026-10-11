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
    bool trusted = false;   // this device trusts it: needed to share with it
    bool verified = false;  // its identity is proven: enough to watch what it shares
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

// What a service instance streams. Each has its own message prefix and
// datagram kinds: screen scr.* (1 video, 2 cursor), camera cam.* (3), and
// extended display dsp.* (4 video, 5 cursor).
enum class Kind { Screen, Camera, Display };

// Session-worker state machine. Capture and UI use signals; codec work has
// dedicated threads with bounded submissions. The transport only moves bytes.
//
// An extended display reuses the screen session with the handshake reversed:
// DisplayService agrees on the session first, then starts the sending side
// with startDisplayHost() (its source is the virtual monitor, configured as
// the only monitor) and the showing side with expectDisplay().
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
        QObject* parent = nullptr, Now now = {}, std::unique_ptr<InputBackend> input = {}, Kind kind = Kind::Screen);
    Kind kind() const { return kind_; }
    // Extended display, sending side: the session `request` was agreed and
    // the virtual monitor is settings.monitors[0]. Streams it at `width`
    // (its pixel width) and `fps`.
    bool startDisplayHost(const Peer& peer, quint32 request, int width, int fps);
    // Extended display, showing side: wait for the sender's started message
    // for `request`.
    void expectDisplay(const Peer& peer, quint32 request, int maxWidth, int fps);
    // Extended displays start at this bitrate on a direct path.
    static constexpr int DisplayStartBitrate = 20000000;
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
    // Ends both roles with `peer` (a disconnect); the others end one: this
    // device sharing with it, or this device watching it.
    void stop(const Peer& peer, const QString& reason = "ended");
    void stopHost(const Peer& peer, const QString& reason = "ended");
    void stopViewer(const Peer& peer, const QString& reason = "ended");
    void stopHosting(const QString& reason = "stopped by host");
    // Qt's screen capture failed. That ends hosting only while Qt's capture
    // is the one in use (see qtCaptureWanted).
    void captureFailed(const QString& error);
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
    // A decoded picture, in its YUV layout: the video sink converts it on the GPU.
    void frameReady(const QString& peer, const QVideoFrame& frame);
    void hostingChanged(const QVariantList& viewers);
    // A sending session (live or still asking) ended, for whatever reason.
    void hostEnded(const QString& peer, const QString& reason);
    void captureChanged(bool active, int monitor);
    void activity(const QString& line);
    // Whether Qt's capture should run. The native path (desktop duplication)
    // is preferred and the two never run together: they compete for the
    // desktop, and whichever starts second fails.
    void qtCaptureWanted(bool wanted);
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
        qint64 startedAt = 0;
        QString controlRefused;  // control was asked for but cannot be granted: why
        quint64 framesSent = 0;
        bool stalledTold = false;
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
        QVector<EncodedFrame> toDecode;  // complete frames waiting for the decoder
        qint64 liveAt = 0;
        bool pictured = false, stalledTold = false;
        bool wantControl = false;
        bool control = false;
        quint32 controlRequest = 0;
        quint32 cursorSequence = 0;
        bool haveCursor = false;
    };
    void startHost(const Peer& peer);
    void endRoles(const Peer& peer, const QString& reason, bool asHost, bool asViewer);
    bool allowed(const Peer& peer) const;
    // May keep showing what `peer` sends: a proven identity, and for an
    // extended display also trust and the display permission.
    bool viewerAllowed(const Peer& peer) const;
    void failMedia(const Peer& peer, const QString& detail);
    bool controlAllowed(const Peer& peer) const;
    void setControl(const Peer& peer, bool on, quint32 request = 0, const QString& detail = {});
    void requestHostControl(const Peer& peer, quint32 request);
    void fail(const Peer& peer, quint32 request, const QString& code);
    void showViewer(const Peer& peer, const QString& state, const QString& detail = {}, const QVariantMap& extra = {});
    void showHosting();
    void encodeCapture(const QVideoFrame& frame, const QImage& image, quint32 captureMs);
    void decodeNext(const Peer& peer);
    Send send_;
    Authorize authorize_;
    Open open_;
    Close close_;
    Datagram datagram_;
    Now now_;
    ScreenSettings settings_;
    Kind kind_ = Kind::Screen;
    bool camera_ = false, display_ = false;
    std::map<Peer, Host> hosts_;
    std::map<Peer, Viewer> viewers_;
    quint32 nextRequest_ = 1, nextFrame_ = 0;
    quint64 generation_ = 0;
    // Decoding has its own: an encoder restart (one session's capture
    // changing) must not orphan another session's decodes in flight.
    quint64 decoderGeneration_ = 0;
    double nextCapture_ = 0;  // when the next frame is due, in fractional ms
    // Diagnostics: per-second counters, logged when UCD_SCREEN_STATS is set.
    struct Stats {
        std::atomic_int offered{0}, busy{0};  // Qt capture frames (its own thread), and those skipped while encoding
        int throttled = 0, encodes = 0, frames = 0, queued = 0, refused = 0;
        int completed = 0, decodes = 0, shown = 0;
        double encodeMs = 0, decodeMs = 0;
        qint64 at = 0;
    } stats_;
    const bool statsOn_ = qEnvironmentVariableIsSet("UCD_SCREEN_STATS");
    void logStats(qint64 now);
    qint64 lastReleaseRetry_ = -1000;
    QTimer timer_;
    QThread encoderThread_, decoderThread_;
    QObject* encoderWorker_;
    QObject* decoderWorker_;
    std::unique_ptr<Encoder> encoder_;
    std::unique_ptr<DesktopEncoder> desktop_;
    bool nativeActive_ = false, nativeDisabled_ = false;
    // A failed native path is tried again later (desktop duplication is lost
    // on display changes, UAC prompts, the lock screen), backing off each time.
    qint64 nativeRetryAt_ = 0;
    int nativeFailures_ = 0;
    qint64 nativeAttemptAt_ = 0;  // a retry waits for Qt's capture to let go
    bool nativeWorked_ = false;   // in this sharing session; only then is it retried
    bool qtWanted_ = false;
    void resetNative();
    void updateQtCapture();
    QString lastPipeline_;
    std::map<Peer, std::unique_ptr<Decoder>> decoders_;
    std::atomic_bool capturePending_{false};
    InputState input_;
    QByteArray cursorShape_;
    QPoint cursorHotspot_;
    quint32 cursorVersion_ = 0, cursorSequence_ = 0;
};

}  // namespace ucd::screen
