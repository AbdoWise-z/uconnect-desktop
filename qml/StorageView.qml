import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import UConnect

// Browses another device's shared folders and transfers files in both
// directions. Selection works like a file manager: click, Ctrl/Shift+click,
// the checkbox that appears on hover, or sweeping across rows with the mouse.
// Dragging selected items out of the window downloads them to wherever they
// are dropped; dropping local files in uploads them.
Item {
    id: view

    required property string devId
    required property string deviceName
    required property bool connected

    property string path: "/"
    property var backStack: []
    property var forwardStack: []
    property real pendingId: -1
    property bool loading: false
    property string error: ""
    property alias transfersOpen: transfers.open

    readonly property var peer: { App.peers.revision; return App.peers.get(devId) }
    function granted(permission) {
        return peer.grantedToMe !== undefined && peer.grantedToMe.indexOf(permission) >= 0
    }
    readonly property bool canRead: granted("storage.read")
    readonly property bool canWrite: granted("storage.write")
    readonly property bool uploadGranted: granted("storage.upload")
    readonly property bool canUpload: canUploadTo(path)

    property int selectedCount: 0
    property int anchorIndex: -1
    property int hoveredIndex: -1
    property int dropIndex: -1  // folder row under an incoming drop

    // Drag-out: the selection is fetched first, then dragged as local files.
    property real dragBatch: -1
    property string dragState: ""  // "" | "preparing" | "ready" | "dragging"
    property var dragUrls: []
    property string dragProgress: ""
    property point dragCursor

    readonly property var current: fileList.currentIndex >= 0 && fileList.currentIndex < entries.count
                                   ? entries.get(fileList.currentIndex) : null

    // --- paths ------------------------------------------------------------------
    function childPath(name) {
        return path === "/" ? "/" + name : path + "/" + name
    }
    function parentPath(p) {
        const i = p.lastIndexOf("/")
        return i <= 0 ? "/" : p.substring(0, i)
    }
    function normalize(text) {
        return "/" + text.split("/").filter(s => s.length > 0).join("/")
    }
    function canUploadTo(folder) {
        return connected && uploadGranted && folder !== "/" && !loading && error.length === 0
    }

    // --- navigation -------------------------------------------------------------
    function load() {
        if (!connected) return
        loading = true
        error = ""
        pendingId = App.storageList(devId, path)
    }
    function navigate(p) {
        if (p !== path) {
            backStack = backStack.concat([path])
            forwardStack = []
            path = p
        }
        load()
    }
    function goBack() {
        if (backStack.length === 0) return
        forwardStack = forwardStack.concat([path])
        path = backStack[backStack.length - 1]
        backStack = backStack.slice(0, -1)
        load()
    }
    function goForward() {
        if (forwardStack.length === 0) return
        backStack = backStack.concat([path])
        path = forwardStack[forwardStack.length - 1]
        forwardStack = forwardStack.slice(0, -1)
        load()
    }
    function goUp() {
        if (path !== "/") navigate(parentPath(path))
    }
    function activate(index) {
        const e = entries.get(index)
        if (!e) return
        if (e.dir) navigate(childPath(e.name))
        else if (canRead) App.storageOpen(devId, path, e.name)
    }

    // --- selection --------------------------------------------------------------
    function isSelected(i) {
        return i >= 0 && i < entries.count && entries.get(i).selected
    }
    function setSelected(i, on) {
        if (i < 0 || i >= entries.count || entries.get(i).selected === on) return
        entries.setProperty(i, "selected", on)
        selectedCount += on ? 1 : -1
    }
    function clearSelection() {
        for (let i = 0; i < entries.count; ++i)
            if (entries.get(i).selected) entries.setProperty(i, "selected", false)
        selectedCount = 0
    }
    function selectOnly(i) {
        clearSelection()
        setSelected(i, true)
        anchorIndex = i
        fileList.currentIndex = i
    }
    function selectAll() {
        for (let i = 0; i < entries.count; ++i) setSelected(i, true)
    }
    function selectedIndexes() {
        const out = []
        for (let i = 0; i < entries.count; ++i)
            if (entries.get(i).selected) out.push(i)
        return out
    }
    // Exactly `base` plus the rows between `from` and `to`.
    function selectRange(base, from, to) {
        const low = Math.min(from, to), high = Math.max(from, to)
        for (let i = 0; i < entries.count; ++i)
            setSelected(i, (i >= low && i <= high) || base.indexOf(i) >= 0)
    }
    function selectedItems() {
        return selectedIndexes().map(i => ({ name: entries.get(i).name, dir: entries.get(i).dir }))
    }
    function selectedPaths() {
        return selectedIndexes().map(i => childPath(entries.get(i).name))
    }
    function entryNamed(name) {
        for (let i = 0; i < entries.count; ++i)
            if (entries.get(i).name === name) return entries.get(i)
        return null
    }

    // --- transfers and changes ----------------------------------------------------
    function downloadSelection() {
        const items = selectedItems()
        if (!canRead || items.length === 0) return
        App.storageDownloadItems(devId, path, items)
        transfersOpen = true
    }
    function fileNameOf(url) {
        return decodeURIComponent(url.toString().split("/").pop())
    }
    // Uploads files and folders into `folder` (this one by default). Items
    // that clash with ones shown here can replace them (folders: merge into
    // them) if this device may write; otherwise they get a numbered name.
    function upload(files, folder) {
        const target = folder !== undefined ? folder : path
        if (!canUploadTo(target)) return false
        if (target === path && canWrite) {
            const clashes = files.filter(url => entryNamed(fileNameOf(url)) !== null)
            if (clashes.length > 0) {
                const folders = clashes.filter(url => entryNamed(fileNameOf(url)).dir).length
                prompt.pending = { files: files, folder: target }
                prompt.ask(folders > 0 ? qsTr("Merge with existing folders?") : qsTr("Replace existing files?"),
                           (clashes.length === 1
                            ? qsTr("“%1” already exists in %2.").arg(fileNameOf(clashes[0])).arg(target)
                            : qsTr("%1 of these items already exist in %2.").arg(clashes.length).arg(target))
                           + (folders > 0 ? " " + qsTr("Merging adds to the existing folder and replaces files of the same name.") : ""),
                           [{ id: "keep", label: qsTr("Keep Both") },
                            { id: "replace", label: folders > 0 ? qsTr("Merge") : qsTr("Replace"), primary: true }])
                return true
            }
        }
        return sendUpload(files, target, false)
    }
    function sendUpload(files, folder, replace) {
        const added = App.storageUpload(devId, folder, files, replace)
        if (added > 0) transfersOpen = true
        return added > 0
    }
    function renameCurrent() {
        const items = selectedItems()
        if (!canWrite || items.length !== 1 || path === "/") return
        prompt.pending = { rename: items[0].name }
        prompt.ask(qsTr("Rename"), "", [{ id: "rename", label: qsTr("Rename"), primary: true }], items[0].name)
    }
    function deleteSelection() {
        const paths = selectedPaths()
        if (!canWrite || paths.length === 0 || path === "/") return
        prompt.pending = { remove: paths }
        prompt.ask(paths.length === 1 ? qsTr("Delete “%1”?").arg(selectedItems()[0].name)
                                      : qsTr("Delete %1 items?").arg(paths.length),
                   qsTr("They are moved to the Recycle Bin on %1 when it has one; otherwise they are deleted.")
                       .arg(deviceName),
                   [{ id: "delete", label: qsTr("Delete"), primary: true }])
    }
    function newFolder() {
        if (!canUploadTo(path)) return
        prompt.pending = { mkdir: true }
        prompt.ask(qsTr("New Folder"), "", [{ id: "mkdir", label: qsTr("Create"), primary: true }],
                   qsTr("New folder"))
    }

    // --- drag-out -------------------------------------------------------------------
    function startDragOut() {
        const items = selectedItems()
        if (!canRead || items.length === 0) return false
        dragUrls = []
        dragProgress = ""
        dragState = "preparing"
        dragBatch = App.prepareDragOut(devId, path, items)
        return true
    }
    function cancelDragOut() {
        if (dragState === "preparing" || dragState === "ready") App.cancelBatch(dragBatch)
        resetDrag()
    }
    function resetDrag() {
        dragState = ""
        dragBatch = -1
        dragUrls = []
        dragProgress = ""
    }
    // Must run inside a mouse event: platforms start a drag only from one.
    function beginOsDrag() {
        dragState = "dragging"
        dragSource.Drag.active = true
    }

    FileDialog {
        id: uploadDialog
        title: qsTr("Upload files to %1").arg(view.deviceName)
        fileMode: FileDialog.OpenFiles
        acceptLabel: qsTr("Upload")
        onAccepted: view.upload(selectedFiles)
    }
    FolderDialog {
        id: uploadFolderDialog
        title: qsTr("Upload a folder to %1").arg(view.deviceName)
        acceptLabel: qsTr("Upload")
        onAccepted: view.upload([selectedFolder])
    }

    PromptDialog {
        id: prompt
        property var pending: ({})
        onChosen: (choiceId, value) => {
            if (choiceId === "replace" || choiceId === "keep")
                view.sendUpload(pending.files, pending.folder, choiceId === "replace")
            else if (choiceId === "rename" && value !== pending.rename)
                App.storageRename(view.devId, view.childPath(pending.rename), value)
            else if (choiceId === "delete")
                App.storageDelete(view.devId, pending.remove)
            else if (choiceId === "mkdir")
                App.storageMakeFolder(view.devId, view.path, value)
            pending = {}
        }
    }

    ListModel {
        id: entries
    }

    // Several changes finishing together refresh the listing once.
    Timer {
        id: reloadSoon
        interval: 150
        onTriggered: view.load()
    }

    Connections {
        target: App
        ignoreUnknownSignals: true
        function onStorageListed(requestId, listedPath, list, err) {
            if (requestId !== view.pendingId) return
            view.loading = false
            entries.clear()
            view.selectedCount = 0
            view.anchorIndex = -1
            if (err.length > 0) {
                view.error = err
                return
            }
            for (const e of list) entries.append(Object.assign({ selected: false }, e))
            fileList.currentIndex = entries.count > 0 ? 0 : -1
        }
        function onStorageUploaded(deviceId, remotePath) {
            if (deviceId === view.devId && view.parentPath(remotePath) === view.path) reloadSoon.restart()
        }
        function onStorageOperationFinished(requestId, deviceId, changedPath, err) {
            if (deviceId === view.devId) reloadSoon.restart()
        }
        function onStorageBatchProgress(batchId, filesDone, filesKnown) {
            if (batchId === view.dragBatch)
                view.dragProgress = qsTr("%1 of %2 files").arg(filesDone).arg(filesKnown)
        }
        function onStorageBatchFinished(batchId, failed, canceled, urls) {
            if (batchId !== view.dragBatch) return
            if (canceled || urls.length === 0) {
                view.resetDrag()
                return
            }
            view.dragUrls = urls
            view.dragState = "ready"
        }
    }

    Component.onCompleted: load()
    onConnectedChanged: if (connected) load()

    Shortcut { sequence: "Alt+Left"; onActivated: view.goBack() }
    Shortcut { sequence: "Alt+Right"; onActivated: view.goForward() }
    Shortcut { sequence: "Alt+Up"; onActivated: view.goUp() }
    Shortcut { sequence: "F5"; onActivated: view.load() }
    Shortcut { sequence: "F2"; onActivated: view.renameCurrent() }
    Shortcut { sequence: StandardKey.SelectAll; enabled: fileList.activeFocus; onActivated: view.selectAll() }
    Shortcut { sequence: "Delete"; enabled: fileList.activeFocus; onActivated: view.deleteSelection() }
    Shortcut { sequence: "Escape"; enabled: fileList.activeFocus; onActivated: view.clearSelection() }

    component HeaderCell: Text {
        color: Theme.textSecondary
        font.pixelSize: Theme.smallFontSize
        elide: Text.ElideRight
    }
    component Separator: Rectangle {
        implicitWidth: 1
        implicitHeight: 18
        color: Theme.border
    }

    // The item a drag-out is started from; it carries the fetched files.
    Item {
        id: dragSource
        Drag.dragType: Drag.Automatic
        Drag.supportedActions: Qt.CopyAction
        Drag.proposedAction: Qt.CopyAction
        Drag.mimeData: ({ "text/uri-list": view.dragUrls.join("\r\n") })
        Drag.onDragFinished: dropAction => {
            view.resetDrag()
            // The platform drag consumed the release; drop the stale press.
            pointer.enabled = false
            pointer.enabled = true
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // --- toolbar -----------------------------------------------------------
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 38
            color: Theme.panel

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                spacing: 4

                UToolButton {
                    text: "←"
                    enabled: view.backStack.length > 0
                    tip: qsTr("Back (Alt+Left)")
                    onClicked: view.goBack()
                }
                UToolButton {
                    text: "→"
                    enabled: view.forwardStack.length > 0
                    tip: qsTr("Forward (Alt+Right)")
                    onClicked: view.goForward()
                }
                UToolButton {
                    text: "↑"
                    enabled: view.path !== "/"
                    tip: qsTr("Up (Alt+Up)")
                    onClicked: view.goUp()
                }
                UTextField {
                    id: pathField
                    Layout.fillWidth: true
                    implicitHeight: 26
                    text: view.path
                    enabled: view.connected
                    onAccepted: view.navigate(view.normalize(text))
                }
                UToolButton {
                    text: "↻"
                    enabled: view.connected
                    tip: qsTr("Refresh (F5)")
                    onClicked: view.load()
                }
                Separator {}
                UToolButton {
                    text: qsTr("Download")
                    enabled: view.connected && view.canRead && view.selectedCount > 0
                    tip: !view.canRead ? qsTr("This device has not granted Read permission.")
                         : qsTr("Download the selected items, folders included")
                    onClicked: view.downloadSelection()
                }
                UToolButton {
                    id: uploadButton
                    text: qsTr("Upload ▾")
                    enabled: view.canUpload
                    tip: !view.uploadGranted ? qsTr("This device has not granted upload permission.")
                         : view.path === "/" ? qsTr("Open a shared folder to upload files.")
                         : qsTr("Upload files or a folder to this folder")
                    onClicked: uploadMenu.popup(uploadButton, 0, uploadButton.height)
                }
                UToolButton {
                    text: qsTr("New Folder")
                    enabled: view.canUpload
                    tip: !view.uploadGranted ? qsTr("This device has not granted upload permission.")
                         : qsTr("Create a folder here")
                    onClicked: view.newFolder()
                }
                UToolButton {
                    text: qsTr("Rename")
                    enabled: view.connected && view.canWrite && view.selectedCount === 1 && view.path !== "/"
                    tip: !view.canWrite ? qsTr("This device has not granted Write permission.") : qsTr("Rename (F2)")
                    onClicked: view.renameCurrent()
                }
                UToolButton {
                    text: qsTr("Delete")
                    enabled: view.connected && view.canWrite && view.selectedCount > 0 && view.path !== "/"
                    tip: !view.canWrite ? qsTr("This device has not granted Write permission.") : qsTr("Delete (Del)")
                    onClicked: view.deleteSelection()
                }
                Separator {}
                UToolButton {
                    text: qsTr("Transfers")
                    checkable: true
                    checked: view.transfersOpen
                    onClicked: {
                        view.transfersOpen = !view.transfersOpen
                        if (view.transfersOpen) transfers.minimized = false
                    }
                }
            }
            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: Theme.divider
            }
        }

        // --- column headers ----------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 26
            Layout.leftMargin: 36
            Layout.rightMargin: 16
            spacing: 12
            HeaderCell { Layout.fillWidth: true; text: qsTr("Name") }
            HeaderCell { Layout.preferredWidth: 90; horizontalAlignment: Text.AlignRight; text: qsTr("Size") }
            HeaderCell { Layout.preferredWidth: 140; text: qsTr("Modified") }
        }
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: Theme.border
        }

        // --- listing -----------------------------------------------------------
        Item {
            id: listArea
            Layout.fillWidth: true
            Layout.fillHeight: true

            DropArea {
                id: fileDrop
                anchors.fill: parent
                function rowAt(y) {
                    const i = fileList.indexAt(20, y - fileList.y + fileList.contentY)
                    return i >= 0 && entries.get(i).dir ? i : -1
                }
                onEntered: drag => {
                    // Our own drag-out passing over the list is not an upload.
                    drag.accepted = drag.hasUrls && drag.source !== dragSource
                }
                onPositionChanged: drag => view.dropIndex = rowAt(drag.y)
                onExited: view.dropIndex = -1
                onDropped: drop => {
                    const row = rowAt(drop.y)
                    view.dropIndex = -1
                    const folder = row >= 0 ? view.childPath(entries.get(row).name) : view.path
                    if (drop.source !== dragSource && view.upload(drop.urls, folder)) drop.accept(Qt.CopyAction)
                    else drop.accepted = false
                }
            }

            ListView {
                id: fileList
                objectName: "storageFileList"
                anchors.fill: parent
                anchors.margins: 4
                clip: true
                focus: true
                interactive: false  // the mouse selects; the wheel and scrollbar scroll
                model: entries
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar { id: scrollBar }
                Keys.onReturnPressed: view.activate(currentIndex)
                Keys.onEnterPressed: view.activate(currentIndex)
                Keys.onPressed: event => {
                    if (event.key === Qt.Key_Backspace) {
                        view.goUp()
                        event.accepted = true
                    } else if ((event.key === Qt.Key_Up || event.key === Qt.Key_Down) && entries.count > 0) {
                        const next = Math.max(0, Math.min(entries.count - 1,
                                                          currentIndex + (event.key === Qt.Key_Up ? -1 : 1)))
                        if (event.modifiers & Qt.ShiftModifier) view.selectRange([], view.anchorIndex < 0 ? next : view.anchorIndex, next)
                        else view.selectOnly(next)
                        currentIndex = next
                        positionViewAtIndex(next, ListView.Contain)
                        event.accepted = true
                    }
                }

                delegate: Rectangle {
                    id: row
                    required property int index
                    required property string name
                    required property bool dir
                    required property real size
                    required property real mtime
                    required property bool selected

                    readonly property bool hovered: index === view.hoveredIndex
                    width: ListView.view.width
                    height: 26
                    radius: Theme.radius
                    color: selected ? Theme.selection : hovered ? Theme.hover : "transparent"
                    border.width: index === view.dropIndex ? 2 : 0
                    border.color: Theme.accent

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 6
                        anchors.rightMargin: 12
                        spacing: 10

                        // The selection checkbox appears on hover, and stays
                        // while anything is selected.
                        Rectangle {
                            implicitWidth: 14
                            implicitHeight: 14
                            radius: 3
                            opacity: row.selected || row.hovered || view.selectedCount > 0 ? 1 : 0
                            color: row.selected ? Theme.accent : "transparent"
                            border.width: row.selected ? 0 : 1
                            border.color: Theme.inputBorder
                            Text {
                                anchors.centerIn: parent
                                visible: row.selected
                                text: "✓"
                                color: "#FFFFFF"
                                font.pixelSize: 11
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            // Folder: filled tab shape; file: outlined page.
                            Item {
                                implicitWidth: 16
                                implicitHeight: 14
                                Rectangle {
                                    visible: row.dir
                                    anchors.fill: parent
                                    anchors.topMargin: 2
                                    radius: 2
                                    color: "#C9A653"
                                }
                                Rectangle {
                                    visible: row.dir
                                    width: 7
                                    height: 4
                                    radius: 1
                                    color: "#C9A653"
                                }
                                Rectangle {
                                    visible: !row.dir
                                    anchors.centerIn: parent
                                    width: 11
                                    height: 14
                                    radius: 2
                                    color: "transparent"
                                    border.color: Theme.textSecondary
                                }
                            }
                            Text {
                                Layout.fillWidth: true
                                text: row.name
                                textFormat: Text.PlainText
                                elide: Text.ElideRight
                                color: Theme.text
                            }
                        }
                        Text {
                            Layout.preferredWidth: 90
                            horizontalAlignment: Text.AlignRight
                            text: row.dir ? "" : Theme.formatBytes(row.size)
                            color: Theme.textSecondary
                        }
                        Text {
                            Layout.preferredWidth: 140
                            text: row.mtime > 0 ? Qt.formatDateTime(new Date(row.mtime), "yyyy-MM-dd hh:mm") : ""
                            color: Theme.textSecondary
                        }
                    }
                }
            }

            // One pointer handler for the whole list: hover, selection gestures,
            // drag-out, double-click and the context menu.
            MouseArea {
                id: pointer
                anchors.fill: fileList
                anchors.rightMargin: scrollBar.visible ? scrollBar.width : 0  // leave the scrollbar draggable
                hoverEnabled: true
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                preventStealing: true

                readonly property int checkboxWidth: 28
                property int pressIndex: -1
                property point pressPoint
                property string gesture: ""  // "" | "sweep" | "maybeDrag" | "dragOut"
                property var sweepBase: []
                property int sweepAnchor: -1
                property real lastY: 0

                function rowAt(y) {
                    return fileList.indexAt(20, y + fileList.contentY)
                }
                // The row under y, or the nearest one when outside the list.
                function rowNear(y) {
                    if (entries.count === 0) return -1
                    const clamped = Math.max(0, Math.min(height - 1, y))
                    const i = rowAt(clamped)
                    return i >= 0 ? i : (clamped < height / 2 ? 0 : entries.count - 1)
                }
                function sweepTo(y) {
                    const i = rowNear(y)
                    if (i < 0) return
                    if (sweepAnchor < 0) sweepAnchor = i
                    view.selectRange(sweepBase, sweepAnchor, i)
                    view.anchorIndex = sweepAnchor
                    fileList.currentIndex = i
                }

                onExited: view.hoveredIndex = -1
                onPressed: mouse => {
                    fileList.forceActiveFocus()
                    const i = rowAt(mouse.y)
                    pressIndex = i
                    pressPoint = Qt.point(mouse.x, mouse.y)
                    gesture = ""
                    if (mouse.button === Qt.RightButton) {
                        if (i >= 0 && !view.isSelected(i)) view.selectOnly(i)
                        contextMenu.popup()
                        return
                    }
                    const ctrl = (mouse.modifiers & Qt.ControlModifier) !== 0
                    if (i >= 0 && mouse.x < checkboxWidth) {
                        view.setSelected(i, !view.isSelected(i))
                        view.anchorIndex = i
                        fileList.currentIndex = i
                        return
                    }
                    if ((mouse.modifiers & Qt.ShiftModifier) && view.anchorIndex >= 0 && i >= 0) {
                        view.selectRange(ctrl ? view.selectedIndexes() : [], view.anchorIndex, i)
                        fileList.currentIndex = i
                        return
                    }
                    if (ctrl && i >= 0 && view.isSelected(i)) {
                        view.setSelected(i, false)
                        return
                    }
                    if (!ctrl && i >= 0 && view.isSelected(i)) {
                        gesture = "maybeDrag"  // a click selects it alone; a drag takes the selection out
                        return
                    }
                    gesture = "sweep"
                    sweepBase = ctrl ? view.selectedIndexes() : []
                    if (!ctrl) view.clearSelection()
                    sweepAnchor = i
                    if (i >= 0) sweepTo(mouse.y)
                }
                onPositionChanged: mouse => {
                    lastY = mouse.y
                    if (!pressed) {
                        view.hoveredIndex = rowAt(mouse.y)
                        return
                    }
                    if (gesture === "maybeDrag"
                            && Math.abs(mouse.x - pressPoint.x) + Math.abs(mouse.y - pressPoint.y) > 8) {
                        gesture = view.startDragOut() ? "dragOut" : ""
                    }
                    if (gesture === "dragOut") {
                        view.dragCursor = Qt.point(mouse.x, mouse.y)
                        if (view.dragState === "ready") view.beginOsDrag()
                    } else if (gesture === "sweep") {
                        sweepTo(mouse.y)
                        if (mouse.y < 0 || mouse.y > height) autoScroll.start()
                        else autoScroll.stop()
                    }
                }
                onReleased: mouse => {
                    autoScroll.stop()
                    if (gesture === "maybeDrag" && pressIndex >= 0) view.selectOnly(pressIndex)
                    else if (gesture === "dragOut" && view.dragState !== "dragging") view.cancelDragOut()
                    gesture = ""
                }
                onCanceled: {
                    autoScroll.stop()
                    if (gesture === "dragOut" && view.dragState !== "dragging") view.cancelDragOut()
                    gesture = ""
                }
                onDoubleClicked: mouse => {
                    const i = rowAt(mouse.y)
                    if (mouse.button === Qt.LeftButton && i >= 0 && mouse.x >= checkboxWidth) view.activate(i)
                }
                onWheel: wheel => {
                    const most = Math.max(0, fileList.contentHeight - fileList.height)
                    fileList.contentY = Math.max(0, Math.min(most, fileList.contentY - wheel.angleDelta.y / 120 * 78))
                }

                // Sweeping past the top or bottom edge scrolls the list.
                Timer {
                    id: autoScroll
                    interval: 30
                    repeat: true
                    onTriggered: {
                        const step = pointer.lastY < 0 ? -20 : 20
                        fileList.contentY = Math.max(0, Math.min(fileList.contentHeight - fileList.height,
                                                                 fileList.contentY + step))
                        pointer.sweepTo(pointer.lastY)
                    }
                }
            }

            // Where a drag-out is while its files are being fetched.
            Rectangle {
                visible: view.dragState === "preparing" || view.dragState === "ready"
                x: Math.min(listArea.width - width - 8, fileList.x + view.dragCursor.x + 14)
                y: Math.min(listArea.height - height - 8, fileList.y + view.dragCursor.y + 14)
                width: dragLabel.implicitWidth + 20
                height: dragLabel.implicitHeight + 12
                radius: Theme.radius
                color: Theme.popup
                border.color: view.dragState === "ready" ? Theme.accent : Theme.popupBorder
                Text {
                    id: dragLabel
                    anchors.centerIn: parent
                    color: Theme.text
                    font.pixelSize: Theme.smallFontSize
                    text: view.dragState === "ready"
                          ? qsTr("Ready — drag to where the files should go")
                          : qsTr("Fetching %1 — keep holding%2")
                                .arg(view.selectedCount === 1 ? qsTr("1 item") : qsTr("%1 items").arg(view.selectedCount))
                                .arg(view.dragProgress.length > 0 ? " (" + view.dragProgress + ")" : "")
                }
            }

            Text {
                anchors.centerIn: parent
                visible: view.loading && entries.count === 0
                text: qsTr("Loading...")
                color: Theme.textSecondary
            }
            Text {
                anchors.centerIn: parent
                width: parent.width - 80
                visible: !view.loading && view.error.length === 0 && entries.count === 0 && view.connected
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                color: Theme.textSecondary
                textFormat: Text.PlainText
                text: view.path === "/"
                      ? qsTr("%1 is not sharing any folders.").arg(view.deviceName)
                      : qsTr("This folder is empty.")
            }
            Banner {
                anchors.centerIn: parent
                width: Math.min(520, parent.width - 48)
                visible: view.error.length > 0
                kind: "error"
                text: view.error
                UButton {
                    text: qsTr("Retry")
                    onClicked: view.load()
                }
            }
            Rectangle {
                anchors.fill: parent
                anchors.margins: 8
                visible: fileDrop.containsDrag && view.dropIndex < 0
                color: Theme.panel
                opacity: 0.96
                radius: Theme.radius
                border.width: 2
                border.color: view.canUpload ? Theme.accent : Theme.border

                ColumnLayout {
                    anchors.centerIn: parent
                    width: Math.min(440, parent.width - 48)
                    spacing: 10
                    Text {
                        Layout.fillWidth: true
                        text: view.canUpload ? qsTr("Drop files or folders to upload") : qsTr("Upload unavailable")
                        font.pixelSize: 20
                        font.bold: true
                        horizontalAlignment: Text.AlignHCenter
                        color: Theme.text
                    }
                    Text {
                        Layout.fillWidth: true
                        textFormat: Text.PlainText
                        wrapMode: Text.WordWrap
                        horizontalAlignment: Text.AlignHCenter
                        color: Theme.textSecondary
                        text: !view.connected ? qsTr("Connect to this device first.")
                              : !view.uploadGranted ? qsTr("Ask this device to grant upload permission.")
                              : view.path === "/" ? qsTr("Drop onto one of the shared folders.")
                              : !view.canUpload ? qsTr("Wait for the folder to load, or retry the listing.")
                              : qsTr("Upload to %1, or drop onto a folder to upload into it.").arg(view.path)
                    }
                }
            }

            TransfersPopup {
                id: transfers
                objectName: "storageTransfers"
                devId: view.devId
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 12
                z: 10
            }
        }

        // --- status line -----------------------------------------------------------
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: Theme.divider
        }
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 26
            Layout.leftMargin: 12
            Layout.rightMargin: 12
            Text {
                text: view.selectedCount > 0
                      ? qsTr("%1 of %2 selected").arg(view.selectedCount).arg(entries.count)
                      : entries.count === 1 ? qsTr("1 item") : qsTr("%1 items").arg(entries.count)
                color: Theme.textSecondary
                font.pixelSize: Theme.smallFontSize
            }
            Item { Layout.fillWidth: true }
            Text {
                text: view.path === "/" ? qsTr("Open a shared folder to upload files.")
                      : view.uploadGranted && view.canRead ? qsTr("Drop files here to upload · drag items out to download")
                      : view.uploadGranted ? qsTr("Drop files here to upload")
                      : view.canRead ? qsTr("Drag items out to download")
                      : qsTr("This device has not granted you file access.")
                color: Theme.textSecondary
                font.pixelSize: Theme.smallFontSize
                elide: Text.ElideLeft
            }
        }
    }

    UMenu {
        id: uploadMenu
        UMenuItem {
            text: qsTr("Files...")
            onTriggered: uploadDialog.open()
        }
        UMenuItem {
            text: qsTr("Folder...")
            onTriggered: uploadFolderDialog.open()
        }
    }

    UMenu {
        id: contextMenu
        UMenuItem {
            text: qsTr("Open")
            hint: "Enter"
            enabled: view.current !== null && (view.current.dir || view.canRead)
            onTriggered: view.activate(fileList.currentIndex)
        }
        UMenuItem {
            text: view.selectedCount > 1 ? qsTr("Download %1 Items").arg(view.selectedCount) : qsTr("Download")
            enabled: view.canRead && view.selectedCount > 0
            onTriggered: view.downloadSelection()
        }
        UMenuSeparator {}
        UMenuItem {
            text: qsTr("Rename...")
            hint: "F2"
            enabled: view.canWrite && view.selectedCount === 1 && view.path !== "/"
            onTriggered: view.renameCurrent()
        }
        UMenuItem {
            text: qsTr("Delete...")
            hint: "Del"
            enabled: view.canWrite && view.selectedCount > 0 && view.path !== "/"
            onTriggered: view.deleteSelection()
        }
        UMenuSeparator {}
        UMenuItem {
            text: qsTr("New Folder...")
            enabled: view.canUpload
            onTriggered: view.newFolder()
        }
        UMenuItem {
            text: qsTr("Upload Files...")
            enabled: view.canUpload
            onTriggered: uploadDialog.open()
        }
        UMenuItem {
            text: qsTr("Upload Folder...")
            enabled: view.canUpload
            onTriggered: uploadFolderDialog.open()
        }
        UMenuSeparator {}
        UMenuItem {
            text: qsTr("Select All")
            hint: "Ctrl+A"
            onTriggered: view.selectAll()
        }
        UMenuItem {
            text: qsTr("Copy Path")
            enabled: view.current !== null
            onTriggered: App.copyText(view.childPath(view.current.name))
        }
    }
}
