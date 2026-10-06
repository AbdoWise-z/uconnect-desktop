import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import UConnect

// Asks the user to compare fingerprints before trusting a device. Trust pins
// the device's public key, so this is the one moment a person checks it.
Popup {
    id: dialog

    property string devId: ""
    readonly property var peer: { App.peers.revision; return App.peers.get(devId) }
    readonly property bool verified: peer.identityState === "verified"

    function openFor(id) {
        devId = id
        open()
    }

    anchors.centerIn: Overlay.overlay
    width: 520
    modal: true
    padding: 24
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    background: Rectangle {
        color: Theme.panel
        border.color: Theme.popupBorder
        radius: 8
    }

    ColumnLayout {
        width: parent.width
        spacing: 12

        RowLayout {
            spacing: 12
            Avatar {
                name: dialog.peer.displayName !== undefined ? dialog.peer.displayName : "?"
                seed: dialog.peer.identityKey ? dialog.peer.identityKey : dialog.devId
                size: 36
            }
            ColumnLayout {
                spacing: 2
                Text {
                    text: qsTr("Trust %1?").arg(dialog.peer.displayName !== undefined ? dialog.peer.displayName : qsTr("this device"))
                    textFormat: Text.PlainText
                    color: Theme.text
                    font.pixelSize: 16
                    font.bold: true
                }
                Text {
                    text: qsTr("Its name is only what it claims. The fingerprint is what proves it.")
                    color: Theme.textSecondary
                }
            }
        }

        Text {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.text
            textFormat: Text.PlainText
            text: qsTr("On %1, open Settings › Device and check that its fingerprint is exactly:")
                      .arg(dialog.peer.displayName !== undefined ? dialog.peer.displayName : qsTr("that device"))
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: theirs.implicitHeight + 24
            radius: Theme.radius
            color: Theme.canvas
            border.color: Theme.border
            Text {
                id: theirs
                anchors.centerIn: parent
                text: dialog.verified ? dialog.peer.fingerprint : qsTr("Its identity is not verified.")
                color: dialog.verified ? Theme.text : Theme.error
                font.family: "Consolas, monospace"
                font.pixelSize: 18
            }
        }

        Text {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textSecondary
            text: qsTr("If it differs, do not trust it: something is impersonating that device. This device's own fingerprint, for them to check, is %1.")
                      .arg(App.myFingerprint)
        }

        Text {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textSecondary
            text: qsTr("Once trusted, it starts with your default permissions, which you can change afterwards.")
        }

        RowLayout {
            Layout.topMargin: 8
            Item { Layout.fillWidth: true }
            UButton {
                text: qsTr("Cancel")
                onClicked: dialog.close()
            }
            UButton {
                text: qsTr("Fingerprints Match — Trust")
                primary: true
                enabled: dialog.verified
                onClicked: {
                    App.trustDevice(dialog.peer.identityKey, dialog.peer.displayName)
                    dialog.close()
                }
            }
        }
    }
}
