import QtQuick
import QtQuick.Layouts

// An inline notice: "error" or "info".
Rectangle {
    id: banner

    property string kind: "info"
    property alias text: message.text
    default property alias actions: actionRow.data

    implicitHeight: row.implicitHeight + 16
    radius: Theme.radius
    color: kind === "error" ? Theme.errorBackground : Theme.infoBackground
    border.color: kind === "error" ? Theme.error : Theme.accent

    RowLayout {
        id: row
        anchors.fill: parent
        anchors.margins: 8
        anchors.leftMargin: 12
        spacing: 12

        Text {
            id: message
            Layout.fillWidth: true
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.fontSize
            color: Theme.text
        }
        RowLayout {
            id: actionRow
            spacing: 6
        }
    }
}
