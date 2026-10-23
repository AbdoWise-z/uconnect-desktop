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
    signal specialRulesRequested(string devId)

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
        required property bool special
        required property var grantedToMe

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
                os: row.os
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
            // Badges: special rules (or a failed identity proof), relayed path.
            UIcon {
                visible: row.special || row.identityState === "invalid"
                name: row.special ? "verified" : "warning"
                size: 14
                color: row.special ? Theme.success : Theme.error
                HoverHandler { id: ruleHover }
                ToolTip.visible: ruleHover.hovered
                ToolTip.delay: 400
                ToolTip.text: row.special ? qsTr("Special rules") : qsTr("Identity proof failed")
            }
            UIcon {
                visible: row.relayed && row.stateCode === Theme.stateConnected
                name: "cloud"
                size: 14
                color: Theme.warning
                HoverHandler { id: relayHover }
                ToolTip.visible: relayHover.hovered
                ToolTip.delay: 400
                ToolTip.text: qsTr("Relayed through the server")
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
                    contextMenu.special = row.special
                    contextMenu.verified = row.identityState === "verified"
                    contextMenu.grantedToMe = row.grantedToMe
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
            UIconButton {
                glyph: "refresh"
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
        property bool special: false
        property bool verified: false
        property var grantedToMe: []
        readonly property bool connected: stateCode === Theme.stateConnected
        readonly property bool linked: connected
                                       || stateCode === Theme.stateProbing
                                       || stateCode === Theme.stateHandshaking
        // Services it offers that it lets us use.
        readonly property var usable: services.filter(s => grantedToMe.indexOf(Theme.serviceGrant(s)) >= 0)

        UMenuItem {
            text: qsTr("Open Device Tab")
            glyph: "info"
            onTriggered: panel.openRequested(contextMenu.devId)
        }
        UMenuItem {
            text: contextMenu.linked ? qsTr("Disconnect") : qsTr("Connect")
            glyph: contextMenu.linked ? "power" : "link"
            onTriggered: contextMenu.linked ? App.disconnectPeer(contextMenu.devId)
                                            : App.connectPeer(contextMenu.devId)
        }
        UMenuSeparator {}
        Instantiator {
            model: contextMenu.usable
            delegate: UMenuItem {
                required property string modelData
                text: Theme.serviceLabel(modelData)
                glyph: Theme.serviceIcon(modelData)
                enabled: contextMenu.connected
                onTriggered: panel.serviceRequested(modelData, contextMenu.devId)
            }
            onObjectAdded: (index, object) => contextMenu.insertItem(3 + index, object)
            onObjectRemoved: (index, object) => contextMenu.removeItem(object)
        }
        UMenuSeparator {
            visible: contextMenu.usable.length > 0
        }
        UMenuItem {
            text: qsTr("Add Special Rules...")
            glyph: "verified"
            visible: contextMenu.verified && !contextMenu.special
            onTriggered: panel.specialRulesRequested(contextMenu.devId)
        }
        UMenuItem {
            text: qsTr("Remove Special Rules")
            glyph: "block"
            visible: contextMenu.special
            onTriggered: App.removeSpecialRules(contextMenu.identityKey)
        }
        UMenuItem {
            text: qsTr("Copy Device ID")
            glyph: "copy"
            onTriggered: App.copyText(contextMenu.devId)
        }
    }
}
