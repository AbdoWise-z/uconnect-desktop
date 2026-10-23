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

    function permissionLabel(id) {
        const d = App.permissionDefinitions.find(p => p.id === id)
        return d ? Theme.serviceLabel(d.service) + " › " + d.label : id
    }

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
                Text {
                    text: view.name
                    color: Theme.text
                    font.pixelSize: 20
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                    textFormat: Text.PlainText
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
                      : view.peer.address ? qsTr("Direct, from %1").arg(view.peer.address) : qsTr("Direct")
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
                ULink {
                    text: qsTr("Copy")
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
                ULink {
                    visible: view.verified
                    text: qsTr("Copy")
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
                StatusDot {
                    color: view.special ? Theme.success
                         : view.peer.permissionSource === "rule" ? Theme.accent : Theme.textSecondary
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
                    ? qsTr("From the network rule %1, because it connects from %2. Add special rules to give it its own.")
                          .arg(view.matchedRule ? (view.matchedRule.label || view.matchedRule.pattern) : "")
                          .arg(view.peer.address)
                    : qsTr("Your default permissions, which any device with the invite link gets. Add special rules to give it its own.")
        }
        PermissionEditor {
            Layout.fillWidth: true
            granted: view.gone ? [] : view.peer.permissions
            editable: view.special
            onToggled: (permission, granted) => App.setDevicePermission(view.peer.identityKey, permission, granted)
        }
        RowLayout {
            spacing: 14
            ULink {
                visible: view.verified && !view.special
                text: qsTr("Add special rules...")
                onClicked: view.specialRulesRequested(view.devId)
            }
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
            text: !view.verified
                  ? qsTr("A device takes part in folder sync once it has proved its identity.")
                  : view.syncFolderList.length === 0
                  ? qsTr("You have no synced folders yet.")
                  : qsTr("Send: it gets the folder and your changes. Accept: you take its changes.")
        }
        Repeater {
            model: view.verified ? view.syncFolderList : []
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
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            textFormat: Text.PlainText
            color: Theme.textSecondary
            text: !view.connected ? qsTr("Unknown until connected.")
                  : view.peer.grantedToMe.length === 0 ? qsTr("Nothing yet. %1 has to give you permissions.").arg(view.name)
                  : view.peer.grantedToMe.map(view.permissionLabel).join(", ")
        }

        TitledSeparator {
            text: qsTr("Services")
        }

        Text {
            visible: view.connected && !view.peer.hasHello
            color: Theme.textSecondary
            text: qsTr("Waiting for this device to introduce itself...")
        }
        Text {
            visible: !view.connected
            color: Theme.textSecondary
            text: qsTr("Connect to this device to use its services.")
        }

        Repeater {
            model: Theme.services
            delegate: RowLayout {
                id: serviceRow
                required property string modelData
                readonly property bool offered: !view.gone && view.peer.services.indexOf(modelData) >= 0

                Layout.fillWidth: true
                spacing: 12

                ColumnLayout {
                    spacing: 2
                    Text {
                        text: Theme.serviceLabel(modelData)
                        color: serviceRow.offered ? Theme.text : Theme.textSecondary
                    }
                    Text {
                        text: Theme.serviceDescription(modelData)
                        color: Theme.textSecondary
                        font.pixelSize: Theme.smallFontSize
                    }
                }
                Item { Layout.fillWidth: true }
                UButton {
                    visible: offered
                    text: qsTr("Open")
                    enabled: view.connected
                    onClicked: view.serviceRequested(modelData, view.devId)
                }
                Text {
                    visible: !offered
                    text: qsTr("Not offered")
                    color: Theme.textDisabled
                }
            }
        }
    }
}
