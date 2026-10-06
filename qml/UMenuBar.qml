import QtQuick
import QtQuick.Controls

MenuBar {
    id: bar

    leftPadding: 6
    topPadding: 3
    bottomPadding: 3

    delegate: MenuBarItem {
        id: entry
        implicitHeight: 26
        leftPadding: 9
        rightPadding: 9
        font.pixelSize: Theme.fontSize
        contentItem: Text {
            text: entry.text.replace("&", "")
            font: entry.font
            color: entry.enabled ? Theme.text : Theme.textDisabled
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            radius: Theme.radius
            color: entry.highlighted ? Theme.hover : "transparent"
        }
    }

    background: Rectangle {
        implicitHeight: 32
        color: Theme.panel
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 1
            color: Theme.divider
        }
    }
}
