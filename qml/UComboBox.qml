import QtQuick
import QtQuick.Controls

ComboBox {
    id: control

    implicitWidth: Math.max(160, implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Theme.controlHeight
    leftPadding: mirrored ? 30 : 8
    rightPadding: mirrored ? 8 : 30
    topPadding: 0
    bottomPadding: 0
    font.pixelSize: Theme.fontSize
    hoverEnabled: true

    contentItem: Text {
        text: control.displayText
        textFormat: Text.PlainText
        font: control.font
        color: control.enabled ? Theme.text : Theme.textDisabled
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    indicator: Canvas {
        x: control.mirrored ? 8 : control.width - width - 8
        y: (control.height - height) / 2
        width: 14
        height: 14
        opacity: control.enabled ? 1 : 0.4
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            ctx.strokeStyle = Theme.textSecondary
            ctx.lineWidth = 1.5
            ctx.lineCap = "round"
            ctx.lineJoin = "round"
            ctx.beginPath()
            ctx.moveTo(3, 5)
            ctx.lineTo(7, 9)
            ctx.lineTo(11, 5)
            ctx.stroke()
        }
    }

    background: Rectangle {
        radius: Theme.radius
        color: control.down ? Theme.pressed : control.hovered && control.enabled ? Theme.hover : Theme.input
        border.width: control.visualFocus ? 2 : 1
        border.color: control.visualFocus ? Theme.focus : Theme.inputBorder
        opacity: control.enabled ? 1 : 0.5
    }

    delegate: ItemDelegate {
        id: option
        required property int index
        width: control.width - 8
        height: Theme.controlHeight
        leftPadding: 8
        rightPadding: 8
        font: control.font
        hoverEnabled: true
        highlighted: control.highlightedIndex === index
        contentItem: Text {
            text: control.textAt(option.index)
            textFormat: Text.PlainText
            font: option.font
            color: Theme.text
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
        background: Rectangle {
            radius: Theme.radius
            color: option.highlighted ? Theme.selection : option.hovered ? Theme.hover : "transparent"
        }
    }

    popup: Popup {
        y: control.height + 4
        width: control.width
        padding: 4
        contentItem: ListView {
            clip: true
            implicitHeight: Math.min(contentHeight, 240)
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
            boundsBehavior: Flickable.StopAtBounds
            ScrollIndicator.vertical: ScrollIndicator {
                contentItem: Rectangle { implicitWidth: 3; implicitHeight: 20; radius: 1; color: Theme.textSecondary }
            }
        }
        background: Rectangle {
            radius: Theme.radius
            color: Theme.popup
            border.color: Theme.popupBorder
        }
    }
}
