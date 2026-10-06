import QtQuick
import QtQuick.Controls

Button {
    id: control

    // The one default action of a form or dialog: filled with the accent colour.
    property bool primary: false

    implicitHeight: Theme.controlHeight
    leftPadding: 14
    rightPadding: 14
    topPadding: 0
    bottomPadding: 0
    font.pixelSize: Theme.fontSize
    hoverEnabled: true

    contentItem: Text {
        text: control.text
        font: control.font
        elide: Text.ElideRight
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        color: !control.enabled ? Theme.textDisabled : control.primary ? "#FFFFFF" : Theme.text
    }

    background: Rectangle {
        implicitWidth: 72
        radius: Theme.radius
        color: {
            if (control.primary) {
                if (!control.enabled) return Theme.pressed
                return control.down ? Theme.accentPressed : control.hovered ? Theme.accentHover : Theme.accent
            }
            return control.down ? Theme.pressed : control.hovered && control.enabled ? Theme.hover : "transparent"
        }
        border.width: control.visualFocus ? 2 : (control.primary ? 0 : 1)
        border.color: control.visualFocus ? Theme.focus : Theme.inputBorder
    }
}
