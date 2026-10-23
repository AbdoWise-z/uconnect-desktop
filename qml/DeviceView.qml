import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import UConnect

// The editor tab for one device.
Flickable {
    id: view

    required property string devId
    readonly property var peer: { App.peers.revision; return App.peers.get(devId) }
    readonly property bool gone: peer.devId === undefined
    readonly property int stateCode: gone ? Theme.stateUnknown : peer.stateCode
    readonly property bool connected: stateCode === Theme.stateConnected
    readonly property bool linked: connected || stateCode === Theme.stateProbing || stateCode === Theme.stateHandshaking
    readonly property string name: gone ? qsTr("Device %1").arg(devId.substring(0, 8)) : peer.displayName
    readonly property string identityState: gone ? "pending" : peer.identityState
    readonly property bool verified: identityState === "verified"
    // It has special rules here (its own permissions, by its key).
    readonly property bool special: !gone && peer.special === true
    // The network rule its address matched, if any.
    readonly property var matchedRule: (peer.networkRule !== undefined && peer.networkRule >= 0
                                        && peer.networkRule < App.networkRules.length)
                                       ? App.networkRules[peer.networkRule] : null
    // Its special rules entry (with its name when they were added), if any.
    readonly property var specialEntry: verified ? App.specialDevices.find(t => t.key === peer.identityKey) : undefined

    // The services it offers, and what it lets us do with each.
    readonly property var offeredServices: gone ? [] : Theme.services.filter(s => peer.services.indexOf(s) >= 0)
    readonly property var notOffered: Theme.services.filter(s => offeredServices.indexOf(s) < 0)
    function grantsFor(service) {
        if (gone) return []
        return App.permissionDefinitions.filter(d => d.service === service && peer.grantedToMe.indexOf(d.id) >= 0)
                                        .map(d => d.label)
    }
    function mayUse(service) {
        return !gone && peer.grantedToMe.indexOf(Theme.serviceGrant(service)) >= 0
    }
    readonly property string rulesIcon: special ? "verified" : peer.permissionSource === "rule" ? "lan" : "shield"
    readonly property color rulesColor: special ? Theme.success
                                        : peer.permissionSource === "rule" ? Theme.accent : Theme.textSecondary

    signal serviceRequested(string service, string devId)
    signal specialRulesRequested(string devId)
    signal settingsRequested(string pageId)

    // Synced folders, rebuilt only when folders come, go or are renamed:
    // App.syncFolders changes with every bit of sync progress.
    property var syncFolderList: []
    readonly property string syncSignature: App.syncFolders.map(f => f.id + "\u0001" + f.label).join("\u0002")
    onSyncSignatureChanged: syncFolderList = App.syncFolders.map(f => ({ id: f.id, label: f.label }))
    Component.onCompleted: syncFolderList = App.syncFolders.map(f => ({ id: f.id, label: f.label }))
    function syncGrant(folderId) {
        const f = App.syncFolders.find(x => x.id === folderId)
        const m = f && !gone ? f.members.find(x => x.key === peer.identityKey) : undefined
        return m ? m : { receive: false, contribute: false }
    }
    readonly property var offeredToMe: gone ? [] : App.syncOffers.filter(o => o.key === peer.identityKey && !o.joined)

    clip: true
    contentWidth: width
    contentHeight: body.implicitHeight + 56
    boundsBehavior: Flickable.StopAtBounds
    ScrollBar.vertical: ScrollBar {}

    component Field: Text {
        color: Theme.textSecondary
        Layout.preferredWidth: 130
        Layout.alignment: Qt.AlignTop
    }

    ColumnLayout {
        id: body
        x: 32
        y: 28
        width: Math.min(760, view.width - 64)
        spacing: 12

        RowLayout {
            spacing: 14
            Avatar {
                name: view.name
                seed: view.verified ? view.peer.identityKey : view.devId
                os: view.peer.os ? view.peer.os : ""
                size: 48
            }
            ColumnLayout {
                spacing: 2
                Layout.fillWidth: true
                RowLayout {
                    spacing: 8
                    Layout.fillWidth: true
                    Text {
                        text: view.name
                        color: Theme.text
                        font.pixelSize: 20
                        elide: Text.ElideRight
                        Layout.maximumWidth: body.width - 260
                        textFormat: Text.PlainText
                    }
                    UIcon {
                        visible: view.special
                        name: "verified"
                        size: 18
                        color: Theme.success
                        HoverHandler { id: specialHover }
                        ToolTip.visible: specialHover.hovered
                        ToolTip.delay: 400
                        ToolTip.text: qsTr("Special rules")
                    }
                    Item { Layout.fillWidth: true }
                }
                Text {
                    color: Theme.textSecondary
                    textFormat: Text.PlainText
                    text: !view.gone && view.peer.os ? view.peer.os + "  ·  " + view.devId.substring(0, 8)
                                                    : view.devId.substring(0, 8)
                }
            }
            UButton {
                visible: !view.gone
                text: view.linked ? qsTr("Disconnect") : qsTr("Connect")
                primary: !view.linked
                onClicked: view.linked ? App.disconnectPeer(view.devId) : App.connectPeer(view.devId)
            }
        }

        Banner {
            Layout.fillWidth: true
            visible: view.gone
            kind: "error"
            text: qsTr("This device is no longer in the topic.")
        }

        TitledSeparator {
            text: qsTr("Connection")
        }

        GridLayout {
            columns: 2
            columnSpacing: 12
            rowSpacing: 8
            Layout.fillWidth: true

            Field { text: qsTr("Status") }
            RowLayout {
                spacing: 6
                StatusDot {
                    color: Theme.stateColor(view.stateCode)
                    pulsing: view.stateCode === Theme.stateProbing || view.stateCode === Theme.stateHandshaking
                }
                Text {
                    color: Theme.text
                    text: view.gone ? qsTr("Gone") : Theme.stateLabel(view.stateCode, view.peer.closeReason)
                }
            }

            Field { text: qsTr("Path"); visible: view.connected }
            Text {
                visible: view.connected
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: view.peer.relayed ? Theme.warning : Theme.text
                text: view.peer.relayed
                      ? qsTr("Relayed through the rendezvous server. Still end-to-end encrypted, but the server sees traffic volume.")
                      : qsTr("Direct (peer to peer)")
            }

            // The address of the direct connection's socket, as this device
            // sees it: what network rules match against.
            Field { text: qsTr("Connected from"); visible: view.connected }
            RowLayout {
                visible: view.connected
                spacing: 10
                Text {
                    text: view.peer.address ? view.peer.address : qsTr("Not known (relayed)")
                    color: view.peer.address ? Theme.text : Theme.textSecondary
                    font.family: view.peer.address ? "Consolas, monospace" : Qt.application.font.family
                }
                UIconButton {
                    visible: !!view.peer.address
                    glyph: "copy"
                    iconSize: 14
                    implicitHeight: 22
                    implicitWidth: 24
                    tip: qsTr("Copy address")
                    onClicked: App.copyText(view.peer.address)
                }
            }

            Field { text: qsTr("Traffic"); visible: view.connected }
            Text {
                visible: view.connected
                color: Theme.text
                text: qsTr("%1 sent  ·  %2 received")
                          .arg(Theme.formatBytes(view.peer.bytesSent))
                          .arg(Theme.formatBytes(view.peer.bytesReceived))
            }

            Field { text: qsTr("Device ID") }
            RowLayout {
                spacing: 10
                Text {
                    text: view.devId
                    color: Theme.text
                    font.family: "Consolas, monospace"
                }
                UIconButton {
                    glyph: "copy"
                    iconSize: 14
                    implicitHeight: 22
                    implicitWidth: 24
                    tip: qsTr("Copy device ID")
                    onClicked: App.copyText(view.devId)
                }
            }
        }

        TitledSeparator {
            text: qsTr("Identity & Rules")
        }

        GridLayout {
            columns: 2
            columnSpacing: 12
            rowSpacing: 8
            Layout.fillWidth: true

            Field { text: qsTr("Fingerprint") }
            RowLayout {
                spacing: 10
                Text {
                    visible: view.verified
                    text: view.verified ? view.peer.fingerprint : ""
                    color: Theme.text
                    font.family: "Consolas, monospace"
                }
                UIconButton {
                    visible: view.verified
                    glyph: "copy"
                    iconSize: 14
                    implicitHeight: 22
                    implicitWidth: 24
                    tip: qsTr("Copy fingerprint")
                    onClicked: App.copyText(view.peer.fingerprint)
                }
                Text {
                    visible: !view.verified
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: view.identityState === "invalid" ? Theme.error : Theme.textSecondary
                    text: view.identityState === "invalid"
                          ? qsTr("Its identity proof did not verify: something may be impersonating it. Give it nothing.")
                          : view.connected ? qsTr("Waiting for it to prove its identity...")
                                           : qsTr("Connect to this device to see its identity.")
                }
            }

            Field { text: qsTr("Rules") }
            RowLayout {
                spacing: 10
                UIcon {
                    name: view.rulesIcon
                    size: 16
                    color: view.rulesColor
                }
                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    textFormat: Text.PlainText
                    color: Theme.text
                    text: view.special
                          ? (view.specialEntry && view.specialEntry.name !== view.name
                             ? qsTr("Special rules (added as “%1”)").arg(view.specialEntry.name)
                             : qsTr("Special rules"))
                          : view.peer.permissionSource === "rule"
                            ? qsTr("Network rule %1").arg(view.matchedRule ? (view.matchedRule.label || view.matchedRule.pattern) : "")
                            : qsTr("Default permissions")
                }
                UButton {
                    visible: view.verified && !view.special
                    text: qsTr("Add Special Rules...")
                    onClicked: view.specialRulesRequested(view.devId)
                }
                UButton {
                    visible: view.special
                    text: qsTr("Remove Special Rules")
                    onClicked: App.removeSpecialRules(view.peer.identityKey)
                }
            }
        }

        TitledSeparator {
            text: qsTr("What It May Do Here")
        }
        // Where its permissions come from: its own set, a network rule, or the
        // defaults (first that applies, see AccessPolicy).
        Text {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textSecondary
            text: view.peer.permissionSource === "device"
                  ? qsTr("Its special rules. Changes apply immediately.")
                  : view.peer.permissionSource === "rule"
                    ? qsTr("From the network rule %1, because it connects from %2.")
                          .arg(view.matchedRule ? (view.matchedRule.label || view.matchedRule.pattern) : "")
                          .arg(view.peer.address)
                    : qsTr("Your default permissions, which any device with the invite link gets.")
        }
        Rectangle {
            visible: !view.special && !view.gone
            Layout.fillWidth: true
            implicitHeight: lockRow.implicitHeight + 16
            radius: Theme.radius
            color: Theme.panel
            border.color: Theme.border
            RowLayout {
                id: lockRow
                anchors.fill: parent
                anchors.margins: 8
                anchors.leftMargin: 12
                spacing: 10
                UIcon { name: "lock"; size: 16; color: Theme.textSecondary }
                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: Theme.textSecondary
                    text: view.verified ? qsTr("Add special rules to change these for this device.")
                                        : qsTr("Special rules can be added once it has proved its identity.")
                }
                UButton {
                    visible: view.verified
                    text: qsTr("Add Special Rules...")
                    onClicked: view.specialRulesRequested(view.devId)
                }
            }
        }
        PermissionEditor {
            Layout.fillWidth: true
            granted: view.gone ? [] : view.peer.permissions
            editable: view.special
            opacity: view.special ? 1 : 0.55
            onToggled: (permission, granted) => App.setDevicePermission(view.peer.identityKey, permission, granted)
        }
        RowLayout {
            spacing: 14
            ULink {
                visible: view.peer.permissionSource !== "device"
                text: view.peer.permissionSource === "rule" ? qsTr("Edit network rules...") : qsTr("Edit default permissions...")
                onClicked: view.settingsRequested(view.peer.permissionSource === "rule" ? "network" : "permissions")
            }
        }

        TitledSeparator {
            text: qsTr("Synced Folders")
        }
        Text {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textSecondary
            text: !view.special
                  ? qsTr("Folders sync only with devices that have special rules.")
                  : view.syncFolderList.length === 0
                  ? qsTr("You have no synced folders yet.")
                  : qsTr("Send: it gets the folder and your changes. Accept: you take its changes.")
        }
        Repeater {
            model: view.special ? view.syncFolderList : []
            delegate: RowLayout {
                id: syncRow
                required property var modelData
                readonly property var grant: view.syncGrant(modelData.id)
                Layout.fillWidth: true
                spacing: 12
                Text {
                    Layout.fillWidth: true
                    text: syncRow.modelData.label
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    color: Theme.text
                }
                UCheckBox {
                    text: qsTr("Send")
                    checked: syncRow.grant.receive
                    onToggled: App.setSyncMember(syncRow.modelData.id, view.peer.identityKey, checked, syncRow.grant.contribute)
                }
                UCheckBox {
                    text: qsTr("Accept")
                    checked: syncRow.grant.contribute
                    onToggled: App.setSyncMember(syncRow.modelData.id, view.peer.identityKey, syncRow.grant.receive, checked)
                }
            }
        }
        Text {
            Layout.fillWidth: true
            visible: view.offeredToMe.length > 0
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            color: Theme.text
            text: qsTr("%1 offers you: %2").arg(view.name).arg(view.offeredToMe.map(o => o.label).join(", "))
        }
        ULink {
            text: view.offeredToMe.length > 0 ? qsTr("Join in Synced Folders...") : qsTr("Open Synced Folders...")
            onClicked: view.serviceRequested("sync", view.devId)
        }

        TitledSeparator {
            text: qsTr("What It Lets You Do")
        }

        Text {
            visible: !view.connected || !view.peer.hasHello
            color: Theme.textSecondary
            text: view.connected ? qsTr("Waiting for this device to introduce itself...")
                                 : qsTr("Connect to this device to use its services.")
        }

        Repeater {
            model: view.connected && view.peer.hasHello ? view.offeredServices : []
            delegate: Rectangle {
                id: serviceRow
                required property string modelData
                readonly property var grants: view.grantsFor(modelData)
                readonly property bool usable: view.mayUse(modelData)

                Layout.fillWidth: true
                implicitHeight: 52
                radius: Theme.radius
                color: serviceArea.containsMouse && usable ? Theme.hover : "transparent"
                border.color: Theme.border

                MouseArea {
                    id: serviceArea
                    anchors.fill: parent
                    hoverEnabled: true
                    enabled: serviceRow.usable
                    cursorShape: serviceRow.usable ? Qt.PointingHandCursor : Qt.ArrowCursor
                    onDoubleClicked: view.serviceRequested(serviceRow.modelData, view.devId)
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 8
                    spacing: 12

                    UIcon {
                        name: Theme.serviceIcon(serviceRow.modelData)
                        size: 20
                        color: serviceRow.usable ? Theme.accent : Theme.textDisabled
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 1
                        Text {
                            text: Theme.serviceLabel(serviceRow.modelData)
                            color: serviceRow.usable ? Theme.text : Theme.textSecondary
                        }
                        Text {
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            font.pixelSize: Theme.smallFontSize
                            color: serviceRow.usable ? Theme.textSecondary : Theme.textDisabled
                            text: serviceRow.grants.length > 0
                                  ? qsTr("Allowed: %1").arg(serviceRow.grants.join(", "))
                                  : qsTr("Not allowed yet: %1 has to give you permission.").arg(view.name)
                        }
                    }
                    UIconButton {
                        glyph: "open"
                        enabled: serviceRow.usable
                        tip: qsTr("Open %1").arg(Theme.serviceLabel(serviceRow.modelData))
                        onClicked: view.serviceRequested(serviceRow.modelData, view.devId)
                    }
                }
            }
        }

        Text {
            visible: view.connected && view.peer.hasHello && view.notOffered.length > 0
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textDisabled
            font.pixelSize: Theme.smallFontSize
            text: qsTr("Not offered: %1").arg(view.notOffered.map(s => Theme.serviceLabel(s)).join(", "))
        }
    }
}
