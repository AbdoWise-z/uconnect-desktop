import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import UConnect

// The host's side of an extended display: choose a mode, start, watch the
// session, stop. One window per device.
Window {
    id: win

    required property string devId
    readonly property var controller: App.display
    readonly property var peer: { App.peers.revision; return App.peers.get(devId) }
    readonly property string deviceName: peer.displayName !== undefined ? peer.displayName : qsTr("Unknown device")
    readonly property bool peerConnected: peer.stateCode === Theme.stateConnected
    property var session: controller.hostState(devId)
    readonly property string phase: session.state || ""
    readonly property bool active: phase === "offering" || phase === "waiting" || phase === "creating" || phase === "live"
    readonly property bool driverReady: controller.driver.state === "ready"
    readonly property var presets: [
        {label: qsTr("Match the other screen"), width: 0, height: 0},
        {label: "1920 × 1080", width: 1920, height: 1080},
        {label: "2560 × 1440", width: 2560, height: 1440},
        {label: "2560 × 1600", width: 2560, height: 1600},
        {label: "3840 × 2160", width: 3840, height: 2160},
        {label: qsTr("Custom"), width: -1, height: -1}
    ]

    width: 520
    height: 470
    minimumWidth: 440
    minimumHeight: 400
    color: Theme.canvas
    title: Theme.serviceLabel("display") + " – " + deviceName

    component FieldLabel: Text {
        Layout.preferredWidth: 140
        color: Theme.text
    }

    Connections {
        target: win.controller
        function onHostStateChanged(peer) { if (peer === win.devId) win.session = win.controller.hostState(peer) }
    }
    Component.onCompleted: {
        controller.refreshDriver()
        const saved = controller.settings
        const index = presets.findIndex(p => p.width === saved.width && p.height === saved.height)
        preset.currentIndex = index >= 0 ? index : presets.length - 1
        customWidth.value = saved.width > 0 ? saved.width : 1920
        customHeight.value = saved.height > 0 ? saved.height : 1080
        fps.value = saved.fps || 60
    }

    function chosenMode() {
        const p = presets[preset.currentIndex]
        if (p.width === 0) return {fps: fps.value}
        if (p.width < 0) return {width: customWidth.value, height: customHeight.value, fps: fps.value}
        return {width: p.width, height: p.height, fps: fps.value}
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 12

        RowLayout {
            spacing: 10
            Avatar { name: win.deviceName; seed: win.peer.identityKey ? win.peer.identityKey : win.devId; size: 28 }
            ColumnLayout {
                spacing: 2
                Text { text: win.deviceName; color: Theme.text; font.bold: true; font.pixelSize: 16; textFormat: Text.PlainText }
                Text { text: qsTr("Use this device's screen as an extra monitor"); color: Theme.textSecondary }
            }
        }

        Banner {
            Layout.fillWidth: true
            visible: !win.peerConnected
            kind: "error"
            text: qsTr("%1 is not connected.").arg(win.deviceName)
        }
        Banner {
            Layout.fillWidth: true
            visible: !win.driverReady
            kind: "error"
            text: win.controller.driver.detail || qsTr("Extended displays are not available here.")
        }
        UButton {
            visible: !win.driverReady && win.controller.driver.needsDriver === true
            enabled: win.controller.driver.bundled === true && win.controller.driver.busy !== true
            text: win.controller.driver.busy ? qsTr("Installing…")
                  : win.controller.driver.state === "outdated" ? qsTr("Update the display driver") : qsTr("Install the display driver")
            onClicked: win.controller.installDriver()
        }

        TitledSeparator { text: qsTr("Screen") }
        GridLayout {
            columns: 2
            columnSpacing: 12
            rowSpacing: 8
            enabled: !win.active
            FieldLabel { text: qsTr("Resolution") }
            UComboBox {
                id: preset
                Layout.preferredWidth: 240
                model: win.presets.map(p => p.label)
            }
            FieldLabel { text: qsTr("Width × height"); visible: preset.currentIndex === win.presets.length - 1 }
            RowLayout {
                visible: preset.currentIndex === win.presets.length - 1
                USpinBox { id: customWidth; from: 640; to: 3840; stepSize: 16 }
                Text { text: "×"; color: Theme.textSecondary }
                USpinBox { id: customHeight; from: 480; to: 2400; stepSize: 16 }
            }
            FieldLabel { text: qsTr("Refresh rate (Hz)") }
            USpinBox { id: fps; from: 24; to: 165 }
        }
        Text {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textSecondary
            text: qsTr("Both devices must be on the same network. The other device chooses which of its screens shows it, and the refresh rate is kept within what it and the video codec handle.")
        }

        TitledSeparator { text: qsTr("Session") }
        RowLayout {
            spacing: 8
            StatusDot { color: win.phase === "live" ? Theme.success : win.active ? Theme.warning : Theme.textSecondary }
            Text {
                Layout.fillWidth: true
                color: Theme.text
                textFormat: Text.PlainText
                elide: Text.ElideRight
                text: win.phase === "offering" ? qsTr("Asking %1…").arg(win.deviceName)
                    : win.phase === "waiting" ? qsTr("Waiting for %1 to allow it…").arg(win.deviceName)
                    : win.phase === "creating" ? qsTr("Adding the monitor…")
                    : win.phase === "live" ? qsTr("Extending to %1 · %2 × %3 at %4 Hz").arg(win.deviceName).arg(win.session.width).arg(win.session.height).arg(win.session.fps)
                    : win.phase === "ended" ? qsTr("Ended")
                    : qsTr("Not running")
            }
        }
        Text {
            Layout.fillWidth: true
            visible: win.phase === "ended" && (win.session.detail || "").length > 0
            text: win.session.detail || ""
            textFormat: Text.PlainText
            wrapMode: Text.WordWrap
            color: Theme.textSecondary
        }
        Item { Layout.fillHeight: true }
        RowLayout {
            Layout.alignment: Qt.AlignRight
            UButton { text: qsTr("Close"); onClicked: win.close() }
            UButton {
                primary: true
                text: win.active ? qsTr("Stop") : qsTr("Start")
                enabled: win.active || (win.peerConnected && win.driverReady)
                onClicked: {
                    if (win.active) { win.controller.stop(win.devId); return }
                    const mode = win.chosenMode()
                    win.controller.configure({width: mode.width || 0, height: mode.height || 0, fps: mode.fps})
                    win.controller.offer(win.devId, mode)
                }
            }
        }
    }
}
