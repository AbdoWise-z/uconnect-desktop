import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import UConnect

// Files syncing set aside in .synctrash: what each was, when it was set
// aside, and a way to put it back or let it go.
Item {
    id: view

    required property string folderId
    property var kept: []
    property bool loading: false

    function reload() {
        if (folderId.length === 0) return
        loading = true
        App.loadSyncKept(folderId)
    }
    Component.onCompleted: reload()
    onFolderIdChanged: reload()
    onVisibleChanged: if (visible) reload()

    Connections {
        target: App
        function onSyncKeptLoaded(id, list) {
            if (id !== view.folderId) return
            view.kept = list
            view.loading = false
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            Text {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.textSecondary
                font.pixelSize: Theme.smallFontSize
                text: qsTr("Files that syncing deletes, or that you give up in a conflict, wait here for 30 days, then go to the Recycle Bin. Restoring one puts it back where it was (beside the current file, if there is one) and syncs it to the other devices as a new change.")
            }
            UIconButton {
                glyph: "refresh"
                tip: qsTr("List again")
                onClicked: view.reload()
            }
            UIconButton {
                glyph: "open"
                tip: qsTr("Open .synctrash")
                onClicked: App.openSyncKept(view.folderId)
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
                objectName: "syncKeptList"
                anchors.fill: parent
                anchors.margins: 1
                clip: true
                model: view.kept
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}
                delegate: Rectangle {
                    id: row
                    required property var modelData
                    width: ListView.view.width
                    height: 44
                    color: rowArea.containsMouse ? Theme.hover : "transparent"
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 6
                        spacing: 8
                        UIcon {
                            name: "file"
                            size: 15
                            color: Theme.textSecondary
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            Text {
                                Layout.fillWidth: true
                                text: row.modelData.path
                                textFormat: Text.PlainText
                                elide: Text.ElideMiddle
                                color: Theme.text
                            }
                            Text {
                                Layout.fillWidth: true
                                text: qsTr("set aside %1 · %2").arg(Qt.formatDateTime(new Date(row.modelData.time), "yyyy-MM-dd hh:mm:ss")).arg(Theme.formatBytes(row.modelData.size))
                                textFormat: Text.PlainText
                                elide: Text.ElideRight
                                font.pixelSize: Theme.smallFontSize
                                color: Theme.textSecondary
                            }
                        }
                        UIconButton {
                            glyph: "restore"
                            tip: qsTr("Restore to %1").arg(row.modelData.path)
                            onClicked: App.restoreSyncKept(view.folderId, row.modelData.kept)
                        }
                        UIconButton {
                            glyph: "delete"
                            tip: qsTr("Remove it now")
                            onClicked: App.discardSyncKept(view.folderId, row.modelData.kept)
                        }
                    }
                    MouseArea {
                        id: rowArea
                        anchors.fill: parent
                        anchors.rightMargin: 70
                        hoverEnabled: true
                        onDoubleClicked: App.openSyncPath(view.folderId, ".synctrash/" + row.modelData.kept)
                    }
                }
                Text {
                    anchors.centerIn: parent
                    width: parent.width - 40
                    visible: list.count === 0
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    color: Theme.textSecondary
                    text: view.loading ? qsTr("Listing...") : qsTr("Nothing has been set aside in this folder.")
                }
            }
        }
    }
}
