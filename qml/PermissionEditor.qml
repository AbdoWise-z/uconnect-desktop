import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import UConnect

// Checkboxes for every permission in App.permissionDefinitions, grouped by
// service. Built from that table, so new permissions appear without changes here.
ColumnLayout {
    id: editor

    property var granted: []      // permission ids
    property bool editable: true
    // Sensitive permissions (camera, screen...) can be set: not for the
    // defaults, which any device with the invite link gets.
    property bool allowSensitive: true

    signal toggled(string permission, bool granted)

    spacing: 4

    Repeater {
        model: Theme.services
        delegate: ColumnLayout {
            id: group
            required property string modelData
            readonly property var perms: App.permissionDefinitions.filter(p => p.service === modelData)

            Layout.fillWidth: true
            Layout.bottomMargin: 6
            spacing: 2
            visible: perms.length > 0

            Text {
                text: Theme.serviceLabel(group.modelData)
                color: Theme.textSecondary
                font.pixelSize: Theme.smallFontSize
            }
            Repeater {
                model: group.perms
                delegate: RowLayout {
                    id: entry
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: 10

                    UCheckBox {
                        Layout.preferredWidth: 130
                        text: entry.modelData.label
                        enabled: editor.editable && (editor.allowSensitive || !entry.modelData.trustedOnly)
                        checked: editor.granted.indexOf(entry.modelData.id) >= 0
                        onToggled: editor.toggled(entry.modelData.id, checked)
                    }
                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: Theme.textSecondary
                        font.pixelSize: Theme.smallFontSize
                        text: entry.modelData.available
                              ? entry.modelData.description
                              : entry.modelData.description + "  " + qsTr("(not available yet)")
                    }
                }
            }
        }
    }
}
