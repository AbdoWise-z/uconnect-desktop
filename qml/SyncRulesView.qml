import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import UConnect

// Edits a synced folder's rules: .syncignore (synced to every member) and
// .syncskip (this device only), with a live preview of what each line
// decides, the lines that are no use, and what saving would change.
Item {
    id: view

    required property string folderId
    // false: editing .syncignore; true: .syncskip.
    property bool skipFile: false
    property string saved: ""     // the text on disk, as loaded
    property bool loaded: false
    property var preview: ({})    // from App.previewSyncRules
    property string probe: ""
    readonly property bool modified: loaded && editor.text !== saved
    readonly property var problems: preview.problems || []
    readonly property var matches: preview.matches || []
    readonly property int stop: preview.stop || 0
    readonly property int resume: preview.resume || 0

    function reload() {
        if (folderId.length === 0) return
        loaded = false
        App.loadSyncRules(folderId, skipFile)
    }
    function requestPreview() {
        if (folderId.length === 0) return
        App.previewSyncRules(folderId, skipFile, editor.text, probe)
    }
    function save() {
        App.saveSyncRules(folderId, skipFile, editor.text)
        saved = editor.text
    }
    function lineText(m) {
        const where = m.via.length > 0 ? " · " + qsTr("through the folder %1").arg(m.via) : ""
        return (m.negate ? qsTr("brought back by line %1 (%2)") : qsTr("left out by line %1 (%2)")).arg(m.line).arg(m.pattern) + where
    }

    Component.onCompleted: reload()
    onFolderIdChanged: reload()
    onSkipFileChanged: reload()
    onVisibleChanged: if (visible && !loaded) reload()

    Connections {
        target: App
        function onSyncRulesLoaded(id, skip, text) {
            if (id !== view.folderId || skip !== view.skipFile) return
            view.saved = text
            editor.text = text
            view.loaded = true
            view.requestPreview()
        }
        function onSyncRulesPreviewed(id, skip, data) {
            if (id === view.folderId && skip === view.skipFile) view.preview = data
        }
    }
    Timer {
        id: debounce
        interval: 350
        onTriggered: view.requestPreview()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        // Which file.
        RowLayout {
            spacing: 0
            Repeater {
                model: [
                    { skip: false, label: ".syncignore", hint: qsTr("Every device") },
                    { skip: true, label: ".syncskip", hint: qsTr("This device only") }
                ]
                delegate: Rectangle {
                    required property var modelData
                    required property int index
                    implicitWidth: tabLabel.implicitWidth + 24
                    implicitHeight: 30
                    color: view.skipFile === modelData.skip ? Theme.selection : tabArea.containsMouse ? Theme.hover : "transparent"
                    radius: Theme.radius
                    ColumnLayout {
                        id: tabLabel
                        anchors.centerIn: parent
                        spacing: 0
                        Text {
                            Layout.alignment: Qt.AlignHCenter
                            text: modelData.label
                            color: Theme.text
                            font.pixelSize: Theme.fontSize
                        }
                        Text {
                            Layout.alignment: Qt.AlignHCenter
                            text: modelData.hint
                            color: Theme.textSecondary
                            font.pixelSize: 10
                        }
                    }
                    MouseArea {
                        id: tabArea
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: {
                            if (view.skipFile === modelData.skip) return
                            if (view.modified) {
                                switchAsk.target = modelData.skip
                                switchAsk.open()
                            } else {
                                view.skipFile = modelData.skip
                            }
                        }
                    }
                }
            }
            Item { Layout.fillWidth: true }
            UButton {
                text: qsTr("Revert")
                enabled: view.modified
                onClicked: editor.text = view.saved
            }
            UButton {
                text: qsTr("Save")
                primary: true
                enabled: view.modified && view.loaded
                onClicked: {
                    if (!view.skipFile && view.stop > 0) saveAsk.open()
                    else view.save()
                }
            }
        }

        Text {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            font.pixelSize: Theme.smallFontSize
            color: Theme.textSecondary
            text: view.skipFile
                  ? qsTr("Paths that match keep their local contents on this device: changes to them are neither sent nor taken. The file never syncs.")
                  : qsTr("Paths that match are not synced. The file itself syncs to every device, so a mistake here reaches all of them; copies already on disk are never deleted.")
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 10

            // The editor.
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredWidth: 1
                spacing: 6
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    radius: Theme.radius
                    color: Theme.input
                    border.color: editor.activeFocus ? Theme.focus : Theme.inputBorder
                    ScrollView {
                        anchors.fill: parent
                        anchors.margins: 1
                        clip: true
                        TextArea {
                            id: editor
                            objectName: "syncRulesEditor"
                            font.family: "monospace"
                            font.pixelSize: Theme.fontSize
                            color: Theme.text
                            selectionColor: Theme.selection
                            background: null
                            wrapMode: TextEdit.NoWrap
                            placeholderText: view.skipFile ? "drafts/\n*.tmp\n/private-notes.md" : "*.tmp\n/build\nnode_modules/\n!keep.tmp"
                            placeholderTextColor: Theme.textDisabled
                            onTextChanged: if (view.loaded) debounce.restart()
                        }
                    }
                }
                Repeater {
                    model: view.problems
                    delegate: Text {
                        required property var modelData
                        Layout.fillWidth: true
                        textFormat: Text.PlainText
                        wrapMode: Text.WordWrap
                        font.pixelSize: Theme.smallFontSize
                        color: Theme.error
                        text: qsTr("Line %1 is not a pattern and is skipped: %2").arg(modelData.line).arg(modelData.pattern)
                    }
                }
                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: Theme.smallFontSize
                    color: Theme.textSecondary
                    text: qsTr("One pattern per line, as in .gitignore: * and ? within a name, ** across folders, a leading or inner / anchors it at the top, a trailing / matches folders only, ! brings a match back (except inside a left-out folder), # starts a comment.")
                }
            }

            // What it would do.
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredWidth: 1
                spacing: 6
                Banner {
                    Layout.fillWidth: true
                    visible: view.modified && (view.stop > 0 || view.resume > 0)
                    kind: view.stop > 0 && !view.skipFile ? "error" : "info"
                    text: {
                        const parts = []
                        if (view.stop > 0)
                            parts.push(view.skipFile
                                       ? qsTr("%1 synced file(s) will stop syncing on this device; their copies here stay as they are.").arg(view.stop)
                                       : qsTr("%1 synced file(s) will stop syncing and count as deleted on the other devices (their copies stay on disk).").arg(view.stop))
                        if (view.resume > 0) parts.push(qsTr("%1 file(s) left out now would sync again.").arg(view.resume))
                        return parts.join(" ")
                    }
                }
                UTextField {
                    Layout.fillWidth: true
                    placeholderText: qsTr("Which rule decides this path? Type one, e.g. docs/draft.tmp")
                    onTextChanged: {
                        view.probe = text.trim()
                        debounce.restart()
                    }
                }
                Text {
                    Layout.fillWidth: true
                    visible: view.probe.length > 0 && view.preview.probe !== undefined
                    wrapMode: Text.WordWrap
                    textFormat: Text.PlainText
                    color: view.preview.probe && view.preview.probe.excluded ? Theme.warning : Theme.success
                    text: !view.preview.probe ? ""
                          : view.preview.probe.line > 0 ? view.lineText(view.preview.probe)
                          : qsTr("No line matches: it syncs (unless the other rules file leaves it out).")
                }
                Text {
                    text: qsTr("Paths these rules decide")
                    color: Theme.text
                    font.bold: true
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    radius: Theme.radius
                    color: Theme.panel
                    border.color: Theme.border
                    ListView {
                        id: matchList
                        objectName: "syncRuleMatches"
                        anchors.fill: parent
                        anchors.margins: 1
                        clip: true
                        model: view.matches
                        boundsBehavior: Flickable.StopAtBounds
                        ScrollBar.vertical: ScrollBar {}
                        delegate: ColumnLayout {
                            required property var modelData
                            width: ListView.view.width
                            spacing: 0
                            Text {
                                Layout.fillWidth: true
                                Layout.leftMargin: 8
                                Layout.rightMargin: 8
                                Layout.topMargin: 4
                                text: modelData.path + (modelData.dir ? "/" : "")
                                textFormat: Text.PlainText
                                elide: Text.ElideMiddle
                                color: modelData.negate ? Theme.text : Theme.textSecondary
                            }
                            Text {
                                Layout.fillWidth: true
                                Layout.leftMargin: 8
                                Layout.rightMargin: 8
                                Layout.bottomMargin: 4
                                text: view.lineText(modelData)
                                textFormat: Text.PlainText
                                elide: Text.ElideRight
                                font.pixelSize: Theme.smallFontSize
                                color: modelData.negate ? Theme.success : Theme.warning
                            }
                        }
                        Text {
                            anchors.centerIn: parent
                            width: parent.width - 24
                            visible: matchList.count === 0
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.WordWrap
                            color: Theme.textSecondary
                            text: view.loaded ? qsTr("No rule matches anything in the folder.") : qsTr("Loading...")
                        }
                    }
                }
            }
        }
    }

    // Saving .syncignore that drops files: a last look.
    Popup {
        id: saveAsk
        anchors.centerIn: Overlay.overlay
        width: Math.min(460, (parent ? parent.width : 460) - 48)
        modal: true
        padding: 20
        background: Rectangle {
            color: Theme.panel
            border.color: Theme.popupBorder
            radius: 8
        }
        ColumnLayout {
            width: parent.width
            spacing: 12
            Text {
                Layout.fillWidth: true
                text: qsTr("Save .syncignore?")
                color: Theme.text
                font.bold: true
            }
            Text {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.textSecondary
                text: qsTr("%1 synced file(s) will stop syncing and count as deleted on the other devices. Their copies stay on disk everywhere, but the rule reaches every device.").arg(view.stop)
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                spacing: 8
                UButton {
                    text: qsTr("Cancel")
                    onClicked: saveAsk.close()
                }
                UButton {
                    text: qsTr("Save")
                    primary: true
                    onClicked: {
                        view.save()
                        saveAsk.close()
                    }
                }
            }
        }
    }

    // Switching files with unsaved edits.
    Popup {
        id: switchAsk
        property bool target: false
        anchors.centerIn: Overlay.overlay
        width: Math.min(400, (parent ? parent.width : 400) - 48)
        modal: true
        padding: 20
        background: Rectangle {
            color: Theme.panel
            border.color: Theme.popupBorder
            radius: 8
        }
        ColumnLayout {
            width: parent.width
            spacing: 12
            Text {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.text
                text: qsTr("Discard the unsaved changes to %1?").arg(view.skipFile ? ".syncskip" : ".syncignore")
            }
            RowLayout {
                Layout.alignment: Qt.AlignRight
                spacing: 8
                UButton {
                    text: qsTr("Keep Editing")
                    onClicked: switchAsk.close()
                }
                UButton {
                    text: qsTr("Discard")
                    primary: true
                    onClicked: {
                        switchAsk.close()
                        view.skipFile = switchAsk.target
                    }
                }
            }
        }
    }
}
