#include "TrayIcon.hpp"

#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QPointer>
#include <QSettings>
#include <QSystemTrayIcon>

#include "AppController.hpp"
#include "DockReopen.hpp"
#include "DisplayController.hpp"
#include "ScreenController.hpp"

namespace ucd {
namespace {

constexpr auto HintShown = "tray/hintShown";

QString statusText(const QString& status, const QString& topicId) {
    if (status == QLatin1String("online"))
        return topicId.isEmpty() ? TrayIcon::tr("Connected")
                                 : TrayIcon::tr("Connected to topic %1").arg(topicId.left(8));
    if (status == QLatin1String("connecting")) return TrayIcon::tr("Connecting...");
    if (status == QLatin1String("reconnecting")) return TrayIcon::tr("Reconnecting...");
    if (status == QLatin1String("error")) return TrayIcon::tr("Could not connect");
    return TrayIcon::tr("Not connected");
}

}  // namespace

TrayIcon::TrayIcon(AppController* app, ScreenController* screen, ScreenController* camera, DisplayController* display)
    : QObject(app), app_(app), screen_(screen), camera_(camera), display_(display) {
    // The menu is a widget, so it needs a QApplication (tests run without).
    available_ = qobject_cast<QApplication*>(QCoreApplication::instance()) && QSystemTrayIcon::isSystemTrayAvailable();
#ifdef Q_OS_MACOS
    if (qobject_cast<QApplication*>(QCoreApplication::instance()))
        onDockReopen([self = QPointer<TrayIcon>(this)] { if (self) emit self->reopened(); });
#endif
    if (!available_) return;

    menu_ = new QMenu;
    auto* open = menu_->addAction(tr("Open uConnect"), this, &TrayIcon::showRequested);
    QFont bold = open->font();
    bold.setBold(true);
    open->setFont(bold);
    menu_->addSeparator();
    status_ = menu_->addAction(QString());
    status_->setEnabled(false);
    sharing_ = menu_->addAction(QString());
    sharing_->setEnabled(false);
    connect_ = menu_->addAction(QString(), this, [this] {
        if (app_->active()) app_->disconnectFromTopic();
        else app_->connectToTopic();
    });
    copyLink_ = menu_->addAction(tr("Copy Invite Link"), app_, &AppController::copyTopicLink);
    stopSharing_ = menu_->addAction(tr("Stop Sharing"), this, [this] {
        screen_->stopHosting();
        camera_->stopHosting();
        display_->stopAll();
    });
    menu_->addSeparator();
    settings_ = menu_->addAction(tr("Settings..."), this, &TrayIcon::settingsRequested);
    menu_->addSeparator();
    menu_->addAction(tr("Exit uConnect"), this, &TrayIcon::exitRequested);

    icon_ = new QSystemTrayIcon(QGuiApplication::windowIcon(), this);
    icon_->setContextMenu(menu_);
    connect(icon_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
#ifdef Q_OS_MACOS
        // A click there opens the menu, which is reported as a trigger too.
        if (reason == QSystemTrayIcon::DoubleClick) emit showRequested();
#else
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) emit showRequested();
#endif
    });

    connect(app_, &AppController::statusChanged, this, &TrayIcon::update);
    connect(app_, &AppController::snapshotChanged, this, &TrayIcon::update);
    connect(app_, &AppController::topicLinkChanged, this, &TrayIcon::update);
    connect(app_, &AppController::firstRunChanged, this, &TrayIcon::update);
    connect(screen_, &ScreenController::hostingChanged, this, &TrayIcon::update);
    connect(camera_, &ScreenController::hostingChanged, this, &TrayIcon::update);
    connect(display_, &DisplayController::hostingChanged, this, &TrayIcon::update);
    update();
    icon_->show();
}

TrayIcon::~TrayIcon() {
    delete icon_;  // before its menu
    delete menu_;
}

void TrayIcon::windowHidden() {
    QSettings settings;
    if (!icon_ || settings.value(HintShown, false).toBool()) return;
    settings.setValue(HintShown, true);
    icon_->showMessage(tr("uConnect is still running"),
                       tr("It stays connected in the tray. To quit, choose Exit uConnect from the tray menu."),
                       QGuiApplication::windowIcon(), 5000);
}

void TrayIcon::update() {
    const QString status = statusText(app_->status(), app_->topicId());

    QStringList sharing;
    auto count = [](qsizetype n) { return n == 1 ? tr("1 device") : tr("%1 devices").arg(n); };
    if (const auto n = screen_->hosting().size()) sharing << tr("Sharing your screen with %1").arg(count(n));
    if (const auto n = camera_->hosting().size()) sharing << tr("Sharing your camera with %1").arg(count(n));
    if (const auto n = display_->hosting().size()) sharing << tr("Extending your desktop to %1").arg(count(n));
    const QString shared = sharing.join(QStringLiteral("; "));

    status_->setText(status);
    sharing_->setText(shared);
    sharing_->setVisible(!shared.isEmpty());
    stopSharing_->setVisible(!shared.isEmpty());

    const bool active = app_->active();
    connect_->setText(active ? tr("Disconnect") : tr("Connect"));
    connect_->setEnabled(active || (!app_->firstRun() && app_->topicLinkValid()));
    copyLink_->setEnabled(app_->status() == QLatin1String("online"));
    settings_->setEnabled(!app_->firstRun());

    icon_->setToolTip(shared.isEmpty() ? tr("uConnect - %1").arg(status)
                                       : tr("uConnect - %1\n%2").arg(status, shared));
}

}  // namespace ucd
