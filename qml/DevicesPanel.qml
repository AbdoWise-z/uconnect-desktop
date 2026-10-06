import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import UConnect

// The "Devices" tool window.
Rectangle {
    id: panel

    property string currentDevId: ""

    signal openRequested(string devId)
    signal serviceRequested(string service, string devId)
    signal trustRequested(string devId)

    color: Theme.panel

    component DeviceRow: Rectangle {
        id: row

        required property string devId
        required property string shortId
        required property string displayName
        required property string os
        required property var services
        required property int stateCode
        required property string closeReason
        required property bool stale
        required property bool relayed
        required property string identityState
        required property string identityKey
        required property bool trusted
        required property var permissions

        width: ListView.view ? ListView.view.width : 0
        implicitHeight: 28
        color: devId === panel.currentDevId ? Theme.selection
               : rowArea.containsMouse ? Theme.hover : "transparent"
        radius: Theme.radius
        opacity: stale && stateCode !== Theme.stateConnected ? 0.55 : 1

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 8
            anchors.rightMargin: 10
            spacing: 8

            Avatar {
                name: row.displayName
                seed: row.identityKey.length > 0 ? row.identityKey : row.devId
                size: 18
            }
            Text {
                Layout.fillWidth: true
                elide: Text.ElideRight
                text: row.displayName
                color: Theme.text
                textFormat: Text.PlainText
            }
            Text {
                text: row.os
                textFormat: Text.PlainText
                visible: row.os.length > 0
                color: Theme.textSecondary
                font.pixelSize: Theme.smallFontSize
            }
            Text {
                visible: row.trusted || row.identityState === "invalid"
                text: row.trusted ? qsTr("trusted") : qsTr("invalid ID")
                color: row.trusted ? Theme.success : Theme.error
                font.pixelSize: Theme.smallFontSize
            }
            Text {
                visible: row.relayed && row.stateCode === Theme.stateConnected
                text: qsTr("relay")
                color: Theme.warning
                font.pixelSize: Theme.smallFontSize
            }
            StatusDot {
                color: Theme.stateColor(row.stateCode)
                pulsing: row.stateCode === Theme.stateProbing || row.stateCode === Theme.stateHandshaking
            }
        }

        MouseArea {
            id: rowArea
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onClicked: mouse => {
                if (mouse.button === Qt.RightButton) {
                    contextMenu.devId = row.devId
                    contextMenu.stateCode = row.stateCode
                    contextMenu.services = row.services
                    contextMenu.identityState = row.identityState
                    contextMenu.identityKey = row.identityKey
                    contextMenu.trusted = row.trusted
                    contextMenu.permissions = row.permissions
                    contextMenu.popup()
                } else {
                    panel.openRequested(row.devId)
                }
            }
        }

        ToolTip.visible: rowArea.containsMouse
        ToolTip.delay: 700
        ToolTip.text: Theme.stateLabel(stateCode, closeReason) + " · " + shortId
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 36
            Layout.leftMargin: 12
            Layout.rightMargin: 6
            spacing: 6

            Text {
                text: qsTr("Devices")
                color: Theme.text
                font.bold: true
            }
            Text {
                text: App.peers.count > 0 ? App.peers.count : ""
                color: Theme.textSecondary
            }
            Item { Layout.fillWidth: true }
            UToolButton {
                text: "↻"
                tip: qsTr("Refresh (F5)")
                onClicked: App.refreshPeers()
            }
        }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: 6
            Layout.rightMargin: 6
            clip: true
            spacing: 1
            model: App.peers
            delegate: DeviceRow {}
            ScrollBar.vertical: ScrollBar {}

            Text {
                anchors.centerIn: parent
                width: parent.width - 32
                visible: list.count === 0
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                color: Theme.textSecondary
                text: qsTr("No other devices in this topic yet.\n\nOn another device, open uConnect and connect with this topic's invite link (Topic › Copy Invite Link).")
            }
        }
    }

    UMenu {
        id: contextMenu

        property string devId: ""
        property int stateCode: 0
        property var services: []
        property string identityState: ""
        property string identityKey: ""
        property bool trusted: false
        property var permissions: []
        readonly property bool linked: stateCode === Theme.stateConnected
                                       || stateCode === Theme.stateProbing
                                       || stateCode === Theme.stateHandshaking

        UMenuItem {
            text: qsTr("Open")
            onTriggered: panel.openRequested(contextMenu.devId)
        }
        UMenuSeparator {}
        UMenuItem {
            text: contextMenu.linked ? qsTr("Disconnect") : qsTr("Connect")
            onTriggered: contextMenu.linked ? App.disconnectPeer(contextMenu.devId)
                                            : App.connectPeer(contextMenu.devId)
        }
        UMenuSeparator {}
        Instantiator {
            model: contextMenu.services
            delegate: UMenuItem {
                required property string modelData
                text: qsTr("Open %1").arg(Theme.serviceLabel(modelData))
                enabled: contextMenu.stateCode === Theme.stateConnected
                onTriggered: panel.serviceRequested(modelData, contextMenu.devId)
            }
            onObjectAdded: (index, object) => contextMenu.insertItem(4 + index, object)
            onObjectRemoved: (index, object) => contextMenu.removeItem(object)
        }
        UMenuSeparator {
            visible: contextMenu.services.length > 0
        }
        UMenuItem {
            text: qsTr("Trust...")
            visible: !contextMenu.trusted
            enabled: contextMenu.identityState === "verified"
            onTriggered: panel.trustRequested(contextMenu.devId)
        }
        UMenuItem {
            text: qsTr("Untrust")
            visible: contextMenu.trusted
            onTriggered: App.untrustDevice(contextMenu.identityKey)
        }
        UMenu {
            id: permissionsMenu
            title: qsTr("Permissions")
            enabled: contextMenu.trusted

            Instantiator {
                model: App.permissionDefinitions
                delegate: UMenuItem {
                    required property var modelData
                    text: Theme.serviceLabel(modelData.service) + " › " + modelData.label
                    hint: modelData.available ? "" : qsTr("not available yet")
                    checkable: true
                    checked: contextMenu.permissions.indexOf(modelData.id) >= 0
                    onTriggered: {
                        App.setDevicePermission(contextMenu.identityKey, modelData.id, checked)
                        // Clicking broke the binding; restore it for the next time.
                        checked = Qt.binding(() => contextMenu.permissions.indexOf(modelData.id) >= 0)
                    }
                }
                onObjectAdded: (index, object) => permissionsMenu.insertItem(index, object)
                onObjectRemoved: (index, object) => permissionsMenu.removeItem(object)
            }
        }
        UMenuSeparator {}
        UMenuItem {
            text: qsTr("Copy Device ID")
            onTriggered: App.copyText(contextMenu.devId)
        }
    }
}
