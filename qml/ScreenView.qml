import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import UConnect

ColumnLayout {
    id: view
    required property string devId
    property var controller: App.screen
    property bool camera: false
    property bool wantControl: false
    property var session: view.controller.state(devId)
    property bool fit: true
    property bool showStats: false
    readonly property string phase: view.session.state || ""
    readonly property bool active: view.phase === "live" || view.phase === "waiting" || view.phase === "requesting"
    // Live, but no frame decoded yet.
    readonly property bool starting: view.phase === "live" && view.session.pictured !== true
    readonly property bool fullScreen: view.Window.window !== null && view.Window.window.visibility === Window.FullScreen
    spacing: 0

    Component.onCompleted: {
        view.controller.attachSink(devId, video.videoSink)
        view.controller.request(devId, wantControl)
    }
    Component.onDestruction: view.controller.attachSink(devId, null)
    Connections {
        target: view.controller
        function onStateChanged(peer) { if (peer === view.devId) view.session = view.controller.state(peer) }
    }
    RowLayout {
        Layout.fillWidth: true
        Layout.margins: 8
        spacing: 8
        UToolButton { text: qsTr("Fit"); checkable: true; checked: view.fit; onClicked: view.fit = true }
        UToolButton { text: "100%"; checkable: true; checked: !view.fit; onClicked: view.fit = false }
        UToolButton {
            text: view.fullScreen ? qsTr("Exit Full Screen") : qsTr("Full Screen")
            onClicked: view.Window.window.visibility = view.fullScreen ? Window.Windowed : Window.FullScreen
        }
        UToolButton { text: qsTr("Stats"); checkable: true; checked: view.showStats; onClicked: view.showStats = !view.showStats }
        UComboBox {
            Layout.preferredWidth: 180
            Layout.maximumWidth: 220
            model: view.session.monitors || []
            textRole: "name"
            currentIndex: view.session.monitor || 0
            enabled: view.session.state === "live" && count > 1
            onActivated: index => view.controller.monitor(view.devId, index)
        }
        Item { Layout.fillWidth: true }
        UButton {
            text: view.active ? qsTr("Stop") : qsTr("Connect")
            onClicked: {
                if (view.active) view.controller.stop(view.devId)
                else view.controller.request(view.devId, view.wantControl)
            }
        }
    }
    RowLayout {
        visible: !view.camera && view.session.state === "live"
        Layout.fillWidth: true
        Layout.leftMargin: 8
        Layout.rightMargin: 8
        Layout.bottomMargin: 8
        spacing: 8
        UToolButton {
            text: view.session.controlPending ? qsTr("Waiting for control…") : view.session.control ? qsTr("Stop control") : qsTr("Request control")
            visible: !view.camera
            enabled: view.session.state === "live" && !view.session.controlPending
            onClicked: view.controller.requestControl(view.devId, !view.session.control)
        }
        UToolButton {
            visible: view.session.control === true
            text: view.session.keyboardCaptured ? qsTr("Keyboard captured: %1 to release").arg(({228: "Right Ctrl", 41: "Escape", 69: "F12"})[view.controller.settings.releaseUsage || 228]) : qsTr("Capture keyboard")
            onClicked: view.controller.captureKeyboard(view.devId, view.Window.window, !view.session.keyboardCaptured)
        }
        Item { Layout.fillWidth: true }
    }
    Rectangle {
        Layout.fillWidth: true
        Layout.fillHeight: true
        color: "#000000"
        clip: true
        VideoOutput {
            id: video
            anchors.centerIn: parent
            width: view.fit ? parent.width : sourceRect.width
            height: view.fit ? parent.height : sourceRect.height
            fillMode: VideoOutput.PreserveAspectFit
        }
        MouseArea {
            anchors.fill: parent
            enabled: view.session.control === true && view.session.state === "live"
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
            function containsVideo(mouse) {
                const rect = video.contentRect
                return mouse.x >= video.x + rect.x && mouse.x < video.x + rect.x + rect.width
                    && mouse.y >= video.y + rect.y && mouse.y < video.y + rect.y + rect.height
            }
            function send(mouse) {
                const rect = video.contentRect
                if (rect.width <= 0 || rect.height <= 0) return
                view.controller.mouse(view.devId, (mouse.x - video.x - rect.x) / rect.width,
                    (mouse.y - video.y - rect.y) / rect.height, mouse.buttons)
            }
            onPressed: mouse => {
                if (!containsVideo(mouse)) { mouse.accepted = false; return }
                view.controller.captureKeyboard(view.devId, view.Window.window, true); send(mouse)
            }
            onReleased: mouse => send(mouse)
            onPositionChanged: mouse => { if (mouse.buttons || containsVideo(mouse)) send(mouse) }
            onCanceled: view.controller.releaseInput(view.devId)
            onWheel: wheel => { view.controller.wheel(view.devId, wheel.angleDelta.x, wheel.angleDelta.y); wheel.accepted = true }
        }
        Image {
            readonly property var monitor: (view.session.monitors || [])[view.session.monitor || 0] || ({})
            readonly property real scaleX: video.contentRect.width / (monitor.width || 1)
            readonly property real scaleY: video.contentRect.height / (monitor.height || 1)
            x: video.x + video.contentRect.x + ((view.session.cursorX || 0) - (view.session.cursorHotspotX || 0)) * scaleX
            y: video.y + video.contentRect.y + ((view.session.cursorY || 0) - (view.session.cursorHotspotY || 0)) * scaleY
            width: implicitWidth * scaleX
            height: implicitHeight * scaleY
            source: view.session.cursorImage || ""
            visible: view.session.state === "live" && view.session.cursorVisible === true && view.session.cursorEmbedded === false
        }
        ColumnLayout {
            anchors.centerIn: parent
            visible: view.phase !== "live" || view.starting
            spacing: 8
            Text {
                Layout.alignment: Qt.AlignHCenter
                color: Theme.text
                font.pixelSize: Theme.fontSize + 2
                text: view.starting ? qsTr("Waiting for the first picture…")
                    : view.phase === "requesting" ? (view.camera ? qsTr("Requesting the camera…") : qsTr("Requesting screen sharing…"))
                    : view.phase === "waiting" ? qsTr("Waiting for the other device to allow it…")
                    : view.phase === "busy" ? (view.camera ? qsTr("This camera is already shared") : qsTr("This screen is already shared"))
                    : view.phase === "denied" ? qsTr("Request denied")
                    : view.camera ? qsTr("Camera sharing ended") : qsTr("Screen sharing ended")
            }
            Text {
                Layout.maximumWidth: 480
                Layout.alignment: Qt.AlignHCenter
                horizontalAlignment: Text.AlignHCenter
                text: view.starting
                      ? (view.session.hostStatus
                         || (view.session.stalled ? qsTr("Nothing has arrived yet. The other device may be unable to capture, or the connection may be blocking video.") : ""))
                      : (view.session.detail || "")
                visible: text.length > 0
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                color: Theme.textSecondary
            }
            UButton {
                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: 4
                visible: !view.active || (view.starting && view.session.stalled === true)
                text: qsTr("Try Again")
                onClicked: view.controller.request(view.devId, view.wantControl)
            }
        }
        Rectangle {
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            anchors.margins: 12
            visible: view.showStats && view.phase === "live" && !view.starting
            color: "#CC2B2D30"
            width: stats.implicitWidth + 20
            height: stats.implicitHeight + 12
            radius: Theme.radius
            Text {
                id: stats
                anchors.centerIn: parent
                color: Theme.text
                font.pixelSize: Theme.smallFontSize
                text: qsTr("%1 fps · %2 Mbit/s · decode %3 ms · loss %4% · %5")
                    .arg(Number(view.session.fps || 0).toFixed(0)).arg(Number((view.session.bitrate || 0) / 1000000).toFixed(1))
                    .arg(Number(view.session.decodeMs || 0).toFixed(0)).arg(Number((view.session.loss || 0) * 100).toFixed(1))
                    .arg(view.session.path || qsTr("opening"))
            }
        }
    }
}
