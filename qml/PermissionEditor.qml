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
    // Where each granted permission comes from (permission -> "device",
    // "rule" or "default"), to mark it; empty for none. One carried over from
    // a network rule or the defaults is not the editor's to take away.
    property var from: ({})
    property string ruleName: ""

    signal toggled(string permission, bool granted)

    function sourceIcon(layer) { return layer === "device" ? "verified" : layer === "rule" ? "lan" : "shield" }
    function sourceColor(layer) {
        return layer === "device" ? Theme.success : layer === "rule" ? Theme.accent : Theme.textSecondary
    }
    function sourceText(layer) {
        return layer === "device" ? qsTr("From its special rules")
             : layer === "rule" ? qsTr("Carried over from the network rule %1").arg(editor.ruleName)
             : qsTr("Carried over from your default permissions")
    }

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
                    readonly property string source: editor.from[modelData.id] !== undefined ? editor.from[modelData.id] : ""
                    readonly property bool carried: source === "rule" || source === "default"
                    Layout.fillWidth: true
                    spacing: 10

                    UCheckBox {
                        Layout.preferredWidth: 130
                        text: entry.modelData.label
                        enabled: editor.editable && !entry.carried && (editor.allowSensitive || !entry.modelData.sensitive)
                        checked: editor.granted.indexOf(entry.modelData.id) >= 0
                        onToggled: editor.toggled(entry.modelData.id, checked)
                    }
                    // Which layer grants it, when the editor shows that.
                    Item {
                        visible: Object.keys(editor.from).length > 0
                        implicitWidth: 16
                        implicitHeight: 16
                        UIcon {
                            anchors.fill: parent
                            visible: entry.source !== ""
                            name: editor.sourceIcon(entry.source)
                            size: 16
                            color: editor.sourceColor(entry.source)
                        }
                        HoverHandler { id: sourceHover; enabled: entry.source !== "" }
                        ToolTip.visible: sourceHover.hovered
                        ToolTip.delay: 300
                        ToolTip.text: editor.sourceText(entry.source)
                                      + (entry.carried && editor.editable
                                         ? "
" + qsTr("Make its special rules an exception to take it away.") : "")
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
