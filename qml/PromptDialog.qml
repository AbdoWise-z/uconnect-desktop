import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import UConnect

// A small modal question: a title, a message, optionally a text field, and a
// row of choices. Closing it (Escape, or clicking outside) chooses nothing.
Popup {
    id: dialog

    property string title: ""
    property string message: ""
    property bool withField: false
    property alias fieldText: field.text
    // [{id, label, primary?, danger?}], shown right to left in reading order.
    property var choices: []

    signal chosen(string choiceId, string value)

    function ask(newTitle, newMessage, newChoices, initialText) {
        title = newTitle
        message = newMessage
        choices = newChoices
        withField = initialText !== undefined
        field.text = withField ? initialText : ""
        open()
        if (withField) {
            field.forceActiveFocus()
            // Select the name without its extension, as file managers do.
            const dot = field.text.lastIndexOf(".")
            field.select(0, dot > 0 ? dot : field.text.length)
        }
    }

    function choose(choiceId) {
        const value = field.text.trim()
        if (withField && value.length === 0) return
        close()
        chosen(choiceId, value)
    }

    anchors.centerIn: Overlay.overlay
    width: Math.min(460, parent ? parent.width - 48 : 460)
    modal: true
    padding: 20
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    background: Rectangle {
        color: Theme.panel
        border.color: Theme.popupBorder
        radius: 8
    }

    ColumnLayout {
        width: parent.width
        spacing: 12

        Text {
            Layout.fillWidth: true
            text: dialog.title
            textFormat: Text.PlainText
            elide: Text.ElideRight
            color: Theme.text
            font.pixelSize: 15
            font.bold: true
        }
        Text {
            Layout.fillWidth: true
            visible: dialog.message.length > 0
            text: dialog.message
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: Theme.textSecondary
        }
        UTextField {
            id: field
            Layout.fillWidth: true
            visible: dialog.withField
            maximumLength: 200
            onAccepted: {
                const primary = dialog.choices.find(c => c.primary)
                if (primary) dialog.choose(primary.id)
            }
        }
        RowLayout {
            Layout.topMargin: 6
            spacing: 8
            Item { Layout.fillWidth: true }
            UButton {
                text: qsTr("Cancel")
                onClicked: dialog.close()
            }
            Repeater {
                model: dialog.choices
                delegate: UButton {
                    required property var modelData
                    text: modelData.label
                    primary: modelData.primary === true
                    enabled: !dialog.withField || field.text.trim().length > 0
                    onClicked: dialog.choose(modelData.id)
                }
            }
        }
    }
}
