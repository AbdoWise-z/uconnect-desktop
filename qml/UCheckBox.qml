import QtQuick
import QtQuick.Controls

CheckBox {
    id: control

    font.pixelSize: Theme.fontSize
    spacing: 8
    padding: 2
    hoverEnabled: true

    indicator: Rectangle {
        x: control.leftPadding
        anchors.verticalCenter: parent.verticalCenter
        implicitWidth: 16
        implicitHeight: 16
        radius: 3
        color: control.checked && control.enabled ? Theme.accent : Theme.input
        border.width: control.checked && control.enabled ? 0 : (control.visualFocus ? 2 : 1)
        border.color: control.visualFocus ? Theme.focus : Theme.inputBorder
        opacity: control.enabled ? 1 : 0.5

        Canvas {
            anchors.fill: parent
            visible: control.checked
            onVisibleChanged: requestPaint()
            onPaint: {
                const ctx = getContext("2d")
                ctx.reset()
                ctx.strokeStyle = control.enabled ? "#FFFFFF" : Theme.textSecondary
                ctx.lineWidth = 1.8
                ctx.lineCap = "round"
                ctx.lineJoin = "round"
                ctx.beginPath()
                ctx.moveTo(width * 0.25, height * 0.52)
                ctx.lineTo(width * 0.43, height * 0.70)
                ctx.lineTo(width * 0.76, height * 0.32)
                ctx.stroke()
            }
        }
    }

    contentItem: Text {
        leftPadding: control.indicator.width + control.spacing
        text: control.text
        font: control.font
        color: control.enabled ? Theme.text : Theme.textDisabled
        verticalAlignment: Text.AlignVCenter
    }
}
