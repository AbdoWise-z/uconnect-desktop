import QtQuick

// An inline text action, like the links in JetBrains dialogs.
Text {
    id: link

    signal clicked()

    font.pixelSize: Theme.fontSize
    color: area.containsMouse ? Qt.lighter(Theme.link, 1.15) : Theme.link
    font.underline: area.containsMouse

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: link.clicked()
    }
}
