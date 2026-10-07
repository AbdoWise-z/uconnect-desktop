import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import UConnect

// Folders kept the same on several devices. Left: this device's synced
// folders and the ones connected devices offer. Right: the selected folder's
// state, the devices it syncs with, and its conflicts.
Window {
    id: win

    title: qsTr("Synced Folders")
    width: 1000
    height: 660
    minimumWidth: 760
    minimumHeight: 440
    color: Theme.canvas

    property string selectedId: ""
    readonly property var folders: App.syncFolders
    readonly property var current: {
        for (const f of folders)
            if (f.id === selectedId) return f
        return folders.length > 0 ? folders[0] : null
    }
    readonly property var openOffers: App.syncOffers.filter(o => !o.joined)
    property var pendingJoin: null
    property string pendingPath: ""

    function stateText(f) {
        if (!f) return ""
        if (f.state === "paused") return qsTr("Paused")
        if (f.state === "missing") return qsTr("The folder is missing on this device")
        const conflicts = f.conflicts ? f.conflicts.length : 0
        const syncing = f.state === "syncing"
                      ? qsTr("Fetching %1 file(s), %2").arg(f.needFiles).arg(Theme.formatBytes(f.needBytes))
                      : f.state === "scanning" ? qsTr("Looking for changes...") : qsTr("Up to date")
        return conflicts > 0 ? syncing + " · " + qsTr("%1 conflict(s)").arg(conflicts) : syncing
    }
    function stateColor(f) {
        if (!f) return Theme.textSecondary
        if (f.conflicts && f.conflicts.length > 0) return Theme.warning
        if (f.state === "missing") return Theme.error
        if (f.state === "syncing" || f.state === "scanning") return Theme.accent
        if (f.state === "paused") return Theme.textSecondary
        return Theme.success
    }
    function show() {
        visible = true
        raise()
        requestActivate()
    }

    FolderDialog {
        id: newDialog
        title: qsTr("Choose a Folder to Sync")
        onAccepted: {
            win.pendingPath = selectedFolder
            const name = decodeURIComponent(String(selectedFolder).split("/").pop())
            prompt.pending = { create: true }
            prompt.ask(qsTr("New Synced Folder"),
                       qsTr("Give it a name the other devices will see. Then choose, below, which devices it syncs with."),
                       [{ id: "create", label: qsTr("Create"), primary: true }], name)
        }
    }
    FolderDialog {
        id: joinDialog
        title: qsTr("Where Should the Folder Be on This Device?")
        onAccepted: {
            if (win.pendingJoin && App.joinSyncFolder(win.pendingJoin.key, win.pendingJoin.id,
                                                      win.pendingJoin.label, selectedFolder))
                win.selectedId = win.pendingJoin.id
            win.pendingJoin = null
        }
    }

    component Section: Text {
        Layout.topMargin: 14
        color: Theme.text
        font.bold: true
    }
    component Help: Text {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.textSecondary
        font.pixelSize: Theme.smallFontSize
    }

    Item {
        anchors.fill: parent

        PromptDialog {
            id: prompt
            property var pending: ({})
            onChosen: (choiceId, value) => {
                if (choiceId === "create") {
                    const id = App.createSyncFolder(win.pendingPath, value)
                    if (id.length > 0) win.selectedId = id
                } else if (choiceId === "leave") {
                    App.leaveSyncFolder(pending.id)
                    win.selectedId = ""
                }
                pending = {}
            }
        }

        RowLayout {
            anchors.fill: parent
            spacing: 0

            // --- left: folders and offers ----------------------------------------
            Rectangle {
                Layout.preferredWidth: 280
                Layout.fillHeight: true
                color: Theme.panel

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 10
                    spacing: 6

                    Text {
                        text: qsTr("Synced folders")
                        color: Theme.textSecondary
                        font.pixelSize: Theme.smallFontSize
                    }
                    ListView {
                        id: folderList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: win.folders
                        spacing: 2
                        delegate: Rectangle {
                            required property var modelData
                            width: ListView.view.width
                            height: 46
                            radius: Theme.radius
                            color: win.current && win.current.id === modelData.id ? Theme.selection
                                   : rowArea.containsMouse ? Theme.hover : "transparent"
                            ColumnLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 10
                                anchors.rightMargin: 10
                                spacing: 2
                                Item { Layout.fillHeight: true }
                                Text {
                                    Layout.fillWidth: true
                                    text: modelData.label
                                    textFormat: Text.PlainText
                                    elide: Text.ElideRight
                                    color: Theme.text
                                }
                                RowLayout {
                                    spacing: 6
                                    StatusDot { color: win.stateColor(modelData) }
                                    Text {
                                        Layout.fillWidth: true
                                        text: win.stateText(modelData)
                                        elide: Text.ElideRight
                                        color: Theme.textSecondary
                                        font.pixelSize: Theme.smallFontSize
                                    }
                                }
                                Item { Layout.fillHeight: true }
                            }
                            MouseArea {
                                id: rowArea
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: win.selectedId = modelData.id
                            }
                        }
                        Text {
                            anchors.centerIn: parent
                            width: parent.width - 20
                            visible: folderList.count === 0
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.WordWrap
                            color: Theme.textSecondary
                            text: qsTr("No synced folders yet.")
                        }
                    }

                    Text {
                        visible: win.openOffers.length > 0
                        Layout.topMargin: 8
                        text: qsTr("Offered to you")
                        color: Theme.textSecondary
                        font.pixelSize: Theme.smallFontSize
                    }
                    Repeater {
                        model: win.openOffers
                        delegate: RowLayout {
                            required property var modelData
                            Layout.fillWidth: true
                            spacing: 6
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 0
                                Text {
                                    Layout.fillWidth: true
                                    text: modelData.label
                                    textFormat: Text.PlainText
                                    elide: Text.ElideRight
                                    color: Theme.text
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: qsTr("from %1").arg(modelData.device)
                                    textFormat: Text.PlainText
                                    elide: Text.ElideRight
                                    color: Theme.textSecondary
                                    font.pixelSize: Theme.smallFontSize
                                }
                            }
                            UButton {
                                text: qsTr("Join...")
                                onClicked: {
                                    win.pendingJoin = modelData
                                    joinDialog.open()
                                }
                            }
                        }
                    }

                    UButton {
                        Layout.fillWidth: true
                        Layout.topMargin: 6
                        text: qsTr("New Synced Folder...")
                        onClicked: newDialog.open()
                    }
                }
            }
            Rectangle {
                Layout.fillHeight: true
                implicitWidth: 1
                color: Theme.divider
            }

            // --- right: the selected folder ----------------------------------------
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                Help {
                    anchors.centerIn: parent
                    width: Math.min(460, parent.width - 60)
                    visible: !win.current
                    horizontalAlignment: Text.AlignHCenter
                    font.pixelSize: Theme.fontSize
                    text: qsTr("A synced folder stays the same on every device it is shared with: a change on any of them reaches the others. Create one, or join one a trusted device offers you.")
                }

                ScrollView {
                    id: scroll
                    anchors.fill: parent
                    visible: win.current !== null
                    contentWidth: availableWidth

                    ColumnLayout {
                        width: scroll.availableWidth - 32
                        x: 16
                        spacing: 6

                        // Header
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.topMargin: 14
                            spacing: 8
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Text {
                                    Layout.fillWidth: true
                                    text: win.current ? win.current.label : ""
                                    textFormat: Text.PlainText
                                    elide: Text.ElideRight
                                    color: Theme.text
                                    font.pixelSize: 16
                                    font.bold: true
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: win.current ? win.current.path : ""
                                    textFormat: Text.PlainText
                                    elide: Text.ElideMiddle
                                    color: Theme.textSecondary
                                    font.pixelSize: Theme.smallFontSize
                                }
                            }
                            UButton {
                                text: qsTr("Open")
                                onClicked: App.openSyncFolder(win.current.id)
                            }
                            UButton {
                                text: qsTr("Rescan")
                                enabled: win.current && !win.current.paused
                                onClicked: App.rescanSyncFolder(win.current.id)
                            }
                            UButton {
                                text: win.current && win.current.paused ? qsTr("Resume") : qsTr("Pause")
                                onClicked: App.setSyncPaused(win.current.id, !win.current.paused)
                            }
                            UButton {
                                text: qsTr("Leave...")
                                onClicked: {
                                    prompt.pending = { id: win.current.id }
                                    prompt.ask(qsTr("Stop syncing “%1”?").arg(win.current.label),
                                               qsTr("The files stay in the folder on this device. Other devices keep syncing it among themselves."),
                                               [{ id: "leave", label: qsTr("Stop Syncing"), primary: true }])
                                }
                            }
                        }
                        RowLayout {
                            spacing: 6
                            StatusDot { color: win.stateColor(win.current) }
                            Text {
                                text: win.stateText(win.current)
                                color: Theme.textSecondary
                            }
                            Text {
                                visible: win.current !== null && win.current.files !== undefined
                                text: win.current && win.current.files !== undefined
                                      ? " · " + qsTr("%1 files, %2").arg(win.current.files).arg(Theme.formatBytes(win.current.bytes))
                                      : ""
                                color: Theme.textSecondary
                            }
                        }

                        // Conflicts
                        Section {
                            visible: win.current && win.current.conflicts.length > 0
                            text: qsTr("Conflicts")
                        }
                        Help {
                            visible: win.current && win.current.conflicts.length > 0
                            text: qsTr("Devices changed these files without seeing each other's change. Whatever you choose reaches every device; files you set aside go to the Recycle Bin.")
                        }
                        Repeater {
                            model: win.current ? win.current.conflicts : []
                            delegate: Rectangle {
                                id: conflict
                                required property var modelData
                                Layout.fillWidth: true
                                implicitHeight: conflictColumn.implicitHeight + 20
                                radius: Theme.radius
                                color: Theme.panel
                                border.color: modelData.involved ? Theme.warning : Theme.border

                                ColumnLayout {
                                    id: conflictColumn
                                    anchors.fill: parent
                                    anchors.margins: 10
                                    spacing: 6
                                    Text {
                                        Layout.fillWidth: true
                                        text: conflict.modelData.path
                                        textFormat: Text.PlainText
                                        elide: Text.ElideMiddle
                                        color: Theme.text
                                        font.bold: true
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        textFormat: Text.PlainText
                                        color: Theme.textSecondary
                                        text: conflict.modelData.involved
                                              ? qsTr("You changed it here while another device changed it too.")
                                              : conflict.modelData.waiting
                                              ? qsTr("Other devices changed it differently; waiting for one of them to connect.")
                                              : conflict.modelData.following.length > 0
                                              ? qsTr("Other devices changed it differently. Following %1's version, the device connected longest.").arg(conflict.modelData.followingName)
                                              : qsTr("Other devices changed it differently. Keeping the version this device has.")
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        textFormat: Text.PlainText
                                        color: Theme.textSecondary
                                        font.pixelSize: Theme.smallFontSize
                                        text: conflict.modelData.candidates.map(c =>
                                            c.name + ": " + (c.deleted ? qsTr("deleted") : c.dir ? qsTr("a folder")
                                                             : Theme.formatBytes(c.size) + ", "
                                                               + Qt.formatDateTime(new Date(c.mtime), "yyyy-MM-dd hh:mm"))).join("   ·   ")
                                    }
                                    Flow {
                                        Layout.fillWidth: true
                                        spacing: 6
                                        UButton {
                                            visible: conflict.modelData.candidates.some(c => c.mine)
                                            primary: conflict.modelData.involved
                                            text: conflict.modelData.involved ? qsTr("Keep Mine") : qsTr("Keep This Version")
                                            onClicked: App.resolveSyncConflict(win.current.id, conflict.modelData.path, "mine", "")
                                        }
                                        Repeater {
                                            model: conflict.modelData.candidates.filter(c => !c.mine && c.key.length > 0)
                                            delegate: UButton {
                                                required property var modelData
                                                text: qsTr("Take %1's").arg(modelData.name)
                                                onClicked: App.resolveSyncConflict(win.current.id, conflict.modelData.path,
                                                                                   "theirs", modelData.key)
                                            }
                                        }
                                        UButton {
                                            text: qsTr("Stop Syncing This File")
                                            onClicked: App.resolveSyncConflict(win.current.id, conflict.modelData.path, "skip", "")
                                        }
                                    }
                                }
                            }
                        }

                        // Devices
                        Section { text: qsTr("Devices") }
                        Help {
                            text: qsTr("Send: the device gets this folder and your changes. Accept: you take its changes. A device that only gets sent the folder keeps a copy without changing yours. Only devices you trust can take part.")
                        }
                        Repeater {
                            model: win.current ? win.current.members : []
                            delegate: RowLayout {
                                id: member
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: 10
                                Avatar {
                                    name: member.modelData.name
                                    seed: member.modelData.key
                                    size: 22
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
                                        }
                                        StatusDot {
                                            color: member.modelData.online ? Theme.success : Theme.textDisabled
                                        }
                                        Text {
                                            visible: !member.modelData.trusted
                                            text: qsTr("not trusted")
                                            color: Theme.warning
                                            font.pixelSize: Theme.smallFontSize
                                        }
                                    }
                                    Text {
                                        text: member.modelData.fingerprint
                                        color: Theme.textDisabled
                                        font.pixelSize: Theme.smallFontSize
                                    }
                                }
                                UCheckBox {
                                    text: qsTr("Send")
                                    checked: member.modelData.receive
                                    onToggled: App.setSyncMember(win.current.id, member.modelData.key, checked,
                                                                 member.modelData.contribute)
                                }
                                UCheckBox {
                                    text: qsTr("Accept")
                                    checked: member.modelData.contribute
                                    onToggled: App.setSyncMember(win.current.id, member.modelData.key,
                                                                 member.modelData.receive, checked)
                                }
                            }
                        }
                        Help {
                            visible: win.current && win.current.members.length === 0
                            text: qsTr("Trust a device first (from its context menu in the device list) to sync with it.")
                        }

                        // Events
                        RowLayout {
                            Layout.fillWidth: true
                            Section {
                                Layout.fillWidth: true
                                text: qsTr("Recent events")
                            }
                            ULink {
                                Layout.topMargin: 14
                                visible: App.syncEvents.length > 0
                                text: qsTr("Clear")
                                font.pixelSize: Theme.smallFontSize
                                onClicked: App.clearSyncEvents()
                            }
                        }
                        Repeater {
                            model: win.current ? App.syncEvents.filter(e => e.folderId === win.current.id).slice(0, 50) : []
                            delegate: Text {
                                required property var modelData
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                textFormat: Text.PlainText
                                color: Theme.textSecondary
                                font.pixelSize: Theme.smallFontSize
                                text: Qt.formatDateTime(new Date(modelData.time), "hh:mm:ss") + "  " + modelData.text
                            }
                        }
                        Help {
                            visible: win.current && App.syncEvents.filter(e => e.folderId === win.current.id).length === 0
                            text: qsTr("Nothing to report.")
                        }
                        Item { implicitHeight: 16 }
                    }
                }
            }
        }
    }
}
