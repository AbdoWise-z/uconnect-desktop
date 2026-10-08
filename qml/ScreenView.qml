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
            text: qsTr("Full Screen")
            onClicked: view.Window.window.visibility = view.Window.window.visibility === Window.FullScreen ? Window.Windowed : Window.FullScreen
        }
        ComboBox {
            model: view.session.monitors || []
            textRole: "name"
            currentIndex: view.session.monitor || 0
            enabled: view.session.state === "live" && count > 1
            onActivated: index => view.controller.monitor(view.devId, index)
        }
        Item { Layout.fillWidth: true }
        UButton {
            text: view.session.state === "live" || view.session.state === "waiting" || view.session.state === "requesting"
                  ? qsTr("Stop") : qsTr("Connect")
            onClicked: {
                if (view.session.state === "live" || view.session.state === "waiting" || view.session.state === "requesting") view.controller.stop(view.devId)
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
            visible: view.session.state !== "live"
            spacing: 8
            Text {
                Layout.alignment: Qt.AlignHCenter
                color: Theme.text
                text: view.session.state === "requesting" ? qsTr("Requesting screen sharing…")
                    : view.session.state === "waiting" ? qsTr("Waiting for consent…")
                    : view.session.state === "busy" ? qsTr("This screen is already shared")
                    : view.session.state === "denied" ? qsTr("Screen request denied") : qsTr("Screen sharing ended")
            }
            Text {
                Layout.maximumWidth: 480
                Layout.alignment: Qt.AlignHCenter
                text: view.session.detail || ""
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                color: Theme.textSecondary
            }
        }
        Rectangle {
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            anchors.margins: 12
            visible: view.session.state === "live"
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
