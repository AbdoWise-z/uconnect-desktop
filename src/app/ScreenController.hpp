#pragma once
#include <QObject>
#include <QPointer>
#include <QVariantMap>
#include "core/Session.hpp"
#include "core/screen/StopShortcut.hpp"
#ifdef UCD_HAS_SCREEN
#include <QMediaCaptureSession>
#include <QScreenCapture>
#include <QCamera>
#include <QMediaDevices>
#include <QVideoSink>
#include "app/InputCapture.hpp"
#endif

class QScreen;

namespace ucd {
class ScreenController : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(QVariantMap settings READ settings NOTIFY settingsChanged)
    Q_PROPERTY(QVariantList hosting READ hosting NOTIFY hostingChanged)
    Q_PROPERTY(QVariantList consents READ consents NOTIFY consentChanged)
    Q_PROPERTY(QString shortcutError READ shortcutError NOTIFY hostingChanged)
public:
    ScreenController(Session* session, QObject* parent = nullptr, bool camera = false);
    ~ScreenController() override;
    bool available() const;
    QVariantMap settings() const { return settings_; }
    QVariantList hosting() const { return hosting_; }
    QVariantList consents() const { return consents_; }
    QString shortcutError() const { return shortcutError_; }
    void setShortcutError(const QString& error);
    Q_INVOKABLE QVariantMap state(const QString& peer) const { return states_.value(peer); }
    Q_INVOKABLE void configure(const QVariantMap& settings);
    // Re-read the OpenH264 codec library the user configured under Screen
    // Sharing, which the camera shares, so a change applies without a restart.
    void reloadSharedCodec();
    Q_INVOKABLE void request(const QString& peer, bool control = false);
    Q_INVOKABLE void stop(const QString& peer);
    Q_INVOKABLE void stopHosting();
    Q_INVOKABLE void consent(const QString& peer, quint32 request, bool allow, bool remember);
    Q_INVOKABLE void monitor(const QString& peer, int index);
    Q_INVOKABLE void attachSink(const QString& peer, QObject* sink);
    Q_INVOKABLE void requestControl(const QString& peer, bool on);
    Q_INVOKABLE void stopControl();
    Q_INVOKABLE void captureKeyboard(const QString& peer, QObject* window, bool capture);
    Q_INVOKABLE void mouse(const QString& peer, double x, double y, int buttons);
    Q_INVOKABLE void wheel(const QString& peer, int dx, int dy);
    Q_INVOKABLE void releaseInput(const QString& peer);
signals:
    void stateChanged(const QString& peer);
    void hostingChanged();
    void consentChanged();
    void settingsChanged();
    void rememberPermission(const QString& key, const QString& permission);
    void notify(const QString& text);
private:
    void pushSettings(const QScreen* removed = nullptr);
    Session* session_;
    bool camera_ = false;
    QVariantMap settings_;
    QVariantList hosting_, consents_;
    QHash<QString, QVariantMap> states_;
    QString shortcutError_;
#ifdef UCD_HAS_SCREEN
    QScreenCapture capture_;
    bool qtCaptureWanted_ = false;  // the service prefers native capture; Qt's runs only when it says so
    std::unique_ptr<QCamera> cameraCapture_;
    QMediaDevices devices_;
    screen::ScreenService* service() const { return camera_ ? session_->camera() : session_->screen(); }
    QMediaCaptureSession captureSession_;
    QVideoSink captureSink_;
    QHash<QString, QPointer<QVideoSink>> sinks_;
    QHash<QString, InputCapture*> captures_;
    QHash<QString, QJsonObject> moves_;
    QHash<QString, int> buttons_;
    QTimer moveTimer_;
    QTimer cursorTimer_;
    quint64 sourceGeneration_ = 0;
    void startCamera(int index, quint64 generation);
    quintptr cursorShape_ = 0;
    void input(const QString& peer, const QJsonObject& message);
#endif
};
}
