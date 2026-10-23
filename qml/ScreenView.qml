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
    property int rotationDeg: 0  // camera preview rotation, 0/90/180/270
    property bool showStats: false

    // What this viewer asks the host for: a resolution, a frame rate and a
    // bitrate (0 = as much as the host allows). They start from Settings >
    // Watching; the view menu changes them, re-requesting the stream. The host
    // caps each by its own settings.
    readonly property var resolutions: [
        { label: qsTr("Original (up to 4K)"), w: 3840 }, { label: qsTr("1440p"), w: 2560 },
        { label: qsTr("1080p"), w: 1920 }, { label: qsTr("720p"), w: 1280 }, { label: qsTr("480p"), w: 854 }
    ]
    readonly property var frameRates: [15, 24, 30, 60, 90, 120, 144]
    readonly property var bitrates: [0, 2000000, 4000000, 8000000, 16000000, 32000000, 50000000]
    property int askWidth: view.controller.settings.viewWidth || 1920
    property int askFps: view.controller.settings.viewFps || 60
    property int askBitrate: view.controller.settings.viewBitrate || 0
    function bitrateLabel(bps) { return bps > 0 ? qsTr("%1 Mbit/s").arg(bps / 1000000) : qsTr("Automatic") }

    function requestView() {
        view.controller.request(view.devId, view.wantControl, view.askWidth, view.askFps, view.askBitrate)
    }
    function ask(width, fps, bitrate) {
        if (width === view.askWidth && fps === view.askFps && bitrate === view.askBitrate) return
        view.askWidth = width; view.askFps = fps; view.askBitrate = bitrate
        if (view.active) view.requestView()
    }
    readonly property string phase: view.session.state || ""
    readonly property bool active: view.phase === "live" || view.phase === "waiting" || view.phase === "requesting"
    // Live, but no frame decoded yet.
    readonly property bool starting: view.phase === "live" && view.session.pictured !== true
    spacing: 0

    Component.onCompleted: {
        view.controller.attachSink(devId, video.videoSink)
        view.requestView()
    }
    Component.onDestruction: view.controller.attachSink(devId, null)
    Connections {
        target: view.controller
        function onStateChanged(peer) { if (peer === view.devId) view.session = view.controller.state(peer) }
    }
    // The toolbar and view-settings menu live in the window header
    // (ServiceWindow.qml); this component is just the video surface.
    Rectangle {
        Layout.fillWidth: true
        Layout.fillHeight: true
        color: "#000000"
        clip: true
        VideoOutput {
            id: video
            anchors.centerIn: parent
            // When rotated a quarter turn, swap the fit box so the turned frame
            // still fills the view.
            readonly property bool sideways: view.rotationDeg === 90 || view.rotationDeg === 270
            width: view.fit ? (sideways ? parent.height : parent.width) : sourceRect.width
            height: view.fit ? (sideways ? parent.width : parent.height) : sourceRect.height
            rotation: view.rotationDeg
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
                onClicked: view.requestView()
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
            // Everything about how the stream keeps up: what this viewer shows
            // and decodes, its playout buffer, and how the host is doing.
            Text {
                id: stats
                anchors.centerIn: parent
                color: Theme.text
                font.pixelSize: Theme.smallFontSize
                font.family: "Consolas"
                readonly property var host: view.session.host || ({})
                function n(value, digits) { return Number(value || 0).toFixed(digits) }
                text: [
                    qsTr("Asked %1 px, %2 fps, %3").arg(view.askWidth).arg(view.askFps).arg(view.bitrateLabel(view.askBitrate)),
                    qsTr("Shown %1 fps (decoded %2) · %3 Mbit/s · loss %4% · jitter %5 ms · %6")
                        .arg(n(view.session.shownFps, 0)).arg(n(view.session.fps, 0))
                        .arg(n((view.session.bitrate || 0) / 1000000, 1)).arg(n((view.session.loss || 0) * 100, 1))
                        .arg(n(view.session.jitterMs, 1)).arg(view.session.path || qsTr("opening")),
                    qsTr("Buffer %1 ms (next in %2 ms, %3 waiting) · late %4 · skipped %5 · decode %6 ms")
                        .arg(view.session.bufferMs || 0).arg(n(view.session.waitingMs, 0)).arg(view.session.waitingFrames || 0)
                        .arg(view.session.late || 0).arg(view.session.skipped || 0).arg(n(view.session.decodeMs, 1)),
                    host.fps === undefined ? qsTr("Host: no statistics (an older version)")
                        : qsTr("Host: %1 fps, encode %2 ms, convert %3 ms on %4 threads · queue %5 (peak %6) · dropped %7/s")
                            .arg(n(host.fps, 0)).arg(n(host.encodeMs, 1)).arg(n(host.convertMs, 1)).arg(host.threads || 0)
                            .arg(n(host.queue, 1)).arg(host.queuePeak || 0).arg(n(host.dropped, 1)),
                    host.fps === undefined ? ""
                        : qsTr("Host target: %1 fps, %2 Mbit/s, %3 px · sending %4 Mbit/s%6 · %5")
                            .arg(host.targetFps || 0).arg(n((host.targetKbps || 0) / 1000, 1)).arg(host.width || 0)
                            .arg(n((host.sentKbps || 0) / 1000, 1)).arg(host.pipeline || "")
                            .arg(host.transport ? qsTr(" over %1").arg(host.transport.toUpperCase()) : "")
                ].filter(line => line.length > 0).join("\n")
            }
        }
    }
}
