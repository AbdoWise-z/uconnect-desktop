import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import UConnect

// Extended display prompts and indicators: the display device's consent
// prompt and full-screen views, and the host's always-visible "extending" bar.
Item {
    id: safety
    readonly property var controller: App.display
    property var views: ({})  // devId -> DisplayView

    Component {
        id: viewComponent
        DisplayView {}
    }
    Connections {
        target: safety.controller
        function onShowRequested(devId, screen) {
            const old = safety.views[devId]
            if (old) old.destroy()
            const view = viewComponent.createObject(null, {devId: devId, screenIndex: screen})
            view.closing.connect(() => {
                if (safety.views[devId] === view) delete safety.views[devId]
                Qt.callLater(view.destroy)
            })
            safety.views[devId] = view
        }
    }

    Window {
        id: consent
        property var request: safety.controller.consents.length ? safety.controller.consents[0] : ({})
        readonly property var screens: request.screens || []
        title: qsTr("Use this screen as a display?")
        transientParent: null
        width: 480
        height: 300
        flags: Qt.Dialog | Qt.WindowStaysOnTopHint | Qt.WindowTitleHint | Qt.WindowCloseButtonHint
        visible: safety.controller.consents.length > 0
        color: Theme.panel
        onVisibleChanged: if (visible) { raise(); requestActivate(); remember.checked = false; screenChoice.currentIndex = 0 }
        onClosing: event => { event.accepted = false; safety.controller.consent(request.peer, request.request, false, false, 0) }
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 24
            spacing: 12
            Text {
                Layout.fillWidth: true
                text: qsTr("%1 wants to use this screen as an extra monitor").arg(consent.request.name || "")
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
            RowLayout {
                visible: consent.screens.length > 1
                Text { text: qsTr("Show it on"); color: Theme.text }
                UComboBox {
                    id: screenChoice
                    Layout.preferredWidth: 260
                    model: consent.screens.map(s => qsTr("%1 (%2 × %3)").arg(s.name).arg(s.width).arg(s.height))
                }
            }
            Text { text: qsTr("It opens full screen; hold Esc to stop. This request expires after 30 seconds."); color: Theme.textSecondary; wrapMode: Text.WordWrap; Layout.fillWidth: true }
            UCheckBox { id: remember; text: qsTr("Don't ask again for this trusted device") }
            Item { Layout.fillHeight: true }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                UButton { text: qsTr("Deny"); onClicked: safety.controller.consent(consent.request.peer, consent.request.request, false, false, 0) }
                UButton {
                    text: qsTr("Allow")
                    primary: true
                    onClicked: safety.controller.consent(consent.request.peer, consent.request.request, true, remember.checked, Math.max(0, screenChoice.currentIndex))
                }
            }
        }
    }

    // The host: a monitor exists only for uConnect, so say so while it does.
    Window {
        title: qsTr("Extended display active")
        transientParent: null
        width: 520
        height: 68
        flags: Qt.Tool | Qt.WindowStaysOnTopHint | Qt.WindowTitleHint | Qt.WindowDoesNotAcceptFocus
        visible: safety.controller.hosting.length > 0
        color: Theme.panel
        onClosing: event => { event.accepted = false; safety.controller.stopAll() }
        RowLayout {
            anchors.fill: parent
            anchors.margins: 12
            StatusDot { color: safety.controller.hosting.some(h => h.state === "live") ? Theme.success : Theme.warning }
            Text {
                Layout.fillWidth: true
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: Theme.text
                text: qsTr("Extending to %1").arg(safety.controller.hosting.map(h => {
                    const p = App.peers.get(h.devId); return p.displayName !== undefined ? p.displayName : h.devId.substring(0, 8)
                }).join(", "))
            }
            UButton { text: qsTr("Stop"); onClicked: safety.controller.stopAll() }
        }
    }
}
