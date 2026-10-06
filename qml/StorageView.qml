import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import UConnect

// Browses another device's shared folders and transfers files in both directions.
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
    property bool transfersOpen: false
    readonly property var peer: { App.peers.revision; return App.peers.get(devId) }
    readonly property bool uploadGranted: peer.grantedToMe !== undefined
                                          && peer.grantedToMe.indexOf("storage.upload") >= 0
    readonly property bool canUpload: connected && uploadGranted && path !== "/" && !loading && error.length === 0

    readonly property var current: fileList.currentIndex >= 0 && fileList.currentIndex < entries.count
                                   ? entries.get(fileList.currentIndex) : null

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
        else download(index)
    }
    function download(index) {
        const e = entries.get(index)
        if (!e || e.dir) return
        App.storageDownload(devId, childPath(e.name))
        transfersOpen = true
    }
    function upload(files) {
        if (!canUpload) return false
        const added = App.storageUpload(devId, path, files)
        if (added > 0) transfersOpen = true
        return added > 0
    }

    FileDialog {
        id: uploadDialog
        title: qsTr("Upload files to %1").arg(view.deviceName)
        fileMode: FileDialog.OpenFiles
        acceptLabel: qsTr("Upload")
        onAccepted: view.upload(selectedFiles)
    }

    ListModel {
        id: entries
    }

    Connections {
        target: App
        function onStorageListed(requestId, listedPath, list, err) {
            if (requestId !== view.pendingId) return
            view.loading = false
            entries.clear()
            if (err.length > 0) {
                view.error = err
                return
            }
            for (const e of list) entries.append(e)
            fileList.currentIndex = entries.count > 0 ? 0 : -1
        }
        function onStorageUploaded(deviceId, remotePath) {
            if (deviceId === view.devId && view.parentPath(remotePath) === view.path) view.load()
        }
    }

    Component.onCompleted: load()
    onConnectedChanged: if (connected) load()

    Shortcut { sequence: "Alt+Left"; onActivated: view.goBack() }
    Shortcut { sequence: "Alt+Right"; onActivated: view.goForward() }
    Shortcut { sequence: "Alt+Up"; onActivated: view.goUp() }
    Shortcut { sequence: "F5"; onActivated: view.load() }

    component HeaderCell: Text {
        color: Theme.textSecondary
        font.pixelSize: Theme.smallFontSize
        elide: Text.ElideRight
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // --- navigation bar ------------------------------------------------
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
                Rectangle {
                    implicitWidth: 1
                    implicitHeight: 18
                    color: Theme.border
                }
                UToolButton {
                    text: qsTr("Download")
                    enabled: view.connected && view.current !== null && !view.current.dir
                    onClicked: view.download(fileList.currentIndex)
                }
                UToolButton {
                    text: qsTr("Upload")
                    enabled: view.canUpload
                    tip: !view.uploadGranted ? qsTr("This device has not granted upload permission.")
                         : view.path === "/" ? qsTr("Open a shared folder to upload files.")
                         : qsTr("Choose files to upload to this folder")
                    onClicked: uploadDialog.open()
                }
                UToolButton {
                    text: qsTr("Transfers")
                    checkable: true
                    checked: view.transfersOpen
                    onClicked: view.transfersOpen = !view.transfersOpen
                }
            }
            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: Theme.divider
            }
        }

        // --- column headers ------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 26
            Layout.leftMargin: 12
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

        // --- listing ---------------------------------------------------------
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            DropArea {
                id: fileDrop
                anchors.fill: parent
                onEntered: drag => {
                    drag.accepted = drag.hasUrls
                }
                onDropped: drop => {
                    if (view.upload(drop.urls)) drop.accept(Qt.CopyAction)
                    else drop.accepted = false
                }
            }

            ListView {
                id: fileList
                anchors.fill: parent
                anchors.margins: 4
                clip: true
                focus: true
                model: entries
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}
                Keys.onReturnPressed: view.activate(currentIndex)
                Keys.onEnterPressed: view.activate(currentIndex)
                Keys.onPressed: event => {
                    if (event.key === Qt.Key_Backspace) {
                        view.goUp()
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

                    width: ListView.view.width
                    height: 26
                    radius: Theme.radius
                    color: ListView.isCurrentItem ? Theme.selection
                           : rowArea.containsMouse ? Theme.hover : "transparent"

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 12
                        spacing: 12

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

                    MouseArea {
                        id: rowArea
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                        onClicked: mouse => {
                            fileList.currentIndex = row.index
                            fileList.forceActiveFocus()
                            if (mouse.button === Qt.RightButton) entryMenu.popup()
                        }
                        onDoubleClicked: view.activate(row.index)
                    }
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
                visible: fileDrop.containsDrag
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
                        text: view.canUpload ? qsTr("Drop files to upload") : qsTr("Upload unavailable")
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
                              : view.path === "/" ? qsTr("Open a shared folder first.")
                              : !view.canUpload ? qsTr("Wait for the folder to load, or retry the listing.")
                              : qsTr("Upload to %1. Existing files are kept; duplicate names get a numbered suffix.").arg(view.path)
                    }
                }
            }
        }

        Text {
            Layout.fillWidth: true
            Layout.leftMargin: 12
            Layout.rightMargin: 12
            Layout.topMargin: 6
            Layout.bottomMargin: 6
            text: view.path === "/" ? qsTr("Open a shared folder to upload files.")
                  : view.uploadGranted ? qsTr("Drop local files here or use Upload. Folders are not supported.")
                  : qsTr("This device has not granted upload permission.")
            color: Theme.textSecondary
            font.pixelSize: Theme.smallFontSize
            wrapMode: Text.WordWrap
        }

        // --- transfers -------------------------------------------------------
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            visible: view.transfersOpen
            color: Theme.divider
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 180
            visible: view.transfersOpen
            color: Theme.panel

            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                RowLayout {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 30
                    Layout.leftMargin: 12
                    Layout.rightMargin: 8
                    spacing: 10
                    Text {
                        text: qsTr("Transfers")
                        color: Theme.text
                        font.bold: true
                    }
                    Text {
                        Layout.fillWidth: true
                        elide: Text.ElideMiddle
                        textFormat: Text.PlainText
                        text: qsTr("Downloads saved to %1").arg(App.downloadFolder)
                        color: Theme.textSecondary
                        font.pixelSize: Theme.smallFontSize
                    }
                    ULink {
                        text: qsTr("Open Folder")
                        font.pixelSize: Theme.smallFontSize
                        onClicked: App.showInFolder(App.downloadFolder)
                    }
                    ULink {
                        text: qsTr("Clear Finished")
                        font.pixelSize: Theme.smallFontSize
                        onClicked: App.transfers.clearFinished()
                    }
                }

                ListView {
                    id: transferList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: App.transfers
                    ScrollBar.vertical: ScrollBar {}

                    delegate: Item {
                        id: transfer
                        required property real transferId
                        required property string devId
                        required property string fileName
                        required property string localPath
                        required property real received
                        required property real total
                        required property string transferState
                        required property string error
                        required property string direction

                        readonly property bool mine: devId === view.devId
                        width: ListView.view.width
                        height: mine ? 36 : 0
                        visible: mine

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 12
                            anchors.rightMargin: 12
                            spacing: 12

                            Text {
                                Layout.preferredWidth: 62
                                text: transfer.direction === "upload" ? qsTr("Upload") : qsTr("Download")
                                color: Theme.textSecondary
                                font.pixelSize: Theme.smallFontSize
                            }

                            Text {
                                Layout.preferredWidth: Math.min(200, view.width * 0.25)
                                text: transfer.fileName
                                textFormat: Text.PlainText
                                elide: Text.ElideMiddle
                                color: Theme.text
                            }
                            Rectangle {
                                Layout.fillWidth: true
                                implicitHeight: 4
                                radius: 2
                                color: Theme.border
                                visible: transfer.transferState === "active"
                                Rectangle {
                                    height: parent.height
                                    radius: 2
                                    color: Theme.accent
                                    width: transfer.total > 0 ? parent.width * Math.min(1, transfer.received / transfer.total) : 0
                                }
                            }
                            Text {
                                Layout.fillWidth: transfer.transferState !== "active"
                                Layout.preferredWidth: transfer.transferState === "active" ? 150 : -1
                                elide: Text.ElideRight
                                textFormat: Text.PlainText
                                font.pixelSize: Theme.smallFontSize
                                color: transfer.transferState === "failed" ? Theme.error
                                       : transfer.transferState === "done" ? Theme.success : Theme.textSecondary
                                text: {
                                    switch (transfer.transferState) {
                                    case "active":
                                        return transfer.total >= 0
                                               ? qsTr("%1 of %2").arg(Theme.formatBytes(transfer.received)).arg(Theme.formatBytes(transfer.total))
                                               : qsTr("Waiting...")
                                    case "done": return qsTr("Done · %1").arg(Theme.formatBytes(transfer.received))
                                    case "canceled": return qsTr("Canceled")
                                    }
                                    return transfer.error
                                }
                            }
                            ULink {
                                visible: transfer.transferState === "active"
                                text: qsTr("Cancel")
                                font.pixelSize: Theme.smallFontSize
                                onClicked: App.storageCancel(transfer.transferId)
                            }
                            ULink {
                                visible: transfer.transferState === "done" && transfer.direction === "download"
                                text: qsTr("Show in Folder")
                                font.pixelSize: Theme.smallFontSize
                                onClicked: App.showInFolder(transfer.localPath)
                            }
                        }
                    }
                }
            }
        }
    }

    UMenu {
        id: entryMenu
        UMenuItem {
            text: view.current && view.current.dir ? qsTr("Open") : qsTr("Download")
            onTriggered: view.activate(fileList.currentIndex)
        }
        UMenuItem {
            text: qsTr("Copy Path")
            onTriggered: if (view.current) App.copyText(view.childPath(view.current.name))
        }
    }
}
