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
    readonly property var peer: { App.peers.revision; return App.peers.get(devId) }
    readonly property string deviceName: peer.displayName !== undefined ? peer.displayName : qsTr("Unknown device")
    readonly property int stateCode: peer.stateCode !== undefined ? peer.stateCode : Theme.stateUnknown
    readonly property bool peerConnected: stateCode === Theme.stateConnected

    width: 1000
    height: 680
    minimumWidth: 560
    minimumHeight: 380
    color: Theme.canvas
    title: Theme.serviceLabel(service) + " – " + deviceName
    onClosing: if (service === "screen" || service === "control") App.screen.stop(devId)

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

    // --- per-service toolbars ----------------------------------------------
    Component {
        id: cameraToolbar
        RowLayout {
            spacing: 4
            UToolButton { text: qsTr("Default Camera ▾"); enabled: false }
            Separator {}
            UToolButton { text: qsTr("Snapshot"); enabled: false }
            UToolButton { text: qsTr("Full Screen"); enabled: false }
        }
    }
    Component {
        id: screenToolbar
        RowLayout {
            spacing: 4
            UToolButton { text: qsTr("Fit"); checkable: true; checked: true; enabled: false }
            UToolButton { text: "100%"; checkable: true; enabled: false }
            Separator {}
            UToolButton { text: qsTr("Full Screen"); enabled: false }
        }
    }
    Component {
        id: controlToolbar
        RowLayout {
            spacing: 4
            UToolButton { text: qsTr("Ctrl+Alt+Del"); enabled: false }
            UToolButton { text: qsTr("Sync Clipboard"); checkable: true; enabled: false }
            Separator {}
            UToolButton { text: qsTr("Fit"); checkable: true; checked: true; enabled: false }
            UToolButton { text: qsTr("Full Screen"); enabled: false }
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
        Loader {
            Component.onCompleted: setSource(Qt.resolvedUrl("ScreenView.qml"), {devId: win.devId, wantControl: win.service === "control"})
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
                Separator {}
                // Storage keeps its navigation bar inside StorageView.
                Loader {
                    sourceComponent: win.service === "camera" ? cameraToolbar
                                   : null
                }
                Item { Layout.fillWidth: true }
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

        Loader {
            Layout.fillWidth: true
            Layout.fillHeight: true
            sourceComponent: win.service === "storage" ? storageContent
                             : (win.service === "screen" || win.service === "control") && App.screen.available ? screenContent : videoContent
        }
    }
}
