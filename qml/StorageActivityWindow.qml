import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import UConnect

// What other devices did in this device's shared folders: who downloaded,
// uploaded, renamed, moved, copied or deleted what, newest first.
Window {
    id: win

    title: qsTr("Storage Activity")
    width: 760
    height: 520
    minimumWidth: 480
    minimumHeight: 300
    color: Theme.canvas

    function describe(e) {
        switch (e.action) {
        case "download": return qsTr("downloaded %1").arg(e.path)
        case "upload": return qsTr("uploaded %1").arg(e.path)
        case "mkdir": return qsTr("created folder %1").arg(e.path)
        case "rename": return qsTr("renamed %1 to %2").arg(e.path).arg(e.detail)
        case "delete": return qsTr("deleted %1").arg(e.path)
        case "move": return qsTr("moved %1 to %2").arg(e.path).arg(e.detail)
        case "copy": return qsTr("copied %1 to %2").arg(e.path).arg(e.detail)
        }
        return e.action + " " + e.path
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 38
            color: Theme.panel
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 8
                spacing: 8
                Text {
                    Layout.fillWidth: true
                    text: App.storageActivity.length === 1 ? qsTr("1 event")
                                                           : qsTr("%1 events").arg(App.storageActivity.length)
                    color: Theme.textSecondary
                    font.pixelSize: Theme.smallFontSize
                }
                UToolButton {
                    text: qsTr("Clear")
                    enabled: App.storageActivity.length > 0
                    onClicked: App.clearStorageActivity()
                }
            }
            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 1
                color: Theme.divider
            }
        }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: App.storageActivity
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}

            delegate: Item {
                id: event
                required property var modelData
                width: ListView.view.width
                height: 44

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    spacing: 10

                    Avatar {
                        name: event.modelData.device
                        seed: event.modelData.fingerprint.length > 0 ? event.modelData.fingerprint
                                                                     : event.modelData.devId
                        size: 22
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Text {
                            Layout.fillWidth: true
                            // Names and paths come from other devices: never markup.
                            textFormat: Text.PlainText
                            elide: Text.ElideMiddle
                            color: Theme.text
                            text: event.modelData.device + " " + win.describe(event.modelData)
                        }
                        Text {
                            Layout.fillWidth: true
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            color: Theme.textSecondary
                            font.pixelSize: Theme.smallFontSize
                            text: Qt.formatDateTime(new Date(event.modelData.time), "yyyy-MM-dd hh:mm:ss")
                                  + (event.modelData.fingerprint.length > 0
                                     ? " · " + event.modelData.fingerprint
                                     : " · " + qsTr("identity not proven"))
                                  + (event.modelData.detail.length > 0 && event.modelData.action !== "rename"
                                     && event.modelData.action !== "move" && event.modelData.action !== "copy"
                                     ? " · " + event.modelData.detail : "")
                        }
                    }
                }
            }

            Text {
                anchors.centerIn: parent
                width: parent.width - 80
                visible: list.count === 0
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                color: Theme.textSecondary
                text: qsTr("When other devices download, upload or change files in your shared folders, it is listed here.")
            }
        }
    }
}
