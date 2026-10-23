import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import UConnect

// A dedicated window for one service of one device. The controls are laid out
// for the real thing and disabled until the service is implemented.
Window {
    id: win

    required property string service
    required property string devId
    readonly property var media: service === "camera" ? App.camera : App.screen
    readonly property var peer: { App.peers.revision; return App.peers.get(devId) }
    readonly property string deviceName: peer.displayName !== undefined ? peer.displayName : qsTr("Unknown device")
    readonly property int stateCode: peer.stateCode !== undefined ? peer.stateCode : Theme.stateUnknown
    readonly property bool peerConnected: stateCode === Theme.stateConnected
    readonly property bool isVideo: service === "camera" || service === "screen" || service === "control"
    readonly property bool fullScreen: win.visibility === Window.FullScreen

    // The live view state for this peer (includes the host's camera/monitor
    // list and the current index), refreshed as the controller reports it.
    property var mediaState: ({})
    function refreshState() {
        mediaState = (service === "camera" || service === "screen" || service === "control")
            ? media.state(devId) : ({})
    }
    function releaseKeyName() {
        return ({228: "Right Ctrl", 41: "Escape", 69: "F12"})[media.settings.releaseUsage || 228]
    }
    Connections {
        target: win.media
        function onStateChanged(peer) { if (peer === win.devId) win.refreshState() }
    }
    Component.onCompleted: refreshState()

    // Showing this camera to other apps as a webcam ("uConnect Camera").
    readonly property var vcam: service === "camera" ? App.camera.virtualCamera : null
    readonly property bool vcamOn: vcam !== null && vcam.peer === devId
    function toggleWebcam() {
        if (vcamOn) { vcam.stop(); return }
        if (!vcam.installed && vcam.canInstall) { installWebcam.open(); return }
        vcam.start(devId, content.item && content.item.rotationDeg !== undefined ? content.item.rotationDeg : 0)
    }
    // Its last message (install result, why it could not start), shown here:
    // the main window's status bar is out of sight from a viewer.
    property string vcamMessage: ""
    Connections {
        target: win.vcam
        function onNotify(text) { win.vcamMessage = text }
    }
    Connections {
        target: content.item
        ignoreUnknownSignals: true
        function onRotationDegChanged() { if (win.vcamOn) win.vcam.setRotation(content.item.rotationDeg) }
    }
    Popup {
        id: installWebcam
        anchors.centerIn: Overlay.overlay
        modal: true
        padding: 20
        width: 440
        background: Rectangle { color: Theme.panel; border.color: Theme.popupBorder; radius: 8 }
        ColumnLayout {
            width: parent.width
            spacing: 10
            RowLayout {
                spacing: 10
                UIcon { name: "webcam"; size: 22; color: Theme.accent }
                Text {
                    text: qsTr("Use cameras in other apps")
                    color: Theme.text
                    font.pixelSize: 16
                    font.bold: true
                }
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.textSecondary
                text: qsTr("uConnect can show the camera you are watching to other apps, as “uConnect Camera”. That needs a small system component, installed once with administrator rights.")
            }
            RowLayout {
                Layout.topMargin: 6
                Item { Layout.fillWidth: true }
                UButton { text: qsTr("Cancel"); onClicked: installWebcam.close() }
                UButton {
                    text: qsTr("Install...")
                    onClicked: { win.vcam.install(); installWebcam.close() }
                }
            }
        }
    }

    width: 1000
    height: 680
    minimumWidth: 560
    minimumHeight: 380
    color: Theme.canvas
    title: Theme.serviceLabel(service) + " – " + deviceName
    onClosing: if (service === "screen" || service === "control" || service === "camera") media.stop(devId)
    onActiveChanged: if (!active && (service === "screen" || service === "control")) media.releaseInput(devId)

    component Separator: Rectangle {
        implicitWidth: 1
        implicitHeight: 18
        color: Theme.border
    }

    component Placeholder: ColumnLayout {
        property string heading
        property string body
        property color textColor: Theme.text
        spacing: 6
        Text {
            Layout.alignment: Qt.AlignHCenter
            textFormat: Text.PlainText
            text: parent.heading
            color: parent.textColor
            font.pixelSize: 16
        }
        Text {
            Layout.alignment: Qt.AlignHCenter
            Layout.maximumWidth: 420
            textFormat: Text.PlainText
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            text: parent.body
            color: Theme.textSecondary
        }
    }

    // --- per-service content ------------------------------------------------
    Component {
        id: storageContent
        StorageView {
            devId: win.devId
            deviceName: win.deviceName
            connected: win.peerConnected
        }
    }
    Component {
        id: videoContent
        Rectangle {
            color: "#000000"
            Placeholder {
                anchors.centerIn: parent
                heading: win.service === "camera" ? qsTr("No video") : qsTr("No screen")
                body: win.service === "camera"
                      ? qsTr("The camera feed from %1 will appear here.").arg(win.deviceName)
                      : win.service === "screen"
                        ? qsTr("%1's screen will appear here.").arg(win.deviceName)
                        : qsTr("%1's screen will appear here, with your mouse and keyboard controlling it.").arg(win.deviceName)
            }
        }
    }
    Component {
        id: screenContent
        ScreenView {
            devId: win.devId
            controller: win.media
            camera: win.service === "camera"
            wantControl: win.service === "control"
        }
    }

    // The settings menu for the video view, opened from the header's tune
    // button. Bound to the loaded ScreenView (content.item).
    UMenu {
        id: viewMenu
        readonly property var v: content.item

        UMenuItem {
            text: qsTr("Fit to window")
            checkable: true; checked: viewMenu.v && viewMenu.v.fit
            onTriggered: if (viewMenu.v) viewMenu.v.fit = true
        }
        UMenuItem {
            text: qsTr("Actual size (100%)")
            checkable: true; checked: viewMenu.v && !viewMenu.v.fit
            onTriggered: if (viewMenu.v) viewMenu.v.fit = false
        }
        UMenuSeparator {}
        UMenu {
            title: qsTr("Quality")
            Instantiator {
                model: viewMenu.v ? viewMenu.v.qualities : []
                delegate: UMenuItem {
                    required property var modelData
                    required property int index
                    text: modelData.label
                    checkable: true
                    checked: viewMenu.v && viewMenu.v.qualityIndex === index
                    onTriggered: if (viewMenu.v) viewMenu.v.setQuality(index)
                }
                onObjectAdded: (i, obj) => qualityMenu.insertItem(i, obj)
                onObjectRemoved: (i, obj) => qualityMenu.removeItem(obj)
            }
            id: qualityMenu
        }
        UMenu {
            title: qsTr("Rotation")
            enabled: win.service === "camera"  // rotating a controlled screen misplaces input
            UMenuItem {
                text: qsTr("None"); checkable: true
                checked: viewMenu.v && viewMenu.v.rotationDeg === 0
                onTriggered: if (viewMenu.v) viewMenu.v.rotationDeg = 0
            }
            UMenuItem {
                text: qsTr("90°"); checkable: true
                checked: viewMenu.v && viewMenu.v.rotationDeg === 90
                onTriggered: if (viewMenu.v) viewMenu.v.rotationDeg = 90
            }
            UMenuItem {
                text: qsTr("180°"); checkable: true
                checked: viewMenu.v && viewMenu.v.rotationDeg === 180
                onTriggered: if (viewMenu.v) viewMenu.v.rotationDeg = 180
            }
            UMenuItem {
                text: qsTr("270°"); checkable: true
                checked: viewMenu.v && viewMenu.v.rotationDeg === 270
                onTriggered: if (viewMenu.v) viewMenu.v.rotationDeg = 270
            }
        }
        UMenuSeparator {}
        UMenuItem {
            text: qsTr("Show statistics")
            checkable: true; checked: viewMenu.v && viewMenu.v.showStats
            onTriggered: if (viewMenu.v) viewMenu.v.showStats = !viewMenu.v.showStats
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 40
            color: Theme.panel

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 12
                spacing: 8

                Avatar {
                    name: win.deviceName
                    seed: win.peer.identityKey ? win.peer.identityKey : win.devId
                    os: win.peer.os ? win.peer.os : ""
                    size: 20
                }
                Text {
                    text: win.deviceName
                    color: Theme.text
                    font.bold: true
                    textFormat: Text.PlainText
                }
                Text {
                    text: Theme.serviceLabel(win.service)
                    color: Theme.textSecondary
                }

                Separator { visible: win.isVideo && (sourceCombo.visible || controlButton.visible) }

                // The video controls live here, in the one header bar.
                // Which camera / monitor to watch.
                UComboBox {
                    id: sourceCombo
                    visible: win.isVideo && (win.mediaState.monitors || []).length > 0
                    Layout.preferredWidth: 170
                    model: (win.mediaState.monitors || []).map(m => m.name)
                    currentIndex: win.mediaState.monitor || 0
                    enabled: win.mediaState.state === "live" && (win.mediaState.monitors || []).length > 1
                    onActivated: index => win.media.monitor(win.devId, index)
                }
                UIconButton {
                    id: controlButton
                    visible: win.service === "screen" || win.service === "control"
                    glyph: "mouse"
                    checked: win.mediaState.control === true
                    enabled: win.mediaState.state === "live" && win.mediaState.controlPending !== true
                    tip: win.mediaState.controlPending ? qsTr("Waiting for control…")
                         : win.mediaState.control ? qsTr("Stop controlling") : qsTr("Request control")
                    onClicked: win.media.requestControl(win.devId, win.mediaState.control !== true)
                }
                UIconButton {
                    visible: (win.service === "screen" || win.service === "control") && win.mediaState.control === true
                    glyph: "keyboard"
                    checked: win.mediaState.keyboardCaptured === true
                    tip: win.mediaState.keyboardCaptured
                         ? qsTr("Keyboard captured — press %1 to release").arg(win.releaseKeyName())
                         : qsTr("Capture keyboard")
                    onClicked: win.media.captureKeyboard(win.devId, win, win.mediaState.keyboardCaptured !== true)
                }

                Item { Layout.fillWidth: true }

                UIconButton {
                    visible: win.vcam !== null && win.vcam.supported
                    glyph: "webcam"
                    checked: win.vcamOn
                    enabled: (win.vcamOn || win.mediaState.state === "live") && !win.vcam.installing
                    tip: win.vcam === null ? ""
                         : win.vcam.installing ? qsTr("Installing the uConnect Camera…")
                         : win.vcamOn ? (win.vcam.inUse ? qsTr("An app is using it as “uConnect Camera” — click to stop")
                                                        : qsTr("Shown to apps as “uConnect Camera” — click to stop"))
                         : win.vcam.installed ? qsTr("Use as a webcam in other apps")
                         : win.vcam.canInstall ? qsTr("Use as a webcam in other apps (installs the uConnect Camera)")
                         : win.vcam.hint
                    onClicked: win.toggleWebcam()
                }
                UIconButton {
                    id: tuneButton
                    visible: win.isVideo
                    glyph: "tune"
                    tip: qsTr("View options")
                    enabled: content.item !== null
                    onClicked: viewMenu.popup(tuneButton, 0, tuneButton.height)
                }
                UIconButton {
                    visible: win.isVideo
                    glyph: win.fullScreen ? "fullscreen-exit" : "fullscreen"
                    tip: win.fullScreen ? qsTr("Exit full screen") : qsTr("Full screen")
                    onClicked: win.visibility = win.fullScreen ? Window.Windowed : Window.FullScreen
                }
                UIconButton {
                    visible: win.isVideo && content.item !== null && content.item.active
                    glyph: "stop"
                    tip: qsTr("Stop")
                    onClicked: win.media.stop(win.devId)
                }
                Separator { visible: win.isVideo }

                StatusDot {
                    color: Theme.stateColor(win.stateCode)
                }
                Text {
                    text: Theme.stateLabel(win.stateCode, win.peer.closeReason)
                    color: Theme.textSecondary
                    font.pixelSize: Theme.smallFontSize
                }
            }
            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: Theme.divider
            }
        }

        Banner {
            Layout.fillWidth: true
            Layout.margins: 8
            visible: !win.peerConnected
            kind: "error"
            text: qsTr("%1 is not connected.").arg(win.deviceName)
        }

        Banner {
            Layout.fillWidth: true
            Layout.leftMargin: 8
            Layout.rightMargin: 8
            Layout.topMargin: 8
            visible: win.vcam !== null && (win.vcam.installing || win.vcamMessage !== "")
            kind: win.vcam !== null && !win.vcam.installing && !win.vcam.installed ? "error" : "info"
            text: win.vcam !== null && win.vcam.installing
                  ? qsTr("Installing the uConnect Camera… Accept the administrator prompt to continue.")
                  : win.vcamMessage
            UIconButton {
                visible: win.vcam !== null && !win.vcam.installing
                glyph: "close"
                tip: qsTr("Dismiss")
                onClicked: win.vcamMessage = ""
            }
        }

        Loader {
            id: content
            Layout.fillWidth: true
            Layout.fillHeight: true
            sourceComponent: win.service === "storage" ? storageContent
                             : win.isVideo && win.media.available ? screenContent : videoContent
        }
    }
}
