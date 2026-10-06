#include <QCommandLineParser>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QtQml>

#include "app/AppController.hpp"

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    QGuiApplication::setOrganizationName(QStringLiteral("uConnect"));
    QGuiApplication::setApplicationName(QStringLiteral("uConnect Desktop"));
    QGuiApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    QCommandLineParser cli;
    cli.setApplicationDescription(QStringLiteral("Connect your desktop devices over uConnect."));
    cli.addHelpOption();
    cli.addVersionOption();
    const QCommandLineOption serverOpt({"s", "server"}, "Rendezvous server (host:port).", "address");
    const QCommandLineOption topicOpt({"t", "topic"}, "Protected topic link (uconn://...).", "link");
    const QCommandLineOption nameOpt({"n", "name"}, "Name shown to other devices.", "name");
    const QCommandLineOption profileOpt(
        "profile", "Use a separate settings profile, e.g. to run two instances on one machine.", "name");
    const QCommandLineOption connectOpt("connect", "Connect immediately on start.");
    const QCommandLineOption debugOpt("debug", "Open the debug window on start.");
    const QCommandLineOption verboseOpt("verbose", "Verbose uconnect library logging.");
    cli.addOptions({serverOpt, topicOpt, nameOpt, profileOpt, connectOpt, debugOpt, verboseOpt});
    cli.process(app);

    if (cli.isSet(profileOpt)) {
        QGuiApplication::setApplicationName(
            QStringLiteral("uConnect Desktop (%1)").arg(cli.value(profileOpt)));
    }

    qRegisterMetaType<ucd::PeerRecord>();
    qRegisterMetaType<QVector<ucd::PeerRecord>>();
    qRegisterMetaType<ucd::NodeSnapshot>();

    ucd::LaunchOptions options;
    options.server      = cli.value(serverOpt);
    options.topicLink   = cli.value(topicOpt);
    options.deviceName  = cli.value(nameOpt);
    options.autoConnect = cli.isSet(connectOpt);
    options.showDebug   = cli.isSet(debugOpt);
    options.verbose     = cli.isSet(verboseOpt);

    // Basic is the style meant to be restyled; the look comes from qml/Theme.qml.
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    ucd::AppController controller(options);
    qmlRegisterSingletonInstance("UConnect", 1, 0, "App", &controller);

    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule("UConnectDesktop", "Main");

    return QGuiApplication::exec();
}
