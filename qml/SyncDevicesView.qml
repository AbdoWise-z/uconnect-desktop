import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import UConnect

// The devices a synced folder syncs with: what each one is sent and
// accepted (as before), and where each one is at: online or last seen,
// what is left in each direction with a time estimate, the file in flight,
// when it was last fully in sync, and how many paths it holds back.
ColumnLayout {
    id: view

    required property string folderId
    // The folder's members, with sync's view of each merged in (App.syncFolders[].members).
    property var members: []
    // Progress changes every second; the rows (with their checkboxes) are
    // rebuilt only when the set of members or their grants change.
    readonly property string signature: JSON.stringify(members.map(m => [m.key, m.name, m.receive, m.contribute, m.paused, m.syncs]))
    property var stableMembers: []
    onSignatureChanged: stableMembers = members
    Component.onCompleted: stableMembers = members
    spacing: 8

    function memberOf(key) {
        for (const m of members)
            if (m.key === key) return m
        return null
    }
    function whenText(ms) {
        if (!ms || ms <= 0) return qsTr("never")
        const d = new Date(ms)
        const now = new Date()
        const sameDay = d.toDateString() === now.toDateString()
        return Qt.formatDateTime(d, sameDay ? "hh:mm" : "yyyy-MM-dd hh:mm")
    }
    function leftText(m) {
        if (!m) return ""
        if (m.paused) return qsTr("Paused: nothing is sent to it or taken from it until you resume.")
        const parts = []
        if (m.toSendFiles > 0 || m.toSendChanges > 0) {
            let s = qsTr("To send: %1 file(s), %2").arg(m.toSendFiles).arg(Theme.formatBytes(m.toSendBytes))
            if (m.toSendChanges > 0) s += qsTr(" and %1 other change(s)").arg(m.toSendChanges)
            parts.push(s)
        }
        if (m.toReceiveFiles > 0 || m.toReceiveChanges > 0) {
            let s = qsTr("To receive: %1 file(s), %2").arg(m.toReceiveFiles).arg(Theme.formatBytes(m.toReceiveBytes))
            if (m.toReceiveChanges > 0) s += qsTr(" and %1 other change(s)").arg(m.toReceiveChanges)
            if (m.online && m.eta >= 0 && m.rate > 0)
                s += " · " + qsTr("%1/s, %2 left").arg(Theme.formatBytes(m.rate)).arg(Theme.formatDuration(m.eta))
            parts.push(s)
        }
        if (parts.length === 0) {
            if (!m.receive && !m.contribute) return qsTr("Not a member of this folder.")
            return m.online ? qsTr("Up to date") + (m.lastInSync > 0 ? " · " + qsTr("in sync since %1").arg(whenText(m.lastInSync)) : "")
                            : qsTr("Nothing left as far as this device knows")
                              + (m.lastInSync > 0 ? " · " + qsTr("last fully in sync %1").arg(whenText(m.lastInSync)) : "")
        }
        if (!m.online) parts.push(qsTr("waiting for it to connect"))
        return parts.join(" · ")
    }
    function currentText(m) {
        if (!m || !m.online || m.current.length === 0) return ""
        return (m.sending ? qsTr("Sending it %1").arg(m.current) : qsTr("Receiving %1").arg(m.current))
               + (m.sending && m.sendRate > 0 ? " · " + qsTr("%1/s").arg(Theme.formatBytes(m.sendRate)) : "")
    }

    Text {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.textSecondary
        font.pixelSize: Theme.smallFontSize
        text: qsTr("Send: the device gets this folder and your changes. Accept: you take its changes. A device that only gets sent the folder keeps a copy without changing yours. A device takes part once it proved its identity and is granted Offer sync or Receive sync here. Pausing a device keeps its grants but stops both directions for now.")
    }

    Repeater {
        model: view.stableMembers
        delegate: Rectangle {
            id: member
            required property var modelData
            // Live values for this row, read from the current list.
            readonly property var live: view.memberOf(modelData.key) || modelData
            Layout.fillWidth: true
            implicitHeight: memberColumn.implicitHeight + 20
            radius: Theme.radius
            color: Theme.panel
            border.color: Theme.border

            ColumnLayout {
                id: memberColumn
                anchors.fill: parent
                anchors.margins: 10
                spacing: 4

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    Avatar {
                        name: member.modelData.name
                        seed: member.modelData.key
                        size: 26
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        RowLayout {
                            spacing: 6
                            Text {
                                text: member.modelData.name
                                textFormat: Text.PlainText
                                color: Theme.text
                                font.bold: true
                            }
                            StatusDot { color: member.live.online ? Theme.success : Theme.textDisabled }
                            Text {
                                text: member.live.online ? qsTr("online")
                                      : member.live.lastSeen > 0 ? qsTr("last seen %1").arg(view.whenText(member.live.lastSeen))
                                      : qsTr("not seen yet")
                                color: Theme.textSecondary
                                font.pixelSize: Theme.smallFontSize
                            }
                        }
                        Text {
                            text: member.modelData.fingerprint
                            color: Theme.textDisabled
                            font.pixelSize: Theme.smallFontSize
                        }
                    }
                    Item { Layout.fillWidth: true }
                    UIconButton {
                        visible: member.modelData.receive || member.modelData.contribute
                        glyph: member.modelData.paused ? "play" : "pause"
                        tip: member.modelData.paused ? qsTr("Resume syncing with this device") : qsTr("Pause syncing with this device")
                        onClicked: App.setSyncMemberPaused(view.folderId, member.modelData.key, !member.modelData.paused)
                    }
                    UCheckBox {
                        text: qsTr("Send")
                        checked: member.modelData.receive
                        onToggled: App.setSyncMember(view.folderId, member.modelData.key, checked, member.modelData.contribute)
                    }
                    UCheckBox {
                        text: qsTr("Accept")
                        checked: member.modelData.contribute
                        onToggled: App.setSyncMember(view.folderId, member.modelData.key, member.modelData.receive, checked)
                    }
                }
                Text {
                    Layout.fillWidth: true
                    Layout.leftMargin: 36
                    visible: !member.modelData.syncs
                    wrapMode: Text.WordWrap
                    font.pixelSize: Theme.smallFontSize
                    color: Theme.warning
                    text: qsTr("Not syncing: this device is granted neither Offer sync nor Receive sync here.")
                }
                Text {
                    Layout.fillWidth: true
                    Layout.leftMargin: 36
                    wrapMode: Text.WordWrap
                    textFormat: Text.PlainText
                    font.pixelSize: Theme.smallFontSize
                    color: member.live.paused ? Theme.textSecondary
                           : (member.live.toSendFiles + member.live.toReceiveFiles + member.live.toSendChanges + member.live.toReceiveChanges) > 0 ? Theme.accent
                           : Theme.textSecondary
                    text: view.leftText(member.live)
                }
                Text {
                    Layout.fillWidth: true
                    Layout.leftMargin: 36
                    visible: text.length > 0
                    textFormat: Text.PlainText
                    elide: Text.ElideMiddle
                    font.pixelSize: Theme.smallFontSize
                    color: Theme.textSecondary
                    text: view.currentText(member.live)
                }
                Text {
                    Layout.fillWidth: true
                    Layout.leftMargin: 36
                    visible: member.live.skipped > 0
                    wrapMode: Text.WordWrap
                    font.pixelSize: Theme.smallFontSize
                    color: Theme.textSecondary
                    text: qsTr("Holds back %1 path(s) it skips or cannot take; those are left alone on every device.").arg(member.live.skipped)
                }
            }
        }
    }
    Text {
        Layout.fillWidth: true
        visible: view.members.length === 0
        wrapMode: Text.WordWrap
        color: Theme.textSecondary
        font.pixelSize: Theme.smallFontSize
        text: qsTr("Your devices appear here once they connect and prove their identity.")
    }
}
