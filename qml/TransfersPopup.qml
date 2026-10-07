import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import UConnect

// One device's transfers as a floating panel in the corner of its storage
// window, like the IDE's "Processes" popup. It opens when a transfer starts,
// and can be minimized to a strip, maximized, or closed.
Item {
    id: popup

    required property string devId
    property bool open: false
    property bool minimized: false
    property bool maximized: false

    readonly property var summary: { App.transfers.revision; return App.transfers.summary(devId) }
    readonly property real fraction: summary.total > 0 ? Math.min(1, summary.done / summary.total) : 0
    property int seen: 0

    // A new transfer brings the panel back, unless the user minimized it.
    onSummaryChanged: {
        if (summary.count > seen) open = true
        seen = summary.count
    }

    visible: open
    width: minimized ? 300 : maximized ? Math.min(parent.width - 24, 900) : Math.min(460, parent.width - 24)
    height: minimized ? 40 : maximized ? Math.max(260, parent.height * 0.75) : Math.min(280, parent.height - 24)

    function statusText() {
        if (summary.active > 0) {
            const percent = summary.total > 0 ? " · " + Math.round(fraction * 100) + "%" : ""
            return (summary.active === 1 ? qsTr("1 transfer") : qsTr("%1 transfers").arg(summary.active)) + percent
        }
        if (summary.failed > 0) return qsTr("%1 failed").arg(summary.failed)
        return summary.count > 0 ? qsTr("All done") : qsTr("No transfers")
    }

    // Window-control glyphs drawn rather than taken from a font.
    component Glyph: Item {
        property string kind  // "minimize" | "maximize" | "restore" | "close"
        implicitWidth: 10
        implicitHeight: 10
        Rectangle {
            visible: parent.kind === "minimize"
            anchors.centerIn: parent
            width: 10
            height: 1.5
            color: Theme.text
        }
        Rectangle {
            visible: parent.kind === "maximize" || parent.kind === "restore"
            y: parent.kind === "restore" ? 3 : 0
            width: parent.kind === "restore" ? 7 : 10
            height: parent.kind === "restore" ? 7 : 10
            color: "transparent"
            border.color: Theme.text
            border.width: 1.5
        }
        Rectangle {
            visible: parent.kind === "restore"
            x: 3
            y: 0
            width: 7
            height: 7
            color: "transparent"
            border.color: Theme.text
            border.width: 1.5
        }
        Text {
            visible: parent.kind === "close"
            anchors.centerIn: parent
            text: "×"
            color: Theme.text
            font.pixelSize: 15
        }
    }

    component HeaderButton: UToolButton {
        property alias kind: glyph.kind
        implicitWidth: 26
        implicitHeight: 24
        contentItem: Item {
            Glyph {
                id: glyph
                anchors.centerIn: parent
            }
        }
    }

    component Bar: Rectangle {
        property real value: 0
        implicitHeight: 4
        radius: 2
        color: Theme.border
        Rectangle {
            width: parent.width * Math.max(0, Math.min(1, parent.value))
            height: parent.height
            radius: 2
            color: Theme.accent
        }
    }

    Rectangle {
        anchors.fill: parent
        radius: 8
        color: Theme.popup
        border.color: Theme.popupBorder

        // --- minimized: one strip ------------------------------------------
        RowLayout {
            visible: popup.minimized
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 6
            spacing: 8

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 3
                Text {
                    Layout.fillWidth: true
                    text: popup.statusText()
                    elide: Text.ElideRight
                    color: Theme.text
                    font.pixelSize: Theme.smallFontSize
                }
                Bar {
                    Layout.fillWidth: true
                    visible: popup.summary.active > 0
                    value: popup.fraction
                }
            }
            HeaderButton {
                kind: "restore"
                tip: qsTr("Restore")
                onClicked: popup.minimized = false
            }
            HeaderButton {
                kind: "close"
                tip: qsTr("Hide")
                onClicked: popup.open = false
            }
        }

        // --- normal and maximized --------------------------------------------
        ColumnLayout {
            visible: !popup.minimized
            anchors.fill: parent
            anchors.margins: 1
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: 36
                Layout.leftMargin: 12
                Layout.rightMargin: 6
                spacing: 6

                Text {
                    text: qsTr("Transfers")
                    color: Theme.text
                    font.bold: true
                }
                Text {
                    Layout.fillWidth: true
                    text: popup.statusText()
                    elide: Text.ElideRight
                    color: Theme.textSecondary
                    font.pixelSize: Theme.smallFontSize
                }
                ULink {
                    text: qsTr("Clear Finished")
                    font.pixelSize: Theme.smallFontSize
                    visible: popup.summary.count > popup.summary.active
                    onClicked: App.transfers.clearFinished(popup.devId)
                }
                HeaderButton {
                    kind: "minimize"
                    tip: qsTr("Minimize")
                    onClicked: popup.minimized = true
                }
                HeaderButton {
                    kind: popup.maximized ? "restore" : "maximize"
                    tip: popup.maximized ? qsTr("Restore") : qsTr("Maximize")
                    onClicked: popup.maximized = !popup.maximized
                }
                HeaderButton {
                    kind: "close"
                    tip: qsTr("Hide")
                    onClicked: popup.open = false
                }
            }
            Bar {
                Layout.fillWidth: true
                Layout.leftMargin: 12
                Layout.rightMargin: 12
                Layout.bottomMargin: 6
                visible: popup.summary.active > 0
                value: popup.fraction
            }
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 1
                color: Theme.popupBorder
            }

            ListView {
                id: list
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: App.transfers
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}

                delegate: Item {
                    id: transfer
                    required property real transferId
                    required property string devId
                    required property string fileName
                    required property string localPath
                    required property string remotePath
                    required property real received
                    required property real total
                    required property string transferState
                    required property string error
                    required property string direction

                    readonly property bool mine: devId === popup.devId
                    readonly property bool active: transferState === "active"
                    width: ListView.view.width
                    height: mine ? 44 : 0
                    visible: mine

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        spacing: 10

                        Text {
                            text: transfer.direction === "upload" ? "↑" : "↓"
                            color: transfer.transferState === "failed" ? Theme.error
                                   : transfer.transferState === "done" ? Theme.success : Theme.textSecondary
                            font.pixelSize: 15
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 3
                            RowLayout {
                                spacing: 8
                                Text {
                                    Layout.fillWidth: true
                                    text: transfer.fileName
                                    textFormat: Text.PlainText
                                    elide: Text.ElideMiddle
                                    color: Theme.text
                                }
                                Text {
                                    textFormat: Text.PlainText
                                    elide: Text.ElideRight
                                    Layout.maximumWidth: list.width * 0.45
                                    font.pixelSize: Theme.smallFontSize
                                    color: transfer.transferState === "failed" ? Theme.error
                                           : transfer.transferState === "done" ? Theme.success : Theme.textSecondary
                                    text: {
                                        switch (transfer.transferState) {
                                        case "active":
                                            return transfer.total >= 0
                                                   ? qsTr("%1 of %2").arg(Theme.formatBytes(transfer.received))
                                                                     .arg(Theme.formatBytes(transfer.total))
                                                   : qsTr("Waiting...")
                                        case "done":
                                            return transfer.direction === "upload" ? qsTr("Uploaded") : qsTr("Done")
                                        case "canceled": return qsTr("Canceled")
                                        }
                                        return transfer.error
                                    }
                                }
                            }
                            Bar {
                                Layout.fillWidth: true
                                visible: transfer.active
                                value: transfer.total > 0 ? transfer.received / transfer.total : 0
                            }
                            Text {
                                Layout.fillWidth: true
                                visible: !transfer.active && popup.maximized
                                textFormat: Text.PlainText
                                elide: Text.ElideMiddle
                                color: Theme.textDisabled
                                font.pixelSize: Theme.smallFontSize
                                text: transfer.direction === "upload" ? transfer.remotePath : transfer.localPath
                            }
                        }
                        ULink {
                            visible: transfer.active
                            text: qsTr("Cancel")
                            font.pixelSize: Theme.smallFontSize
                            onClicked: App.storageCancel(transfer.transferId)
                        }
                        ULink {
                            visible: transfer.transferState === "done" && transfer.direction === "download"
                            text: qsTr("Show")
                            font.pixelSize: Theme.smallFontSize
                            onClicked: App.showInFolder(transfer.localPath)
                        }
                    }
                }

                Text {
                    anchors.centerIn: parent
                    visible: popup.summary.count === 0
                    text: qsTr("Uploads and downloads appear here.")
                    color: Theme.textSecondary
                }
            }
        }
    }
}
