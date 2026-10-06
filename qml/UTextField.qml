import QtQuick
import QtQuick.Controls

TextField {
    id: control

    property bool invalid: false

    implicitHeight: Theme.controlHeight
    leftPadding: 8
    rightPadding: 8
    topPadding: 0
    bottomPadding: 0
    verticalAlignment: TextInput.AlignVCenter
    font.pixelSize: Theme.fontSize
    color: enabled ? Theme.text : Theme.textSecondary
    placeholderTextColor: Theme.textDisabled
    selectionColor: Theme.selection
    selectedTextColor: Theme.text
    selectByMouse: true

    background: Rectangle {
        implicitWidth: 200
        radius: Theme.radius
        color: control.enabled ? Theme.input : "transparent"
        border.width: control.activeFocus ? 2 : 1
        border.color: control.invalid ? Theme.error : control.activeFocus ? Theme.focus : Theme.inputBorder
    }
}
