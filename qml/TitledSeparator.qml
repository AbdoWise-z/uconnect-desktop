import QtQuick
import QtQuick.Layouts

// A group title followed by a rule, as used in JetBrains settings pages.
RowLayout {
    property alias text: title.text

    Layout.fillWidth: true
    Layout.topMargin: 10
    spacing: 8

    Text {
        id: title
        font.pixelSize: Theme.fontSize
        color: Theme.textSecondary
    }
    Rectangle {
        Layout.fillWidth: true
        implicitHeight: 1
        color: Theme.border
    }
}
