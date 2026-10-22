#pragma once
// The icon in the system tray (the menu bar on macOS), which keeps the app
// reachable while its window is closed: what it is doing, and a menu to open
// it, connect, stop sharing or exit. The window itself is QML's, so the menu
// asks for it with signals.

#include <QObject>

class QAction;
class QMenu;
class QSystemTrayIcon;

namespace ucd {

class AppController;
class DisplayController;
class ScreenController;

class TrayIcon : public QObject {
    Q_OBJECT
    // A tray exists here (some Linux desktops have none).
    Q_PROPERTY(bool available READ available CONSTANT)

public:
    TrayIcon(AppController* app, ScreenController* screen, ScreenController* camera, DisplayController* display);
    ~TrayIcon() override;

    bool available() const { return available_; }
    // Called when the window closes into the tray: says so, the first time.
    Q_INVOKABLE void windowHidden();

signals:
    void showRequested();
    void settingsRequested();
    void exitRequested();
    // macOS: the Dock icon was clicked, which should bring a closed window back.
    void reopened();

private:
    void update();

    AppController*     app_;
    ScreenController*  screen_;
    ScreenController*  camera_;
    DisplayController* display_;
    bool               available_ = false;
    bool               sharingActive_ = false;  // badge the tray icon while sharing
    QSystemTrayIcon*   icon_ = nullptr;
    QMenu*             menu_ = nullptr;
    QAction*           status_ = nullptr;
    QAction*           sharing_ = nullptr;
    QAction*           connect_ = nullptr;
    QAction*           copyLink_ = nullptr;
    QAction*           stopSharing_ = nullptr;
    QAction*           settings_ = nullptr;
};

}  // namespace ucd
