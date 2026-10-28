import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import UConnect

// The files of a synced folder as sync sees them: a tree with each path's
// state (synced, to send, to receive, transferring, in conflict, ignored,
// skipped, held back, blocked), a search, a filter, and what can be done
// with a path (ignore everywhere, skip here, open, show, history).
Item {
    id: view

    required property string folderId
    // What the folder can hold: false while it is missing or paused.
    property bool enabledForFolder: true
    // [{path, name, dir, size, mtime, state, progress, detail, version}] from App.
    property var files: []
    property bool loading: false
    property string query: ""
    property int filterIndex: 0  // 0 all, 1 pending, 2 problems, 3 left out
    // Folders opened in the tree, by path; the top level is always open.
    property var expanded: ({})
    readonly property var rows: computeRows()
    // The path whose history is shown, and what came back for it.
    property string infoPath: ""
    property var info: ({})

    readonly property var filters: [qsTr("All"), qsTr("Pending"), qsTr("Problems"), qsTr("Ignored or skipped")]

    function stateLabel(f) {
        switch (f.state) {
        case "synced": return qsTr("Synced")
        case "send": return f.detail.length > 0 ? qsTr("Waiting to send to %1 device(s)").arg(f.detail) : qsTr("Waiting to send")
        case "receive": return f.detail === "waiting" ? qsTr("Waiting for a device that has it")
                             : f.detail === "delete" ? qsTr("To be deleted here") : qsTr("Waiting to receive")
        case "transferring": return qsTr("Receiving, %1%").arg(Math.round(Math.max(0, f.progress) * 100))
        case "conflict": return qsTr("In conflict")
        case "ignored": return f.detail.length > 0 ? qsTr("Ignored everywhere (%1)").arg(f.detail) : qsTr("Ignored everywhere")
        case "skipped": return f.detail.length > 0 ? qsTr("Skipped on this device (%1)").arg(f.detail) : qsTr("Skipped on this device")
        case "held": return f.detail.startsWith("clash:") ? qsTr("Not synced: same name as %1 in another case").arg(f.detail.substring(6))
                          : qsTr("Not synced: its name cannot be used on every device")
        case "blocked": return qsTr("Not applied here (%1)").arg(f.detail.replace("_", " "))
        }
        return f.state
    }
    function stateIcon(state) {
        switch (state) {
        case "synced": return "check"
        case "send": return "upload"
        case "receive": return "download"
        case "transferring": return "download"
        case "conflict": return "warning"
        case "ignored": return "block"
        case "skipped": return "block"
        case "held": return "error"
        case "blocked": return "error"
        }
        return "info"
    }
    function stateColor(state) {
        switch (state) {
        case "synced": return Theme.success
        case "send":
        case "receive":
        case "transferring": return Theme.accent
        case "conflict":
        case "held":
        case "blocked": return Theme.warning
        }
        return Theme.textDisabled
    }
    function pending(f) {
        return f.state === "send" || f.state === "receive" || f.state === "transferring" || f.state === "conflict"
               || f.state === "blocked"
    }
    function problem(f) {
        return f.state === "conflict" || f.state === "held" || f.state === "blocked"
               || (f.state === "receive" && f.detail === "waiting")
    }
    function leftOut(f) { return f.state === "ignored" || f.state === "skipped" }
    function passes(f) {
        if (filterIndex === 1 && !pending(f)) return false
        if (filterIndex === 2 && !problem(f)) return false
        if (filterIndex === 3 && !leftOut(f)) return false
        return query.length === 0 || f.path.toLowerCase().includes(query.toLowerCase())
    }
    function parentOf(path) {
        const slash = path.lastIndexOf("/")
        return slash < 0 ? "" : path.substring(0, slash)
    }
    function depthOf(path) { return path.split("/").length - 1 }

    // Flat list of what is shown. Searching or filtering lists matches by
    // full path; otherwise the tree, folders first, opened ones expanded.
    function computeRows() {
        const flat = query.length > 0 || filterIndex > 0
        const byPath = {}
        for (const f of files) byPath[f.path] = f
        let out = []
        if (flat) {
            out = files.filter(passes).map(f => Object.assign({ depth: 0, flat: true, open: false }, f))
        } else {
            const children = {}
            for (const f of files) {
                const parent = parentOf(f.path)
                if (!(parent in children)) children[parent] = []
                children[parent].push(f)
            }
            const walk = (parent, depth) => {
                const list = (children[parent] || []).slice().sort((a, b) => {
                    if (a.dir !== b.dir) return a.dir ? -1 : 1
                    return a.name.localeCompare(b.name, undefined, { sensitivity: "base" })
                })
                for (const f of list) {
                    const open = f.dir && expanded[f.path] === true
                    out.push(Object.assign({ depth: depth, flat: false, open: open, hasChildren: (children[f.path] || []).length > 0 }, f))
                    if (open) walk(f.path, depth + 1)
                }
            }
            walk("", 0)
        }
        return out
    }
    function toggle(path) {
        const next = Object.assign({}, expanded)
        if (next[path]) delete next[path]
        else next[path] = true
        expanded = next
    }
    function reload() {
        if (folderId.length === 0) return
        loading = true
        App.loadSyncFiles(folderId)
    }
    function showHistory(path) {
        infoPath = path
        info = {}
        App.loadSyncFileInfo(folderId, path)
        historyPopup.open()
    }
    function versionLines(version) {
        if (!version) return []
        return Object.keys(version).map(id => ({ name: App.syncMemberName(id), count: version[id] }))
    }
    // Events about this path, from the window-wide list.
    function eventsFor(path) {
        return App.syncEvents.filter(e => e.folderId === folderId && e.path === path).slice(0, 20)
    }

    Component.onCompleted: reload()
    onFolderIdChanged: {
        expanded = {}
        reload()
    }
    onVisibleChanged: if (visible) reload()

    Connections {
        target: App
        function onSyncFilesLoaded(id, list) {
            if (id !== view.folderId) return
            view.files = list
            view.loading = false
        }
        function onSyncFileInfoLoaded(id, path, data) {
            if (id === view.folderId && path === view.infoPath) view.info = data
        }
        // The folder changed: list again, but not more than twice a second.
        function onSyncChanged() { if (view.visible) refresh.restart() }
    }
    Timer {
        id: refresh
        interval: 600
        onTriggered: view.reload()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 6

        RowLayout {
            Layout.fillWidth: true
            spacing: 6
            UTextField {
                id: search
                Layout.fillWidth: true
                placeholderText: qsTr("Search files")
                onTextChanged: view.query = text.trim()
            }
            UIcon {
                name: "filter"
                size: 16
                color: Theme.textSecondary
            }
            UComboBox {
                id: filterBox
                model: view.filters
                currentIndex: view.filterIndex
                onActivated: view.filterIndex = currentIndex
            }
            UIconButton {
                glyph: "refresh"
                tip: qsTr("List again")
                onClicked: view.reload()
            }
        }
        Text {
            Layout.fillWidth: true
            font.pixelSize: Theme.smallFontSize
            color: Theme.textSecondary
            textFormat: Text.PlainText
            text: {
                const total = view.files.filter(f => !f.dir).length
                const pendingCount = view.files.filter(f => !f.dir && view.pending(f)).length
                const problems = view.files.filter(f => view.problem(f)).length
                let s = qsTr("%1 file(s)").arg(total)
                if (pendingCount > 0) s += " · " + qsTr("%1 pending").arg(pendingCount)
                if (problems > 0) s += " · " + qsTr("%1 problem(s)").arg(problems)
                if (view.rows.length !== view.files.length && (view.query.length > 0 || view.filterIndex > 0))
                    s += " · " + qsTr("%1 shown").arg(view.rows.length)
                return s
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: Theme.radius
            color: Theme.panel
            border.color: Theme.border

            ListView {
                id: list
                objectName: "syncFileList"
                anchors.fill: parent
                anchors.margins: 1
                clip: true
                model: view.rows
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}

                delegate: Rectangle {
                    id: row
                    required property var modelData
                    required property int index
                    width: ListView.view.width
                    height: 30
                    color: rowArea.containsMouse ? Theme.hover : "transparent"

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 8 + row.modelData.depth * 18
                        anchors.rightMargin: 6
                        spacing: 6

                        // A chevron for folders in the tree.
                        Item {
                            implicitWidth: 14
                            implicitHeight: 14
                            UIcon {
                                anchors.centerIn: parent
                                visible: row.modelData.dir && !row.modelData.flat
                                name: row.modelData.open ? "expand-more" : "chevron-right"
                                size: 14
                                color: Theme.textSecondary
                            }
                        }
                        UIcon {
                            name: row.modelData.dir ? "folder" : "file"
                            size: 15
                            color: row.modelData.dir ? Theme.warning : Theme.textSecondary
                        }
                        Text {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 80
                            text: row.modelData.flat ? row.modelData.path : row.modelData.name
                            textFormat: Text.PlainText
                            elide: Text.ElideMiddle
                            color: view.leftOut(row.modelData) ? Theme.textSecondary : Theme.text
                        }
                        // Progress while a file is being received.
                        Rectangle {
                            visible: row.modelData.state === "transferring"
                            implicitWidth: 90
                            implicitHeight: 6
                            radius: 3
                            color: Theme.pressed
                            Rectangle {
                                width: parent.width * Math.max(0, Math.min(1, row.modelData.progress))
                                height: parent.height
                                radius: 3
                                color: Theme.accent
                            }
                        }
                        UIcon {
                            name: view.stateIcon(row.modelData.state)
                            size: 14
                            color: view.stateColor(row.modelData.state)
                        }
                        Text {
                            Layout.preferredWidth: Math.min(implicitWidth, 300)
                            text: view.stateLabel(row.modelData)
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            font.pixelSize: Theme.smallFontSize
                            color: view.stateColor(row.modelData.state)
                        }
                        Text {
                            Layout.preferredWidth: 64
                            horizontalAlignment: Text.AlignRight
                            text: row.modelData.dir ? "" : Theme.formatBytes(row.modelData.size)
                            font.pixelSize: Theme.smallFontSize
                            color: Theme.textSecondary
                        }
                        UIconButton {
                            glyph: "more"
                            tip: qsTr("Actions")
                            iconSize: 15
                            onClicked: {
                                rowMenu.target = row.modelData
                                rowMenu.popup()
                            }
                        }
                    }
                    MouseArea {
                        id: rowArea
                        anchors.fill: parent
                        anchors.rightMargin: 36
                        hoverEnabled: true
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        onClicked: (mouse) => {
                            if (mouse.button === Qt.RightButton) {
                                rowMenu.target = row.modelData
                                rowMenu.popup()
                            } else if (row.modelData.dir && !row.modelData.flat) {
                                view.toggle(row.modelData.path)
                            }
                        }
                        onDoubleClicked: (mouse) => {
                            if (mouse.button === Qt.LeftButton && !row.modelData.dir) App.openSyncPath(view.folderId, row.modelData.path)
                        }
                    }
                }

                Text {
                    anchors.centerIn: parent
                    width: parent.width - 40
                    visible: list.count === 0
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    color: Theme.textSecondary
                    text: view.loading ? qsTr("Listing...")
                          : view.files.length === 0 ? qsTr("Nothing in this folder yet.")
                          : qsTr("Nothing matches.")
                }
            }
        }
        Text {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.smallFontSize
            color: Theme.textSecondary
            text: qsTr("Double-click a file to open it. Ignoring a path (everywhere) or skipping it (on this device only) adds a line to the folder's rules; copies already on disk stay.")
        }
    }

    UMenu {
        id: rowMenu
        property var target: ({})
        UMenuItem {
            text: qsTr("Open")
            glyph: "open"
            enabled: rowMenu.target && rowMenu.target.state !== "receive" && rowMenu.target.state !== "transferring"
            onTriggered: App.openSyncPath(view.folderId, rowMenu.target.path)
        }
        UMenuItem {
            text: qsTr("Show in Folder")
            glyph: "folder"
            onTriggered: App.showSyncPath(view.folderId, rowMenu.target.path)
        }
        UMenuItem {
            text: qsTr("History...")
            glyph: "history"
            onTriggered: view.showHistory(rowMenu.target.path)
        }
        UMenuSeparator {}
        UMenuItem {
            text: qsTr("Ignore Everywhere")
            glyph: "block"
            enabled: rowMenu.target && rowMenu.target.state !== "ignored"
            onTriggered: {
                ignoreAsk.path = rowMenu.target.path
                ignoreAsk.open()
            }
        }
        UMenuItem {
            text: qsTr("Skip on This Device")
            glyph: "block"
            enabled: rowMenu.target && rowMenu.target.state !== "skipped" && rowMenu.target.state !== "ignored"
            onTriggered: App.addSyncRule(view.folderId, true, rowMenu.target.path)
        }
    }

    // Ignoring reaches every member: say what it means first.
    Popup {
        id: ignoreAsk
        property string path: ""
        anchors.centerIn: Overlay.overlay
        width: Math.min(440, (parent ? parent.width : 440) - 48)
        modal: true
        padding: 20
        background: Rectangle {
            color: Theme.panel
            border.color: Theme.popupBorder
            radius: 8
        }
        ColumnLayout {
            width: parent.width
            spacing: 12
            Text {
                Layout.fillWidth: true
                text: qsTr("Ignore %1 everywhere?").arg(ignoreAsk.path)
                textFormat: Text.PlainText
                wrapMode: Text.Wrap
                color: Theme.text
                font.bold: true
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.textSecondary
                text: qsTr(".syncignore syncs to every device in the folder, so it stops syncing there too. It counts as deleted for sync, but copies already on disk stay where they are.")
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                spacing: 8
                UButton {
                    text: qsTr("Cancel")
                    onClicked: ignoreAsk.close()
                }
                UButton {
                    text: qsTr("Ignore")
                    primary: true
                    onClicked: {
                        App.addSyncRule(view.folderId, false, ignoreAsk.path)
                        ignoreAsk.close()
                    }
                }
            }
        }
    }

    // One path's history: who changed it and when, where it is held, and
    // the versions kept in .synctrash.
    Popup {
        id: historyPopup
        anchors.centerIn: Overlay.overlay
        width: Math.min(560, (parent ? parent.width : 560) - 48)
        height: Math.min(520, (parent ? parent.height : 520) - 48)
        modal: true
        padding: 20
        background: Rectangle {
            color: Theme.panel
            border.color: Theme.popupBorder
            radius: 8
        }
        contentItem: ColumnLayout {
            spacing: 10
            RowLayout {
                Layout.fillWidth: true
                Text {
                    Layout.fillWidth: true
                    text: view.infoPath
                    textFormat: Text.PlainText
                    elide: Text.ElideMiddle
                    color: Theme.text
                    font.bold: true
                }
                UIconButton {
                    glyph: "close"
                    tip: qsTr("Close")
                    onClicked: historyPopup.close()
                }
            }
            ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                contentWidth: availableWidth
                clip: true
                ColumnLayout {
                    width: parent.width
                    spacing: 8
                    Text {
                        Layout.fillWidth: true
                        visible: view.info.state !== undefined
                        wrapMode: Text.WordWrap
                        color: Theme.textSecondary
                        text: view.info.state !== undefined
                              ? view.stateLabel(view.info)
                                + (view.info.dir ? "" : " · " + Theme.formatBytes(view.info.size)
                                   + (view.info.mtime > 0 ? " · " + Qt.formatDateTime(new Date(view.info.mtime), "yyyy-MM-dd hh:mm") : ""))
                              : ""
                    }
                    Text {
                        text: qsTr("Changes by device")
                        color: Theme.text
                        font.bold: true
                    }
                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: Theme.textSecondary
                        font.pixelSize: Theme.smallFontSize
                        text: qsTr("Each device counts the changes it made to this path; together the counts say which version is newer, without clocks.")
                    }
                    Repeater {
                        model: view.versionLines(view.info.version)
                        delegate: Text {
                            required property var modelData
                            Layout.fillWidth: true
                            textFormat: Text.PlainText
                            color: Theme.text
                            text: qsTr("%1: %2 change(s)").arg(modelData.name).arg(modelData.count)
                        }
                    }
                    Text {
                        visible: view.versionLines(view.info.version).length === 0
                        color: Theme.textSecondary
                        text: qsTr("No version on this device yet.")
                    }
                    Text {
                        Layout.topMargin: 6
                        text: qsTr("On other devices")
                        color: Theme.text
                        font.bold: true
                    }
                    Repeater {
                        model: view.info.holders || []
                        delegate: Text {
                            required property var modelData
                            Layout.fillWidth: true
                            textFormat: Text.PlainText
                            wrapMode: Text.WordWrap
                            color: Theme.text
                            text: view.holderText(modelData)
                        }
                    }
                    Text {
                        visible: !view.info.holders || view.info.holders.length === 0
                        color: Theme.textSecondary
                        text: qsTr("No other device has told this one about it.")
                    }
                    Text {
                        Layout.topMargin: 6
                        text: qsTr("Kept versions")
                        color: Theme.text
                        font.bold: true
                    }
                    Repeater {
                        model: view.info.kept || []
                        delegate: RowLayout {
                            required property var modelData
                            Layout.fillWidth: true
                            spacing: 8
                            Text {
                                Layout.fillWidth: true
                                textFormat: Text.PlainText
                                color: Theme.text
                                text: Qt.formatDateTime(new Date(modelData.time), "yyyy-MM-dd hh:mm:ss") + " · " + Theme.formatBytes(modelData.size)
                            }
                            UButton {
                                text: qsTr("Restore")
                                onClicked: {
                                    App.restoreSyncKept(view.folderId, modelData.kept)
                                    historyPopup.close()
                                }
                            }
                        }
                    }
                    Text {
                        visible: !view.info.kept || view.info.kept.length === 0
                        color: Theme.textSecondary
                        text: qsTr("Nothing kept in .synctrash for this path.")
                    }
                    Text {
                        Layout.topMargin: 6
                        text: qsTr("Recent events")
                        color: Theme.text
                        font.bold: true
                    }
                    Repeater {
                        model: view.eventsFor(view.infoPath)
                        delegate: Text {
                            required property var modelData
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            textFormat: Text.PlainText
                            color: Theme.textSecondary
                            font.pixelSize: Theme.smallFontSize
                            text: Qt.formatDateTime(new Date(modelData.time), "yyyy-MM-dd hh:mm:ss") + "  " + modelData.text
                        }
                    }
                    Text {
                        visible: view.eventsFor(view.infoPath).length === 0
                        color: Theme.textSecondary
                        text: qsTr("Nothing to report.")
                    }
                }
            }
        }
    }

    // The member list the window holds names every key.
    property var memberNames: ({})
    function deviceNameOf(key) { return memberNames[key] || qsTr("Unknown device") }
    function holderText(h) {
        const who = deviceNameOf(h.key)
        if (h.deleted) return qsTr("%1: deleted").arg(who)
        if (h.dir) return qsTr("%1: a folder").arg(who)
        const same = info.hash !== undefined && h.hash === info.hash
        return qsTr("%1: %2, %3%4").arg(who).arg(Theme.formatBytes(h.size))
            .arg(Qt.formatDateTime(new Date(h.mtime), "yyyy-MM-dd hh:mm"))
            .arg(h.skipped ? " · " + qsTr("skipped there") : same ? " · " + qsTr("same content") : "")
    }
}
