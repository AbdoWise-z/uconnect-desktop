import QtQuick
import QtQuick.Controls

MenuSeparator {
    implicitHeight: visible ? implicitContentHeight + topPadding + bottomPadding : 0
    topPadding: 4
    bottomPadding: 4
    leftPadding: 6
    rightPadding: 6
    contentItem: Rectangle {
        implicitWidth: 200
        implicitHeight: 1
        color: Theme.popupBorder
    }
}
