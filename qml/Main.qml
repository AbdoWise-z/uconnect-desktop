import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import UConnect

ApplicationWindow {
    id: root

    width: 1024
    height: 680
    minimumWidth: 900
    minimumHeight: 560
    visible: !App.startsHidden  // --minimized: opened later, from the tray
    color: Theme.canvas
    font.pixelSize: Theme.fontSize
    title: inWorkspace && App.topicId.length > 0
           ? qsTr("Topic %1 – uConnect").arg(App.topicId.substring(0, 8))
           : "uConnect"

    palette.window: Theme.panel
    palette.windowText: Theme.text
    palette.base: Theme.input
    palette.text: Theme.text
    palette.button: Theme.panel
    palette.buttonText: Theme.text
    palette.highlight: Theme.selection
    palette.highlightedText: Theme.text
    palette.toolTipBase: Theme.popup
    palette.toolTipText: Theme.text
    palette.placeholderText: Theme.textDisabled
    palette.mid: Theme.border
    palette.dark: Theme.popupBorder
    palette.light: Theme.hover
    palette.midlight: Theme.hover

    // first run -> not connected -> connected (workspace)
    readonly property string page: App.firstRun ? "firstRun"
                                 : (App.status === "online" || App.status === "reconnecting") ? "workspace"
                                 : "connect"
    readonly property bool inWorkspace: page === "workspace"

    // Closing this window leaves the app in the tray, still connected and
    // sharing; Exit (here or in the tray) ends it. Without a tray, or with
    // that setting off, closing exits, which also closes the other windows.
    onClosing: {
        if (App.tray.available && App.closeToTray) App.tray.windowHidden()
        else root.quitApp()
    }
    function restore() {
        if (root.visibility === Window.Minimized) root.showNormal()
        else root.show()
        root.raise()
        root.requestActivate()
    }
    Connections {
        target: App.tray
        function onShowRequested() { root.restore() }
        function onSettingsRequested() { root.restore(); root.openSettings("") }
        function onExitRequested() { root.quitApp() }
        function onReopened() { if (!root.visible) root.restore() }
    }
    // Qt.quit() first asks every visible window to close and gives up if one
    // refuses; the sharing bar refuses (it stops sharing instead), which left
    // the app running with no window. Stop sharing, then leave regardless.
    function quitApp() {
        App.screen.stopHosting()
        App.camera.stopHosting()
        App.display.stopAll()
        Qt.exit(0)
    }
    Loader { source: App.screen.available ? Qt.resolvedUrl("ScreenSafety.qml") : "" }
    Loader { source: App.display.available ? Qt.resolvedUrl("DisplaySafety.qml") : "" }
    Loader {
        active: App.camera.available
        Component.onCompleted: if (active) setSource(Qt.resolvedUrl("ScreenSafety.qml"), {controller: App.camera, camera: true})
    }

    function openSettings(pageId) {
        settingsWindow.openAt(pageId)
    }

    // --- service windows: one per (service, device) -----------------------
    property var serviceWindows: ({})

    function openService(service, devId) {
        if (service === "sync") return syncWindow.show()  // one window for every synced folder
        const key = (service === "control" ? "screen" : service) + ":" + devId
        let w = serviceWindows[key]
        if (w && service === "control" && App.screen.available) App.screen.requestControl(devId, true)
        if (!w) {
            // No parent: each service gets an independent top-level window.
            w = service === "display"
                ? Qt.createComponent("DisplaySetup.qml").createObject(null, { devId: devId })
                : serviceWindowComponent.createObject(null, { service: service, devId: devId })
            w.closing.connect(() => {
                delete root.serviceWindows[key]
                Qt.callLater(w.destroy)
            })
            serviceWindows[key] = w
        }
        w.show()
        w.raise()
        w.requestActivate()
    }

    function closeServiceWindows() {
        for (const key of Object.keys(serviceWindows))
            serviceWindows[key].close()
    }

    Connections {
        target: App
        function onStatusChanged() {
            if (!App.active) root.closeServiceWindows()
        }
    }

    Component {
        id: serviceWindowComponent
        ServiceWindow {}
    }

    // --- actions: the one definition of each command and its shortcut ----
    Action {
        id: settingsAction
        text: qsTr("Settings...")
        shortcut: "Ctrl+Alt+S"
        enabled: !App.firstRun
        onTriggered: root.openSettings("")
    }
    Action {
        id: quitAction
        text: qsTr("Exit")
        shortcut: "Ctrl+Q"
        onTriggered: root.quitApp()
    }
    Action {
        id: copyLinkAction
        text: qsTr("Copy Invite Link")
        enabled: root.inWorkspace
        onTriggered: App.copyTopicLink()
    }
    Action {
        id: copyIdAction
        text: qsTr("Copy Topic ID")
        enabled: root.inWorkspace
        onTriggered: App.copyTopicId()
    }
    Action {
        id: refreshAction
        text: qsTr("Refresh Devices")
        shortcut: "F5"
        enabled: root.inWorkspace
        onTriggered: App.refreshPeers()
    }
    Action {
        id: disconnectAction
        text: qsTr("Disconnect")
        enabled: App.active
        onTriggered: App.disconnectFromTopic()
    }
    Action {
        id: devicesPanelAction
        text: qsTr("Devices")
        shortcut: "Alt+1"
        checkable: true
        checked: true
        enabled: root.inWorkspace
    }
    Action {
        id: statusBarAction
        text: qsTr("Status Bar")
        checkable: true
        checked: true
    }
    Action {
        id: syncAction
        text: qsTr("Synced Folders")
        shortcut: "Ctrl+Shift+Y"
        onTriggered: syncWindow.show()
    }
    Action {
        id: storageActivityAction
        text: qsTr("Storage Activity")
        onTriggered: {
            activityWindow.show()
            activityWindow.raise()
            activityWindow.requestActivate()
        }
    }
    Action {
        id: debugAction
        text: qsTr("Debug Window")
        shortcut: "Ctrl+Shift+D"
        enabled: App.debugAvailable
        onTriggered: App.showDebugWindow()
    }
    Action {
        id: aboutAction
        text: qsTr("About uConnect")
        onTriggered: aboutDialog.open()
    }

    menuBar: UMenuBar {
        visible: root.inWorkspace

        UMenu {
            title: qsTr("File")
            UMenuItem { action: settingsAction }
            UMenuSeparator {}
            UMenuItem { action: quitAction }
        }
        UMenu {
            title: qsTr("Topic")
            UMenuItem {
                action: copyLinkAction
                hint: App.topicId.substring(0, 8)
            }
            UMenuItem { action: copyIdAction }
            UMenuSeparator {}
            UMenuItem { action: refreshAction }
            UMenuSeparator {}
            UMenuItem { action: disconnectAction }
        }
        UMenu {
            title: qsTr("Services")
            ServiceMenu {
                service: "storage"
                onOpenRequested: devId => root.openService(service, devId)
            }
            ServiceMenu {
                service: "camera"
                onOpenRequested: devId => root.openService(service, devId)
            }
            ServiceMenu {
                service: "screen"
                onOpenRequested: devId => root.openService(service, devId)
            }
            ServiceMenu {
                service: "control"
                onOpenRequested: devId => root.openService(service, devId)
            }
            ServiceMenu {
                service: "display"
                enabled: App.display.available
                onOpenRequested: devId => root.openService(service, devId)
            }
            UMenuSeparator {}
            UMenuItem { action: syncAction }
            UMenuSeparator {}
            UMenuItem {
                text: qsTr("Configure Services...")
                onTriggered: root.openSettings("services")
            }
        }
        UMenu {
            title: qsTr("View")
            UMenuItem { action: devicesPanelAction }
            UMenuItem { action: statusBarAction }
            UMenuItem { action: storageActivityAction }
            UMenuSeparator {}
            UMenuItem { action: debugAction }
        }
        UMenu {
            title: qsTr("Help")
            UMenuItem { action: aboutAction }
        }
    }

    StackLayout {
        anchors.fill: parent
        currentIndex: root.page === "firstRun" ? 0 : root.page === "connect" ? 1 : 2

        FirstRunPage {}

        ConnectPage {
            onSettingsRequested: pageId => root.openSettings(pageId)
        }

        WorkspacePage {
            devicesPanelVisible: devicesPanelAction.checked
            onServiceRequested: (service, devId) => root.openService(service, devId)
            onTrustRequested: devId => trustDialog.openFor(devId)
            onSettingsRequested: pageId => root.openSettings(pageId)
        }
    }

    footer: StatusBar {
        visible: root.page !== "firstRun" && statusBarAction.checked
        inWorkspace: root.inWorkspace
    }

    SettingsWindow {
        id: settingsWindow
    }

    TrustDialog {
        id: trustDialog
    }

    StorageActivityWindow {
        id: activityWindow
    }

    SyncWindow {
        id: syncWindow
    }

    Popup {
        id: aboutDialog

        anchors.centerIn: Overlay.overlay
        modal: true
        padding: 24
        width: 380

        background: Rectangle {
            color: Theme.panel
            border.color: Theme.popupBorder
            radius: 8
        }

        ColumnLayout {
            width: parent.width
            spacing: 10

            RowLayout {
                spacing: 12
                Avatar {
                    name: "u Connect"
                    seed: "uConnect"
                    size: 40
                }
                ColumnLayout {
                    spacing: 2
                    Text {
                        text: "uConnect Desktop"
                        color: Theme.text
                        font.pixelSize: 16
                        font.bold: true
                    }
                    Text {
                        text: qsTr("Version %1").arg(Qt.application.version)
                        color: Theme.textSecondary
                    }
                }
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.textSecondary
                text: qsTr("Connects your devices over protected uConnect topics. Built with Qt and the uConnect library.")
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.textSecondary
                font.pixelSize: Theme.smallFontSize
                text: qsTr("Screen builds dynamically link FFmpeg under the LGPL. License and source details are included in third-party-screen.md and FFmpeg-LICENSE.txt beside the app.")
            }
            UButton {
                Layout.alignment: Qt.AlignRight
                text: qsTr("Close")
                primary: true
                onClicked: aboutDialog.close()
            }
        }
    }
}
