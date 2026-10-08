import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import UConnect

Item {
    id: safety
    property var controller: App.screen
    property bool camera: false
    Window {
        id: consent
        property var request: safety.controller.consents.length ? safety.controller.consents[0] : ({})
        title: qsTr("Allow screen sharing?")
        transientParent: null
        width: 460
        height: 270
        flags: Qt.Dialog | Qt.WindowStaysOnTopHint | Qt.WindowTitleHint | Qt.WindowCloseButtonHint
        visible: safety.controller.consents.length > 0
        color: Theme.panel
        onVisibleChanged: if (visible) { raise(); requestActivate(); remember.checked = false }
        onRequestChanged: remember.checked = false
        onClosing: event => { event.accepted = false; safety.controller.consent(request.peer, request.request, false, false) }
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 24
            spacing: 12
            Text {
                Layout.fillWidth: true
                text: safety.camera ? qsTr("%1 wants to view your camera").arg(consent.request.name || "") : consent.request.controlOnly ? qsTr("%1 wants to control your mouse and keyboard").arg(consent.request.name || "")
                    : consent.request.control ? qsTr("%1 wants to view and control your screen").arg(consent.request.name || "")
                    : qsTr("%1 wants to view your screen").arg(consent.request.name || "")
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                color: Theme.text
                font.pixelSize: 16
            }
            Text {
                Layout.fillWidth: true
                text: consent.request.fingerprint || ""
                textFormat: Text.PlainText
                color: Theme.textSecondary
                wrapMode: Text.WrapAnywhere
            }
            Text { text: qsTr("This request expires after 30 seconds."); color: Theme.textSecondary }
            UCheckBox { id: remember; visible: !safety.camera; text: qsTr("Don't ask again for this trusted device") }
            Item { Layout.fillHeight: true }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                UButton { text: qsTr("Deny"); onClicked: safety.controller.consent(consent.request.peer, consent.request.request, false, false) }
                UButton { text: qsTr("Allow"); primary: true; onClicked: safety.controller.consent(consent.request.peer, consent.request.request, true, remember.checked) }
            }
        }
    }
    Window {
        title: qsTr("Screen sharing active")
        transientParent: null
        width: 580
        height: safety.controller.shortcutError.length ? 110 : 68
        flags: Qt.Tool | Qt.WindowStaysOnTopHint | Qt.WindowTitleHint
        visible: safety.controller.hosting.length > 0
        color: Theme.panel
        onClosing: event => { event.accepted = false; safety.controller.stopHosting() }
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 12
            RowLayout {
                Layout.fillWidth: true
                StatusDot { color: Theme.warning }
                Text {
                    Layout.fillWidth: true
                    text: (safety.camera ? qsTr("Sharing your camera with %1") : qsTr("Sharing your screen with %1")).arg(safety.controller.hosting.map(p => p.name).join(", "))
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    color: Theme.text
                }
                UButton { text: qsTr("Stop sharing"); onClicked: safety.controller.stopHosting() }
                UButton {
                    visible: safety.controller.hosting.some(p => p.control)
                    text: qsTr("Stop control")
                    onClicked: safety.controller.stopControl()
                }
            }
            Text {
                Layout.fillWidth: true
                visible: safety.controller.shortcutError.length > 0
                text: safety.controller.shortcutError
                color: Theme.warning
                wrapMode: Text.WordWrap
            }
        }
    }
}
