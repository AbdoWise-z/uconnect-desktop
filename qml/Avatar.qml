import QtQuick

// Initials on a coloured square, like JetBrains project icons.
Rectangle {
    id: avatar

    property string name: ""
    property string seed: name
    property int size: 20

    implicitWidth: size
    implicitHeight: size
    radius: Math.max(3, size / 5)
    color: Theme.avatarColor(seed)

    Text {
        anchors.centerIn: parent
        text: Theme.initials(avatar.name)
        textFormat: Text.PlainText
        color: "#FFFFFF"
        font.pixelSize: Math.round(avatar.size * 0.42)
        font.bold: true
    }
}
