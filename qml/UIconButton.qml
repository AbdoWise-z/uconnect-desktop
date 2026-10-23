import QtQuick
import QtQuick.Controls

// A compact, borderless icon button for toolbars (see UIcon for the glyphs).
ToolButton {
    id: control

    property string glyph: ""  // a UIcon name (`icon` is taken by AbstractButton)
    property string tip: ""
    property int iconSize: 17

    implicitHeight: 28
    implicitWidth: 30
    padding: 0
    hoverEnabled: true

    ToolTip.visible: tip.length > 0 && hovered
    ToolTip.delay: 600
    ToolTip.text: tip

    contentItem: UIcon {
        name: control.glyph
        size: control.iconSize
        color: control.enabled ? Theme.text : Theme.textDisabled
    }

    background: Rectangle {
        radius: Theme.radius
        color: control.down || control.checked ? Theme.pressed
                                               : control.hovered && control.enabled ? Theme.hover : "transparent"
    }
}
