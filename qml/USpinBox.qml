import QtQuick
import QtQuick.Controls

SpinBox {
    id: control

    implicitWidth: 132
    implicitHeight: Theme.controlHeight
    leftPadding: Theme.controlHeight + 2
    rightPadding: Theme.controlHeight + 2
    topPadding: 0
    bottomPadding: 0
    font.pixelSize: Theme.fontSize
    editable: true
    hoverEnabled: true

    contentItem: TextInput {
        text: control.displayText
        font: control.font
        color: control.enabled ? Theme.text : Theme.textDisabled
        selectionColor: Theme.selection
        selectedTextColor: Theme.text
        horizontalAlignment: TextInput.AlignHCenter
        verticalAlignment: TextInput.AlignVCenter
        readOnly: !control.editable
        validator: control.validator
        inputMethodHints: Qt.ImhFormattedNumbersOnly
        selectByMouse: true
        clip: true
    }

    up.indicator: Rectangle {
        x: control.mirrored ? 1 : control.width - width - 1
        y: 1
        width: Theme.controlHeight - 2
        height: control.height - 2
        radius: Theme.radius - 1
        color: !enabled ? "transparent" : control.up.pressed ? Theme.pressed
               : control.up.hovered ? Theme.hover : "transparent"
        Text {
            anchors.centerIn: parent
            text: "+"
            font.pixelSize: 18
            color: enabled ? Theme.text : Theme.textDisabled
        }
    }
    down.indicator: Rectangle {
        x: control.mirrored ? control.width - width - 1 : 1
        y: 1
        width: Theme.controlHeight - 2
        height: control.height - 2
        radius: Theme.radius - 1
        color: !enabled ? "transparent" : control.down.pressed ? Theme.pressed
               : control.down.hovered ? Theme.hover : "transparent"
        Text {
            anchors.centerIn: parent
            text: "\u2212"
            font.pixelSize: 18
            color: enabled ? Theme.text : Theme.textDisabled
        }
    }

    background: Rectangle {
        radius: Theme.radius
        color: control.enabled ? Theme.input : "transparent"
        border.width: control.activeFocus || control.contentItem.activeFocus ? 2 : 1
        border.color: control.activeFocus || control.contentItem.activeFocus ? Theme.focus : Theme.inputBorder
    }
}
