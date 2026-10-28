import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import UConnect

// Folders kept the same on several devices. Left: this device's synced
// folders, the ones connected devices offer, and the transfers under way.
// Right: the selected folder, in views: what is going on (Overview), what
// is in it (Files), its rules (Rules), the devices it syncs with and where
// each is at (Devices), and what syncing set aside (Kept Files).
Window {
    id: win

    title: qsTr("Synced Folders")
    width: 1060
    height: 700
    minimumWidth: 800
    minimumHeight: 480
    color: Theme.canvas

    property string selectedId: ""
    readonly property var folders: App.syncFolders
    readonly property var current: {
        for (const f of folders)
            if (f.id === selectedId) return f
        return folders.length > 0 ? folders[0] : null
    }
    readonly property string currentId: current ? current.id : ""
    readonly property var openOffers: App.syncOffers.filter(o => !o.joined)
    readonly property var transfers: App.syncTransfers
    property var pendingJoin: null
    // The selected folder's members, rebuilt only when they change, not with
    // every bit of progress (which would recreate the checkboxes under the mouse).
    property var members: []
    readonly property string membersSignature: current ? JSON.stringify(current.members) : ""
    onMembersSignatureChanged: members = current ? current.members : []
    readonly property var memberNames: {
        const names = {}
        for (const m of members) names[m.key] = m.name
        return names
    }
    property string pendingPath: ""
    // Which view of the folder is shown: 0 overview, 1 files, 2 rules, 3 devices, 4 kept.
    property int view: 0
    readonly property var views: [
        { label: qsTr("Overview"), icon: "info" },
        { label: qsTr("Files"), icon: "list" },
        { label: qsTr("Rules"), icon: "rules" },
        { label: qsTr("Devices"), icon: "devices" },
        { label: qsTr("Kept Files"), icon: "restore" }
    ]

    function stateText(f) {
        if (!f) return ""
        if (f.state === "paused") return qsTr("Paused")
        if (f.state === "missing") return qsTr("The folder is missing on this device")
        const conflicts = f.conflicts ? f.conflicts.length : 0
        const syncing = f.state === "syncing"
                      ? qsTr("Fetching %1 file(s), %2").arg(f.needFiles).arg(Theme.formatBytes(f.needBytes))
                      : f.state === "scanning" ? qsTr("Looking for changes...") : qsTr("Up to date")
        const parts = [syncing]
        if (conflicts > 0) parts.push(qsTr("%1 conflict(s)").arg(conflicts))
        if (f.pendingDeleteCount > 0) parts.push(qsTr("%1 delete(s) waiting for you").arg(f.pendingDeleteCount))
        return parts.join(" · ")
    }
    function stateColor(f) {
        if (!f) return Theme.textSecondary
        if ((f.conflicts && f.conflicts.length > 0) || f.pendingDeleteCount > 0) return Theme.warning
        if (f.state === "missing") return Theme.error
        if (f.state === "syncing" || f.state === "scanning") return Theme.accent
        if (f.state === "paused") return Theme.textSecondary
        return Theme.success
    }
    function summaryText(f) {
        if (!f || f.files === undefined) return ""
        let s = qsTr("%1 file(s) in %2 folder(s), %3").arg(f.files).arg(f.dirs).arg(Theme.formatBytes(f.bytes))
        if (f.free >= 0) s += " · " + qsTr("%1 free on this disk").arg(Theme.formatBytes(f.free))
        return s
    }
    function candidateText(c) {
        if (c.deleted) return qsTr("deleted")
        if (c.dir) return qsTr("a folder")
        return Theme.formatBytes(c.size) + ", " + Qt.formatDateTime(new Date(c.mtime), "yyyy-MM-dd hh:mm")
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

            // --- left: folders, offers and transfers ---------------------------------
            Rectangle {
                Layout.preferredWidth: 290
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

                    // Transfers across every folder.
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.topMargin: 6
                        implicitHeight: 1
                        color: Theme.divider
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        UIcon {
                            name: "swap"
                            size: 14
                            color: Theme.textSecondary
                        }
                        Text {
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            color: Theme.textSecondary
                            font.pixelSize: Theme.smallFontSize
                            text: {
                                const t = win.transfers
                                if (!t || !t.active || t.active.length === 0)
                                    return t && t.queued > 0 ? qsTr("%1 file(s) waiting to be fetched").arg(t.queued) : qsTr("No transfers")
                                let s = qsTr("%1 transfer(s)").arg(t.active.length)
                                if (t.queued > 0) s += " · " + qsTr("%1 queued").arg(t.queued)
                                if (t.rate > 0) s += " · " + qsTr("%1/s").arg(Theme.formatBytes(t.rate))
                                return s
                            }
                        }
                        ULink {
                            visible: win.transfers && win.transfers.active && win.transfers.active.length > 0
                            text: transferList.visible ? qsTr("Hide") : qsTr("Show")
                            font.pixelSize: Theme.smallFontSize
                            onClicked: transferList.visible = !transferList.visible
                        }
                    }
                    ListView {
                        id: transferList
                        objectName: "syncTransferList"
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.min(160, contentHeight)
                        visible: false
                        clip: true
                        model: win.transfers && win.transfers.active ? win.transfers.active : []
                        boundsBehavior: Flickable.StopAtBounds
                        delegate: ColumnLayout {
                            required property var modelData
                            width: ListView.view.width
                            spacing: 2
                            Text {
                                Layout.fillWidth: true
                                text: modelData.path
                                textFormat: Text.PlainText
                                elide: Text.ElideMiddle
                                color: Theme.text
                                font.pixelSize: Theme.smallFontSize
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 6
                                Rectangle {
                                    Layout.fillWidth: true
                                    implicitHeight: 5
                                    radius: 2.5
                                    color: Theme.pressed
                                    Rectangle {
                                        width: parent.width * (modelData.size > 0 ? Math.min(1, modelData.done / modelData.size) : 0)
                                        height: parent.height
                                        radius: 2.5
                                        color: Theme.accent
                                    }
                                }
                                Text {
                                    text: qsTr("%1 of %2 · from %3").arg(Theme.formatBytes(modelData.done)).arg(Theme.formatBytes(modelData.size)).arg(modelData.device)
                                    textFormat: Text.PlainText
                                    color: Theme.textSecondary
                                    font.pixelSize: 10
                                }
                            }
                            Item { implicitHeight: 2 }
                        }
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
                    text: qsTr("A synced folder stays the same on every device it is shared with: a change on any of them reaches the others. Create one, or join one another of your devices offers you.")
                }

                ColumnLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 16
                    anchors.rightMargin: 16
                    visible: win.current !== null
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
                            text: qsTr("Kept Files")
                            onClicked: win.view = 4
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
                        Layout.fillWidth: true
                        spacing: 6
                        StatusDot { color: win.stateColor(win.current) }
                        Text {
                            text: win.stateText(win.current)
                            color: Theme.textSecondary
                        }
                        Text {
                            Layout.fillWidth: true
                            visible: text.length > 0
                            text: win.current && win.current.files !== undefined ? " · " + win.summaryText(win.current) : ""
                            elide: Text.ElideRight
                            color: Theme.textSecondary
                        }
                    }

                    // The views.
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: 4
                        spacing: 2
                        Repeater {
                            model: win.views
                            delegate: Rectangle {
                                required property var modelData
                                required property int index
                                implicitWidth: tabRow.implicitWidth + 20
                                implicitHeight: 28
                                radius: Theme.radius
                                color: win.view === index ? Theme.selection : tabArea.containsMouse ? Theme.hover : "transparent"
                                RowLayout {
                                    id: tabRow
                                    anchors.centerIn: parent
                                    spacing: 6
                                    UIcon {
                                        name: modelData.icon
                                        size: 14
                                        color: win.view === index ? Theme.text : Theme.textSecondary
                                    }
                                    Text {
                                        text: modelData.label
                                        color: win.view === index ? Theme.text : Theme.textSecondary
                                    }
                                    Rectangle {
                                        visible: index === 0 && win.current && (win.current.conflicts.length > 0 || win.current.pendingDeleteCount > 0)
                                        implicitWidth: 7
                                        implicitHeight: 7
                                        radius: 3.5
                                        color: Theme.warning
                                    }
                                }
                                MouseArea {
                                    id: tabArea
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    onClicked: win.view = index
                                }
                            }
                        }
                        Item { Layout.fillWidth: true }
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: 1
                        color: Theme.divider
                    }

                    StackLayout {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        currentIndex: win.view

                        // Overview: conflicts, deletes waiting, held names, events.
                        ScrollView {
                            id: scroll
                            contentWidth: availableWidth
                            clip: true

                            ColumnLayout {
                                width: scroll.availableWidth
                                spacing: 6

                                Help {
                                    Layout.topMargin: 6
                                    text: qsTr("Files that syncing deletes, or that you give up in a conflict, are kept in .synctrash inside the folder for 30 days, then moved to the Recycle Bin. Kept Files lists them and puts them back.")
                                }

                                // Deletes held back.
                                Banner {
                                    Layout.fillWidth: true
                                    Layout.topMargin: 8
                                    visible: win.current && win.current.pendingDeleteCount > 0
                                    kind: "error"
                                    text: qsTr("Another device removed %1 files at once; they are kept here until you choose. Delete them here too (they go to .synctrash), or keep them everywhere: they become changes of yours that the other devices take back.").arg(win.current ? win.current.pendingDeleteCount : 0)
                                    UButton {
                                        text: qsTr("Keep Everywhere")
                                        onClicked: App.confirmSyncDeletes(win.current.id, false)
                                    }
                                    UButton {
                                        text: qsTr("Delete Here Too")
                                        primary: true
                                        onClicked: App.confirmSyncDeletes(win.current.id, true)
                                    }
                                }
                                Repeater {
                                    model: win.current && win.current.pendingDeleteCount > 0 ? win.current.pendingDeletes.slice(0, 12) : []
                                    delegate: Text {
                                        required property var modelData
                                        Layout.fillWidth: true
                                        Layout.leftMargin: 12
                                        text: modelData
                                        textFormat: Text.PlainText
                                        elide: Text.ElideMiddle
                                        font.pixelSize: Theme.smallFontSize
                                        color: Theme.textSecondary
                                    }
                                }
                                Help {
                                    Layout.leftMargin: 12
                                    visible: win.current && win.current.pendingDeleteCount > 12
                                    text: qsTr("… and %1 more. The Files view, filtered to Pending, lists them all.").arg(win.current ? win.current.pendingDeleteCount - 12 : 0)
                                }

                                // Conflicts
                                Section {
                                    visible: win.current && win.current.conflicts.length > 0
                                    text: qsTr("Conflicts")
                                }
                                Help {
                                    visible: win.current && win.current.conflicts.length > 0
                                    text: qsTr("Devices changed these files without seeing each other's change. Whatever you choose reaches every device; a version you give up here is kept in .synctrash.")
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
                                            RowLayout {
                                                Layout.fillWidth: true
                                                Text {
                                                    Layout.fillWidth: true
                                                    text: conflict.modelData.path
                                                    textFormat: Text.PlainText
                                                    elide: Text.ElideMiddle
                                                    color: Theme.text
                                                    font.bold: true
                                                }
                                                UIconButton {
                                                    glyph: "open"
                                                    tip: qsTr("Open this device's version")
                                                    visible: conflict.modelData.candidates.some(c => c.mine && !c.deleted)
                                                    onClicked: App.openSyncPath(win.current.id, conflict.modelData.path)
                                                }
                                                UIconButton {
                                                    glyph: "history"
                                                    tip: qsTr("History")
                                                    onClicked: {
                                                        win.view = 1
                                                        filesView.showHistory(conflict.modelData.path)
                                                    }
                                                }
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
                                            // The versions side by side.
                                            Repeater {
                                                model: conflict.modelData.candidates
                                                delegate: RowLayout {
                                                    id: candidate
                                                    required property var modelData
                                                    Layout.fillWidth: true
                                                    spacing: 8
                                                    Avatar {
                                                        name: candidate.modelData.name
                                                        seed: candidate.modelData.key
                                                        size: 18
                                                    }
                                                    Text {
                                                        Layout.preferredWidth: 160
                                                        text: candidate.modelData.name
                                                        textFormat: Text.PlainText
                                                        elide: Text.ElideRight
                                                        color: Theme.text
                                                        font.bold: candidate.modelData.mine
                                                    }
                                                    Text {
                                                        Layout.fillWidth: true
                                                        text: win.candidateText(candidate.modelData)
                                                        textFormat: Text.PlainText
                                                        elide: Text.ElideRight
                                                        color: Theme.textSecondary
                                                        font.pixelSize: Theme.smallFontSize
                                                    }
                                                    UButton {
                                                        visible: candidate.modelData.mine
                                                        primary: conflict.modelData.involved
                                                        text: conflict.modelData.involved ? qsTr("Keep Mine") : qsTr("Keep This Version")
                                                        onClicked: App.resolveSyncConflict(win.current.id, conflict.modelData.path, "mine", "")
                                                    }
                                                    UButton {
                                                        visible: !candidate.modelData.mine && candidate.modelData.key.length > 0
                                                        text: qsTr("Take %1's").arg(candidate.modelData.name)
                                                        onClicked: App.resolveSyncConflict(win.current.id, conflict.modelData.path, "theirs", candidate.modelData.key)
                                                    }
                                                }
                                            }
                                            Flow {
                                                Layout.fillWidth: true
                                                spacing: 6
                                                UButton {
                                                    text: qsTr("Stop Syncing This File")
                                                    onClicked: App.resolveSyncConflict(win.current.id, conflict.modelData.path, "skip", "")
                                                }
                                            }
                                        }
                                    }
                                }

                                // Held back
                                Section {
                                    visible: win.current && win.current.held && win.current.held.length > 0
                                    text: qsTr("Not synced here")
                                }
                                Help {
                                    visible: win.current && win.current.held && win.current.held.length > 0
                                    text: qsTr("Synced names follow the strictest rules of every system, so any device can hold every file. These stay on this device, untouched, until the cause goes.")
                                }
                                Repeater {
                                    model: win.current && win.current.held ? win.current.held : []
                                    delegate: ColumnLayout {
                                        required property var modelData
                                        Layout.fillWidth: true
                                        spacing: 0
                                        Text {
                                            Layout.fillWidth: true
                                            text: modelData.path
                                            textFormat: Text.PlainText
                                            elide: Text.ElideMiddle
                                            color: Theme.text
                                        }
                                        Text {
                                            Layout.fillWidth: true
                                            text: modelData.text
                                            textFormat: Text.PlainText
                                            wrapMode: Text.WordWrap
                                            color: Theme.textSecondary
                                            font.pixelSize: Theme.smallFontSize
                                        }
                                    }
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

                        // Files
                        Item {
                            SyncFilesView {
                                id: filesView
                                anchors.fill: parent
                                anchors.topMargin: 8
                                anchors.bottomMargin: 12
                                folderId: win.currentId
                                memberNames: win.memberNames
                            }
                        }

                        // Rules
                        Item {
                            SyncRulesView {
                                anchors.fill: parent
                                anchors.topMargin: 8
                                anchors.bottomMargin: 12
                                folderId: win.currentId
                            }
                        }

                        // Devices
                        ScrollView {
                            id: devicesScroll
                            contentWidth: availableWidth
                            clip: true
                            SyncDevicesView {
                                width: devicesScroll.availableWidth
                                folderId: win.currentId
                                members: win.current ? win.current.members : []
                            }
                        }

                        // Kept files
                        Item {
                            SyncKeptView {
                                anchors.fill: parent
                                anchors.topMargin: 8
                                anchors.bottomMargin: 12
                                folderId: win.currentId
                            }
                        }
                    }
                }
            }
        }
    }
}
