import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import UConnect

// The connected workspace: the Devices tool window on the left, one editor
// tab per opened device on the right.
Rectangle {
    id: ws

    property bool devicesPanelVisible: true
    property int currentTab: -1
    readonly property string currentDevId: currentTab >= 0 && currentTab < tabs.count ? tabs.get(currentTab).devId : ""

    signal serviceRequested(string service, string devId)
    signal trustRequested(string devId)
    signal settingsRequested(string pageId)

    color: Theme.canvas

    function openDevice(devId) {
        for (let i = 0; i < tabs.count; ++i) {
            if (tabs.get(i).devId === devId) {
                currentTab = i
                return
            }
        }
        tabs.append({ devId: devId })
        currentTab = tabs.count - 1
    }

    function closeTab(index) {
        tabs.remove(index)
        if (index < currentTab || currentTab >= tabs.count) currentTab -= 1
        if (currentTab < 0 && tabs.count > 0) currentTab = 0
    }

    ListModel {
        id: tabs
    }

    Connections {
        target: App
        function onStatusChanged() {
            if (!App.active) {
                tabs.clear()
                ws.currentTab = -1
            }
        }
    }

    component EditorTab: Rectangle {
        id: tab

        required property int index
        required property string devId
        readonly property var peer: { App.peers.revision; return App.peers.get(devId) }
        readonly property string name: peer.displayName !== undefined ? peer.displayName : qsTr("Gone")
        readonly property bool current: index === ws.currentTab

        implicitWidth: tabRow.implicitWidth + 24
        implicitHeight: 36
        color: tabArea.containsMouse && !current ? Theme.hover : "transparent"

        RowLayout {
            id: tabRow
            anchors.centerIn: parent
            spacing: 6
            Avatar {
                name: tab.name
                seed: tab.peer.identityKey ? tab.peer.identityKey : tab.devId
                size: 16
            }
            Text {
                text: tab.name
                textFormat: Text.PlainText
                color: tab.current ? Theme.text : Theme.textSecondary
                opacity: tab.peer.devId !== undefined ? 1 : 0.6
            }
            Text {
                text: "×"
                font.pixelSize: 15
                color: closeArea.containsMouse ? Theme.text : Theme.textSecondary
                opacity: tab.current || tabArea.containsMouse ? 1 : 0
                MouseArea {
                    id: closeArea
                    anchors.fill: parent
                    anchors.margins: -3
                    hoverEnabled: true
                    onClicked: ws.closeTab(tab.index)
                }
            }
        }
        Rectangle {
            visible: tab.current
            anchors.bottom: parent.bottom
            width: parent.width
            height: 2
            color: Theme.accent
        }
        MouseArea {
            id: tabArea
            anchors.fill: parent
            z: -1
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.MiddleButton
            onClicked: mouse => {
                if (mouse.button === Qt.MiddleButton) ws.closeTab(tab.index)
                else ws.currentTab = tab.index
            }
        }
    }

    component Hint: RowLayout {
        property alias action: actionLabel.text
        property alias keys: keysLabel.text
        spacing: 10
        Layout.alignment: Qt.AlignHCenter
        Text {
            id: actionLabel
            color: Theme.textSecondary
            horizontalAlignment: Text.AlignRight
            Layout.preferredWidth: 220
        }
        Text {
            id: keysLabel
            color: Theme.link
            Layout.preferredWidth: 220
        }
    }

    SplitView {
        anchors.fill: parent
        orientation: Qt.Horizontal

        handle: Rectangle {
            implicitWidth: 4
            color: SplitHandle.pressed || SplitHandle.hovered ? Theme.accent : "transparent"
            opacity: 0.6
        }

        DevicesPanel {
            SplitView.preferredWidth: 290
            SplitView.minimumWidth: 200
            visible: ws.devicesPanelVisible
            currentDevId: ws.currentDevId
            onOpenRequested: devId => ws.openDevice(devId)
            onServiceRequested: (service, devId) => ws.serviceRequested(service, devId)
            onTrustRequested: devId => ws.trustRequested(devId)
        }

        ColumnLayout {
            SplitView.fillWidth: true
            spacing: 0

            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 36
                visible: tabs.count > 0
                color: Theme.canvas

                Flickable {
                    anchors.fill: parent
                    contentWidth: tabStrip.implicitWidth
                    flickableDirection: Flickable.HorizontalFlick
                    clip: true
                    Row {
                        id: tabStrip
                        Repeater {
                            model: tabs
                            delegate: EditorTab {}
                        }
                    }
                }
                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: 1
                    color: Theme.border
                }
            }

            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: tabs.count > 0
                currentIndex: ws.currentTab

                Repeater {
                    model: tabs
                    delegate: DeviceView {
                        onServiceRequested: (service, devId) => ws.serviceRequested(service, devId)
                        onTrustRequested: devId => ws.trustRequested(devId)
                        onSettingsRequested: pageId => ws.settingsRequested(pageId)
                    }
                }
            }

            // Empty editor area: the shortcuts, as the IDE shows them.
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true
                visible: tabs.count === 0

                ColumnLayout {
                    anchors.centerIn: parent
                    spacing: 12
                    Hint {
                        action: qsTr("Open a device")
                        keys: qsTr("Click it in Devices")
                    }
                    Hint {
                        action: qsTr("Refresh devices")
                        keys: "F5"
                    }
                    Hint {
                        action: qsTr("Show or hide Devices")
                        keys: "Alt+1"
                    }
                    Hint {
                        action: qsTr("Settings")
                        keys: "Ctrl+Alt+S"
                    }
                    Hint {
                        action: qsTr("Debug window")
                        keys: "Ctrl+Shift+D"
                        visible: App.debugAvailable
                    }
                }
            }
        }
    }
}
