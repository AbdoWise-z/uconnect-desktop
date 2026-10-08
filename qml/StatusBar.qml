import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import UConnect

Rectangle {
    id: bar

    property bool inWorkspace: false
    // Latest App.notify() message, shown in place of the status for a while.
    property string flash: ""

    implicitHeight: 26
    color: Theme.panel

    function statusText() {
        switch (App.status) {
        case "offline": return qsTr("Not connected")
        case "online": return qsTr("Connected to topic %1").arg(App.topicId.substring(0, 8))
        }
        return App.statusDetail
    }

    Connections {
        target: App
        function onNotify(message) {
            bar.flash = message
            flashTimer.restart()
        }
    }
    Timer {
        id: flashTimer
        interval: 6000
        onTriggered: bar.flash = ""
    }

    Rectangle {
        width: parent.width
        height: 1
        color: Theme.divider
    }

    component Segment: Rectangle {
        id: segment

        property alias text: label.text
        property bool showDot: false
        property color dotColor: Theme.textSecondary
        property bool pulsing: false
        property string tip: ""
        signal clicked()

        Layout.fillHeight: true
        Layout.topMargin: 3
        Layout.bottomMargin: 2
        implicitWidth: row.implicitWidth + 16
        radius: Theme.radius
        color: area.containsMouse && area.enabled ? Theme.hover : "transparent"

        RowLayout {
            id: row
            anchors.centerIn: parent
            spacing: 6
            StatusDot {
                visible: segment.showDot
                color: segment.dotColor
                pulsing: segment.pulsing
            }
            Text {
                id: label
                color: Theme.textSecondary
                font.pixelSize: Theme.smallFontSize
            }
        }
        MouseArea {
            id: area
            anchors.fill: parent
            hoverEnabled: true
            onClicked: segment.clicked()
        }
        ToolTip.visible: tip.length > 0 && area.containsMouse
        ToolTip.delay: 500
        ToolTip.text: tip
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 10
        anchors.rightMargin: 4
        spacing: 2

        Text {
            Layout.fillWidth: true
            elide: Text.ElideRight
            textFormat: Text.PlainText
            font.pixelSize: Theme.smallFontSize
            text: bar.flash.length > 0 ? bar.flash : bar.statusText()
            color: bar.flash.length === 0 && App.status === "error" ? Theme.error : Theme.textSecondary
        }

        Segment {
            visible: bar.inWorkspace
            text: qsTr("%1 of %2 devices connected").arg(App.peers.connectedCount).arg(App.peers.count)
        }
        Segment {
            visible: App.screen.hosting.length > 0
            showDot: true
            dotColor: Theme.warning
            text: qsTr("Sharing screen · Stop")
            onClicked: App.screen.stopHosting()
        }
        Segment {
            visible: bar.inWorkspace && App.topicId.length > 0
            text: qsTr("Topic %1").arg(App.topicId.substring(0, 8))
            tip: qsTr("Click to copy the topic ID")
            onClicked: App.copyTopicId()
        }
        Segment {
            text: App.serverAddress.length > 0 ? App.serverAddress : qsTr("No server")
            showDot: true
            dotColor: !App.active ? Theme.textSecondary
                      : App.serverConnected ? Theme.success : Theme.warning
            pulsing: App.status === "connecting" || App.status === "reconnecting"
            tip: !App.active ? qsTr("Rendezvous server (not connected)")
                 : App.serverConnected ? qsTr("Rendezvous server connected")
                 : qsTr("Waiting for the rendezvous server")
        }
    }
}
