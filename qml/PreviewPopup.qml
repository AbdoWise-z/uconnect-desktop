import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import UConnect

// A quick look at a file on another device: fetched to a temporary folder,
// then shown as an image or as text. Space or Escape closes it.
Item {
    id: popup

    required property string devId
    property bool opened: false
    property string name: ""
    property real batchId: -1
    property string state_: ""  // "fetching" | "image" | "text" | "none" | "error" | "large"
    property string url: ""
    property string text: ""
    property bool truncated: false
    property string error: ""

    signal openRequested(string name)

    visible: opened

    readonly property var imageTypes: ["png", "jpg", "jpeg", "gif", "bmp", "webp", "svg", "ico"]

    // `batch`: what App.storagePreview() returned; -1 when the file is too
    // large to preview.
    function show(fileName, batch) {
        name = fileName
        batchId = batch
        url = ""
        text = ""
        error = ""
        state_ = batch < 0 ? "large" : "fetching"
        opened = true
    }
    function close() {
        if (state_ === "fetching") App.cancelBatch(batchId)
        opened = false
        batchId = -1
    }

    Connections {
        target: App
        ignoreUnknownSignals: true
        function onStoragePreviewReady(batch, fileUrl, err) {
            if (batch !== popup.batchId) return
            if (err.length > 0) {
                popup.error = err
                popup.state_ = "error"
                return
            }
            popup.url = fileUrl
            const ext = popup.name.split(".").pop().toLowerCase()
            if (popup.imageTypes.indexOf(ext) >= 0) {
                popup.state_ = "image"
                return
            }
            const t = App.previewText(fileUrl)
            if (t.binary) {
                popup.state_ = "none"
            } else {
                popup.text = t.text
                popup.truncated = t.truncated
                popup.state_ = "text"
            }
        }
    }

    Keys.onEscapePressed: close()

    Rectangle {
        anchors.fill: parent
        radius: 8
        color: Theme.popup
        border.color: Theme.popupBorder

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 1
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: 36
                Layout.leftMargin: 12
                Layout.rightMargin: 6
                spacing: 8
                Text {
                    Layout.fillWidth: true
                    text: popup.name
                    textFormat: Text.PlainText
                    elide: Text.ElideMiddle
                    color: Theme.text
                    font.bold: true
                }
                ULink {
                    text: qsTr("Open")
                    font.pixelSize: Theme.smallFontSize
                    onClicked: {
                        popup.openRequested(popup.name)
                        popup.close()
                    }
                }
                UToolButton {
                    text: "×"
                    tip: qsTr("Close (Space)")
                    onClicked: popup.close()
                }
            }
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 1
                color: Theme.popupBorder
            }

            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                Image {
                    id: image
                    anchors.fill: parent
                    anchors.margins: 12
                    visible: popup.state_ === "image" && status === Image.Ready
                    source: popup.state_ === "image" ? popup.url : ""
                    fillMode: Image.PreserveAspectFit
                    asynchronous: true
                    autoTransform: true
                    sourceSize.width: 2048
                    sourceSize.height: 2048
                }

                ScrollView {
                    anchors.fill: parent
                    anchors.margins: 1
                    visible: popup.state_ === "text"
                    TextArea {
                        readOnly: true
                        selectByMouse: true
                        textFormat: TextEdit.PlainText
                        wrapMode: TextEdit.NoWrap
                        font.family: "Consolas"
                        color: Theme.text
                        background: null
                        text: popup.text + (popup.truncated ? "\n\n" + qsTr("[Only the start of the file is shown.]") : "")
                    }
                }

                Text {
                    anchors.centerIn: parent
                    width: parent.width - 64
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    textFormat: Text.PlainText
                    color: popup.state_ === "error" ? Theme.error : Theme.textSecondary
                    visible: text.length > 0
                    text: popup.state_ === "fetching" ? qsTr("Fetching...")
                          : popup.state_ === "large" ? qsTr("This file is too large to preview. Open it instead.")
                          : popup.state_ === "none" ? qsTr("There is no preview for this kind of file. Open it instead.")
                          : popup.state_ === "error" ? popup.error
                          : popup.state_ === "image" && image.status === Image.Error ? qsTr("This image cannot be shown.")
                          : ""
                }
            }
        }
    }
}
