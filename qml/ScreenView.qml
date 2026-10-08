import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtMultimedia
import UConnect

ColumnLayout {
    id: view
    required property string devId
    property bool wantControl: false
    property var session: App.screen.state(devId)
    property bool fit: true
    spacing: 0

    Component.onCompleted: {
        App.screen.attachSink(devId, video.videoSink)
        App.screen.request(devId, wantControl)
    }
    Component.onDestruction: App.screen.attachSink(devId, null)
    Connections {
        target: App.screen
        function onStateChanged(peer) { if (peer === view.devId) view.session = App.screen.state(peer) }
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
            enabled: view.session.state === "live" && count > 1
            onActivated: index => App.screen.monitor(view.devId, index)
        }
        Item { Layout.fillWidth: true }
        UButton {
            text: view.session.state === "live" || view.session.state === "waiting" || view.session.state === "requesting"
                  ? qsTr("Stop") : qsTr("Connect")
            onClicked: {
                if (view.session.state === "live" || view.session.state === "waiting" || view.session.state === "requesting") App.screen.stop(view.devId)
                else App.screen.request(view.devId, view.wantControl)
            }
        }
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
