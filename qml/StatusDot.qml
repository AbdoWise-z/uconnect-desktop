import QtQuick

Rectangle {
    id: dot

    property bool pulsing: false

    implicitWidth: 8
    implicitHeight: 8
    radius: width / 2

    SequentialAnimation on opacity {
        running: dot.pulsing
        loops: Animation.Infinite
        onRunningChanged: if (!running) dot.opacity = 1
        NumberAnimation { to: 0.3; duration: 600 }
        NumberAnimation { to: 1.0; duration: 600 }
    }
}
