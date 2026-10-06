import QtQuick
import QtQuick.Controls

Menu {
    id: menu

    padding: 4
    delegate: UMenuItem {}

    background: Rectangle {
        implicitWidth: 220
        color: Theme.popup
        border.color: Theme.popupBorder
        radius: 6
    }
}
