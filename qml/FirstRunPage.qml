import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import UConnect

Rectangle {
    id: page

    readonly property bool valid: App.isValidServer(serverField.text) && nameField.text.trim().length > 0

    function finish() {
        if (valid) App.completeFirstRun(serverField.text, nameField.text)
    }

    color: Theme.canvas

    ColumnLayout {
        anchors.centerIn: parent
        width: 460
        spacing: 8

        RowLayout {
            spacing: 14
            Layout.bottomMargin: 12
            Avatar {
                name: "u Connect"
                seed: "uConnect"
                size: 44
            }
            ColumnLayout {
                spacing: 2
                Text {
                    text: qsTr("Welcome to uConnect")
                    color: Theme.text
                    font.pixelSize: 22
                }
                Text {
                    text: qsTr("Set up this device")
                    color: Theme.textSecondary
                }
            }
        }

        Text {
            Layout.fillWidth: true
            Layout.bottomMargin: 10
            wrapMode: Text.WordWrap
            color: Theme.textSecondary
            text: qsTr("Your devices find each other through a rendezvous server. Enter the one you use; it cannot read what your devices send each other.")
        }

        Text {
            text: qsTr("Rendezvous server")
            color: Theme.text
        }
        UTextField {
            id: serverField
            Layout.fillWidth: true
            focus: true
            placeholderText: qsTr("host:port, for example rv.example.com:4433")
            invalid: text.length > 0 && !App.isValidServer(text)
            onAccepted: page.finish()
        }
        Text {
            Layout.bottomMargin: 8
            font.pixelSize: Theme.smallFontSize
            color: serverField.invalid ? Theme.error : Theme.textSecondary
            text: serverField.invalid ? qsTr("Use the form host:port.") : qsTr("A host name or IP address, then the port.")
        }

        Text {
            text: qsTr("Device name")
            color: Theme.text
        }
        UTextField {
            id: nameField
            Layout.fillWidth: true
            text: App.deviceName
            maximumLength: 64
            onAccepted: page.finish()
        }
        Text {
            font.pixelSize: Theme.smallFontSize
            color: Theme.textSecondary
            text: qsTr("How this device appears to your other devices.")
        }

        RowLayout {
            Layout.topMargin: 18
            Text {
                Layout.fillWidth: true
                font.pixelSize: Theme.smallFontSize
                color: Theme.textSecondary
                text: qsTr("You can change these later in Settings (Ctrl+Alt+S).")
            }
            UButton {
                text: qsTr("Continue")
                primary: true
                enabled: page.valid
                onClicked: page.finish()
            }
        }
    }
}
