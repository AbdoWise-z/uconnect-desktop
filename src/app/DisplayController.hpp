#pragma once
// Extended displays for QML: this device gaining a monitor (the host), and
// this device showing another's (the display device). The services live on
// the session thread; this object is the GUI thread's side of them.

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QVariantMap>
#include "core/Session.hpp"
#ifdef UCD_HAS_SCREEN
#include <QVideoSink>
#endif

namespace ucd {
class DisplayController : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)
    // state: ready, notInstalled, outdated, notSupported; detail; needsDriver
    // (Windows); bundled (a driver package ships with the app); busy.
    Q_PROPERTY(QVariantMap driver READ driver NOTIFY driverChanged)
    Q_PROPERTY(QVariantMap settings READ settings NOTIFY settingsChanged)
    Q_PROPERTY(QVariantList localScreens READ localScreens NOTIFY screensChanged)
    Q_PROPERTY(QVariantList consents READ consents NOTIFY consentChanged)
    // Hosted sessions that have not ended: devId, name, state, detail, width, height, fps.
    Q_PROPERTY(QVariantList hosting READ hosting NOTIFY hostingChanged)
public:
    explicit DisplayController(Session* session, QObject* parent = nullptr);
    ~DisplayController() override;
    bool available() const;
    QVariantMap driver() const { return driver_; }
    QVariantMap settings() const { return settings_; }
    QVariantList localScreens() const { return screens_; }
    QVariantList consents() const { return consents_; }
    QVariantList hosting() const;

    Q_INVOKABLE QVariantMap hostState(const QString& devId) const { return hostStates_.value(devId); }
    Q_INVOKABLE QVariantMap showState(const QString& devId) const { return showStates_.value(devId); }
    // `mode`: {width, height, fps}, or empty to match the other screen.
    Q_INVOKABLE void offer(const QString& devId, const QVariantMap& mode);
    Q_INVOKABLE void stop(const QString& devId);
    Q_INVOKABLE void stopAll();
    Q_INVOKABLE void consent(const QString& devId, quint32 request, bool allow, bool remember, int screen);
    Q_INVOKABLE void attachSink(const QString& devId, QObject* sink);
    Q_INVOKABLE void configure(const QVariantMap& settings);
    Q_INVOKABLE void refreshDriver();
    Q_INVOKABLE void installDriver();
    Q_INVOKABLE void removeDriver();
    // The codec choices shared with screen sharing changed.
    void refreshCodec();

signals:
    void driverChanged();
    void settingsChanged();
    void screensChanged();
    void consentChanged();
    void hostingChanged();
    void hostStateChanged(const QString& devId);
    void showStateChanged(const QString& devId);
    // Open the full-screen view of `devId`'s monitor on local screen `screen`.
    void showRequested(const QString& devId, int screen);
    void rememberPermission(const QString& key, const QString& permission);
    void notify(const QString& text);

private:
    void updateScreens();
    void setKeepAwake(bool on);
    void changeDriver(const QStringList& arguments, const QString& done);
    Session* session_;
    QVariantMap driver_, settings_;
    QVariantList screens_, consents_;
    QHash<QString, QVariantMap> hostStates_, showStates_;
    bool awake_ = false;
#ifdef UCD_HAS_SCREEN
    QHash<QString, QPointer<QVideoSink>> sinks_;
    QTimer cursorTimer_;
    quintptr cursorShape_ = 0;
#endif
#ifdef Q_OS_MACOS
    unsigned int awakeAssertion_ = 0;
#endif
};
}  // namespace ucd
