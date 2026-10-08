import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import UConnect

Item {
    Window {
        id: consent
        property var request: App.screen.consents.length ? App.screen.consents[0] : ({})
        title: qsTr("Allow screen sharing?")
        transientParent: null
        width: 460
        height: 270
        flags: Qt.Dialog | Qt.WindowStaysOnTopHint | Qt.WindowTitleHint | Qt.WindowCloseButtonHint
        visible: App.screen.consents.length > 0
        color: Theme.panel
        onVisibleChanged: if (visible) { raise(); requestActivate(); remember.checked = false }
        onRequestChanged: remember.checked = false
        onClosing: event => { event.accepted = false; App.screen.consent(request.peer, request.request, false, false) }
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 24
            spacing: 12
            Text {
                Layout.fillWidth: true
                text: consent.request.controlOnly ? qsTr("%1 wants to control your mouse and keyboard").arg(consent.request.name || "")
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
            UCheckBox { id: remember; text: qsTr("Don't ask again for this trusted device") }
            Item { Layout.fillHeight: true }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                UButton { text: qsTr("Deny"); onClicked: App.screen.consent(consent.request.peer, consent.request.request, false, false) }
                UButton { text: qsTr("Allow"); primary: true; onClicked: App.screen.consent(consent.request.peer, consent.request.request, true, remember.checked) }
            }
        }
    }
    Window {
        title: qsTr("Screen sharing active")
        transientParent: null
        width: 580
        height: App.screen.shortcutError.length ? 110 : 68
        flags: Qt.Tool | Qt.WindowStaysOnTopHint | Qt.WindowTitleHint
        visible: App.screen.hosting.length > 0
        color: Theme.panel
        onClosing: event => { event.accepted = false; App.screen.stopHosting() }
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 12
            RowLayout {
                Layout.fillWidth: true
                StatusDot { color: Theme.warning }
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Sharing your screen with %1").arg(App.screen.hosting.map(p => p.name).join(", "))
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    color: Theme.text
                }
                UButton { text: qsTr("Stop sharing"); onClicked: App.screen.stopHosting() }
                UButton {
                    visible: App.screen.hosting.some(p => p.control)
                    text: qsTr("Stop control")
                    onClicked: App.screen.stopControl()
                }
            }
            Text {
                Layout.fillWidth: true
                visible: App.screen.shortcutError.length > 0
                text: App.screen.shortcutError
                color: Theme.warning
                wrapMode: Text.WordWrap
            }
        }
    }
}
