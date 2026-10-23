import QtQuick
import QtQuick.Controls

MenuItem {
    id: item

    // Right-aligned secondary text; defaults to the action's shortcut.
    property string hint: action && action.shortcut ? String(action.shortcut) : ""
    // A UIcon name drawn in the left gutter (where a check mark goes).
    property string glyph: ""

    implicitWidth: Math.max(220, leftPadding + label.implicitWidth + 32 + hintLabel.implicitWidth + rightPadding)
    implicitHeight: visible ? 26 : 0
    leftPadding: 26
    rightPadding: subMenu ? 22 : 10
    topPadding: 0
    bottomPadding: 0
    font.pixelSize: Theme.fontSize

    indicator: UIcon {
        x: 6
        anchors.verticalCenter: parent.verticalCenter
        size: 14
        name: item.checkable && item.checked ? "check" : item.glyph
        visible: name.length > 0
        color: item.enabled ? (item.checkable ? Theme.text : Theme.textSecondary) : Theme.textDisabled
    }

    arrow: Text {
        x: item.width - width - 8
        anchors.verticalCenter: parent.verticalCenter
        visible: item.subMenu !== null
        text: "›"
        font.pixelSize: 16
        color: item.enabled ? Theme.textSecondary : Theme.textDisabled
    }

    contentItem: Item {
        implicitHeight: 26
        Text {
            id: label
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            text: item.text.replace("&", "")
            textFormat: Text.PlainText
            font: item.font
            color: item.enabled ? Theme.text : Theme.textDisabled
        }
        Text {
            id: hintLabel
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            text: item.hint
            visible: text.length > 0 && !item.subMenu
            font.pixelSize: Theme.smallFontSize
            color: Theme.textSecondary
        }
    }

    background: Rectangle {
        implicitWidth: 220
        radius: Theme.radius
        color: item.highlighted && item.enabled ? Theme.selection : "transparent"
    }
}
