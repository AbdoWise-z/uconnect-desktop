import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import UConnect

// The display device's side: another device's monitor, full screen on one of
// this device's screens. The local pointer is hidden; moving it or pressing
// Esc shows a bar with Stop, and holding Esc for a second stops.
Window {
    id: view

    required property string devId
    required property int screenIndex
    readonly property var controller: App.display
    property var session: controller.showState(devId)
    readonly property string phase: session.state || ""
    readonly property bool pictured: session.pictured === true
    readonly property var peer: { App.peers.revision; return App.peers.get(devId) }
    readonly property string hostName: peer.displayName !== undefined ? peer.displayName : qsTr("Another device")
    property bool barShown: true
    property bool showStats: false

    title: qsTr("Extended display from %1").arg(hostName)
    color: "#000000"
    flags: Qt.Window | Qt.FramelessWindowHint
    screen: Qt.application.screens[Math.max(0, Math.min(screenIndex, Qt.application.screens.length - 1))]

    Component.onCompleted: {
        controller.attachSink(devId, video.videoSink)
        // Placed on its screen first, then full screen there.
        x = screen.virtualX; y = screen.virtualY; width = screen.width; height = screen.height
        showFullScreen()
        raise(); requestActivate()
        hideBar.restart()
    }
    Component.onDestruction: controller.attachSink(devId, null)
    onClosing: if (phase !== "ended") controller.stop(devId)

    Connections {
        target: view.controller
        function onShowStateChanged(peer) {
            if (peer !== view.devId) return
            view.session = view.controller.showState(peer)
            if (view.phase === "ended" || view.phase === "denied") { view.barShown = true; closeLater.restart() }
        }
    }
    Timer { id: hideBar; interval: 2500; onTriggered: view.barShown = !view.pictured }
    Timer { id: closeLater; interval: 4000; onTriggered: view.close() }
    Timer { id: escHold; interval: 1000; onTriggered: view.controller.stop(view.devId) }

    Item {
        anchors.fill: parent
        focus: true
        Keys.onPressed: event => {
            if (event.key === Qt.Key_Escape) {
                view.barShown = true; hideBar.restart()
                if (!event.isAutoRepeat) escHold.restart()
                event.accepted = true
            }
        }
        Keys.onReleased: event => { if (event.key === Qt.Key_Escape && !event.isAutoRepeat) escHold.stop() }

        VideoOutput {
            id: video
            anchors.fill: parent
            fillMode: VideoOutput.PreserveAspectFit
        }
        // The host's pointer, drawn here: it is not in the picture.
        Image {
            readonly property var monitor: (view.session.monitors || [])[0] || ({})
            readonly property real scaleX: video.contentRect.width / (monitor.width || 1)
            readonly property real scaleY: video.contentRect.height / (monitor.height || 1)
            x: video.contentRect.x + ((view.session.cursorX || 0) - (view.session.cursorHotspotX || 0)) * scaleX
            y: video.contentRect.y + ((view.session.cursorY || 0) - (view.session.cursorHotspotY || 0)) * scaleY
            width: implicitWidth * scaleX
            height: implicitHeight * scaleY
            source: view.session.cursorImage || ""
            visible: view.phase === "live" && view.session.cursorVisible === true && view.session.cursorEmbedded === false
        }
        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.NoButton
            cursorShape: view.barShown ? Qt.ArrowCursor : Qt.BlankCursor
            onPositionChanged: { view.barShown = true; hideBar.restart() }
        }

        ColumnLayout {
            anchors.centerIn: parent
            visible: !view.pictured || view.phase !== "live"
            spacing: 8
            Text {
                Layout.alignment: Qt.AlignHCenter
                color: "#FFFFFF"
                font.pixelSize: Theme.fontSize + 4
                text: view.phase === "live" ? qsTr("Waiting for the first picture…")
                    : view.phase === "ended" || view.phase === "denied" ? qsTr("The extended display ended")
                    : qsTr("Starting the extended display from %1…").arg(view.hostName)
            }
            Text {
                Layout.alignment: Qt.AlignHCenter
                Layout.maximumWidth: 520
                horizontalAlignment: Text.AlignHCenter
                visible: text.length > 0
                text: view.phase === "live" ? (view.session.hostStatus || "") : (view.session.detail || "")
                textFormat: Text.PlainText
                wrapMode: Text.WordWrap
                color: "#BBBBBB"
            }
        }

        Rectangle {
            anchors.top: parent.top
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.topMargin: 12
            visible: view.barShown
            width: bar.implicitWidth + 24
            height: bar.implicitHeight + 12
            radius: Theme.radius
            color: "#E02B2D30"
            RowLayout {
                id: bar
                anchors.centerIn: parent
                spacing: 10
                StatusDot { color: view.phase === "live" ? Theme.success : Theme.warning }
                Text {
                    color: Theme.text
                    textFormat: Text.PlainText
                    text: qsTr("Extended display from %1").arg(view.hostName)
                }
                Text {
                    visible: view.showStats && view.phase === "live"
                    color: Theme.textSecondary
                    font.pixelSize: Theme.smallFontSize
                    text: qsTr("%1 fps · %2 Mbit/s · decode %3 ms · loss %4%")
                        .arg(Number(view.session.fps || 0).toFixed(0)).arg(Number((view.session.bitrate || 0) / 1000000).toFixed(1))
                        .arg(Number(view.session.decodeMs || 0).toFixed(0)).arg(Number((view.session.loss || 0) * 100).toFixed(1))
                }
                UToolButton { text: qsTr("Stats"); checkable: true; checked: view.showStats; onClicked: view.showStats = !view.showStats }
                UButton {
                    text: view.phase === "ended" || view.phase === "denied" ? qsTr("Close") : qsTr("Stop")
                    onClicked: view.phase === "ended" || view.phase === "denied" ? view.close() : view.controller.stop(view.devId)
                }
            }
        }
        Text {
            anchors.bottom: parent.bottom
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottomMargin: 12
            visible: view.barShown && view.phase === "live"
            text: qsTr("Hold Esc to stop")
            color: "#99FFFFFF"
            font.pixelSize: Theme.smallFontSize
        }
    }
}
