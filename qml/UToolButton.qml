import QtQuick
import QtQuick.Controls

// A borderless, compact button for toolbars and tool window headers.
ToolButton {
    id: control

    property string tip: ""

    implicitHeight: 24
    implicitWidth: Math.max(24, contentItem.implicitWidth + leftPadding + rightPadding)
    leftPadding: 6
    rightPadding: 6
    topPadding: 0
    bottomPadding: 0
    font.pixelSize: Theme.fontSize
    hoverEnabled: true

    ToolTip.visible: tip.length > 0 && hovered
    ToolTip.delay: 600
    ToolTip.text: tip

    contentItem: Text {
        text: control.text
        font: control.font
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        color: control.enabled ? Theme.text : Theme.textDisabled
    }

    background: Rectangle {
        radius: Theme.radius
        color: control.down || control.checked ? Theme.pressed
                                               : control.hovered && control.enabled ? Theme.hover : "transparent"
    }
}
