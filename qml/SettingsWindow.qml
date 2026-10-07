import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import UConnect

// Modal settings dialog, laid out like the JetBrains one: searchable category
// tree on the left, the page on the right, OK / Cancel / Apply below. Edits go
// to a draft and only reach App on Apply or OK.
Window {
    id: dlg

    property string currentPage: "connection"
    property string query: ""

    readonly property var pages: [
        { id: "connection", title: qsTr("Connection"), depth: 0, keywords: "rendezvous server host port address startup automatically" },
        { id: "device", title: qsTr("Device"), depth: 0, keywords: "name computer" },
        { id: "services", title: qsTr("Services"), depth: 0, keywords: "offer share" },
        { id: "storage", title: Theme.serviceLabel("storage"), depth: 1, parent: "services", keywords: "files shared folders download upload drag drop" },
        { id: "camera", title: Theme.serviceLabel("camera"), depth: 1, parent: "services", keywords: "video webcam" },
        { id: "screen", title: Theme.serviceLabel("screen"), depth: 1, parent: "services", keywords: "display" },
        { id: "control", title: Theme.serviceLabel("control"), depth: 1, parent: "services", keywords: "mouse keyboard" },
        { id: "permissions", title: qsTr("Permissions"), depth: 0, keywords: "trust trusted untrust identity fingerprint default access" },
        { id: "security", title: qsTr("Security"), depth: 0, keywords: "invite link key remember store" },
        { id: "advanced", title: qsTr("Advanced"), depth: 0, keywords: "debug verbose logging diagnostics" }
    ]

    function pageById(id) {
        return pages.find(p => p.id === id)
    }
    function matches(p) {
        return query.length === 0 || (p.title + " " + p.keywords).toLowerCase().includes(query.toLowerCase())
    }
    readonly property var visiblePages: pages.filter(p => matches(p) || pages.some(c => c.parent === p.id && matches(c)))

    QtObject {
        id: draft
        property string server
        property string deviceName
        property var services: []
        property bool autoConnect
        property bool rememberTopic
        property bool verboseLogging
        property bool debugOnStartup
        property var sharedFolders: []
        property var readOnlyFolders: []
        property string downloadFolder
        property int maxUploadMB: 0
        property var defaultPermissions: []
        property var untrust: []  // public keys
    }

    function sameServices(a, b) {
        return a.slice().sort().join() === b.slice().sort().join()
    }

    readonly property bool valid: App.isValidServer(draft.server) && draft.deviceName.trim().length > 0
    readonly property bool modified: draft.server.trim() !== App.serverAddress
                                     || draft.deviceName.trim() !== App.deviceName
                                     || !sameServices(draft.services, App.offeredServices)
                                     || draft.autoConnect !== App.autoConnect
                                     || draft.rememberTopic !== App.rememberTopic
                                     || draft.verboseLogging !== App.verboseLogging
                                     || draft.debugOnStartup !== App.debugOnStartup
                                     || draft.sharedFolders.join("
") !== App.sharedFolders.join("
")
                                     || !sameServices(draft.readOnlyFolders, App.readOnlyFolders)
                                     || draft.downloadFolder !== App.downloadFolder
                                     || draft.maxUploadMB !== App.maxUploadMB
                                     || !sameServices(draft.defaultPermissions, App.defaultPermissions)
                                     || draft.untrust.length > 0

    function load() {
        draft.server = App.serverAddress
        draft.deviceName = App.deviceName
        draft.services = App.offeredServices.slice()
        draft.autoConnect = App.autoConnect
        draft.rememberTopic = App.rememberTopic
        draft.verboseLogging = App.verboseLogging
        draft.debugOnStartup = App.debugOnStartup
        draft.sharedFolders = App.sharedFolders.slice()
        draft.readOnlyFolders = App.readOnlyFolders.slice()
        draft.downloadFolder = App.downloadFolder
        draft.maxUploadMB = App.maxUploadMB
        draft.defaultPermissions = App.defaultPermissions.slice()
        draft.untrust = []
    }

    function apply() {
        App.applySettings({
            server: draft.server,
            deviceName: draft.deviceName,
            services: draft.services,
            autoConnect: draft.autoConnect,
            rememberTopic: draft.rememberTopic,
            verboseLogging: draft.verboseLogging,
            debugOnStartup: draft.debugOnStartup,
            sharedFolders: draft.sharedFolders,
            readOnlyFolders: draft.readOnlyFolders.filter(f => draft.sharedFolders.indexOf(f) >= 0),
            downloadFolder: draft.downloadFolder,
            maxUploadMB: draft.maxUploadMB,
            defaultPermissions: draft.defaultPermissions,
            untrust: draft.untrust
        })
        load()  // the draft now matches what was applied
    }

    function setDefaultPermission(permission, on) {
        const next = draft.defaultPermissions.filter(p => p !== permission)
        if (on) next.push(permission)
        draft.defaultPermissions = next
    }
    function toggleUntrust(key) {
        draft.untrust = draft.untrust.indexOf(key) >= 0 ? draft.untrust.filter(k => k !== key)
                                                        : draft.untrust.concat([key])
    }

    function setReadOnly(folder, on) {
        const next = draft.readOnlyFolders.filter(f => f !== folder)
        if (on) next.push(folder)
        draft.readOnlyFolders = next
    }

    function offers(service) {
        return draft.services.indexOf(service) >= 0
    }
    function setOffered(service, on) {
        const next = draft.services.filter(s => s !== service)
        if (on) next.push(service)
        draft.services = next
    }

    function addSharedFolder(url) {
        const path = App.localPath(url)
        if (path.length > 0 && draft.sharedFolders.indexOf(path) < 0)
            draft.sharedFolders = draft.sharedFolders.concat([path])
    }

    function openAt(pageId) {
        load()
        query = ""
        searchField.text = ""
        currentPage = pageId && pageById(pageId) ? pageId : "connection"
        show()
        raise()
        requestActivate()
    }

    title: qsTr("Settings")
    width: 960
    height: 660
    minimumWidth: 760
    minimumHeight: 480
    modality: Qt.ApplicationModal
    flags: Qt.Dialog
    color: Theme.panel

    FolderDialog {
        id: shareFolderDialog
        title: qsTr("Choose a Folder to Share")
        onAccepted: dlg.addSharedFolder(selectedFolder)
    }

    FolderDialog {
        id: downloadFolderDialog
        title: qsTr("Choose the Download Folder")
        onAccepted: {
            const path = App.localPath(selectedFolder)
            if (path.length > 0) draft.downloadFolder = path
        }
    }

    Shortcut {
        sequence: "Escape"
        onActivated: dlg.close()
    }

    // --- page building blocks ---------------------------------------------
    component Help: Text {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.textSecondary
        font.pixelSize: Theme.smallFontSize
    }
    component FieldLabel: Text {
        Layout.preferredWidth: 140
        color: Theme.text
    }
    component Indented: ColumnLayout {
        Layout.fillWidth: true
        Layout.leftMargin: 24
        spacing: 4
    }

    // --- pages -------------------------------------------------------------
    Component {
        id: connectionPage
        ColumnLayout {
            spacing: 8
            TitledSeparator { text: qsTr("Rendezvous Server") }
            RowLayout {
                FieldLabel { text: qsTr("Server address:") }
                UTextField {
                    Layout.preferredWidth: 320
                    text: draft.server
                    placeholderText: "host:port"
                    invalid: !App.isValidServer(text)
                    onTextEdited: draft.server = text
                }
            }
            Indented {
                Layout.leftMargin: 148
                Help {
                    text: qsTr("Where your devices find each other, as host:port. It cannot read what they send each other.")
                }
                Help {
                    visible: App.active && draft.server.trim() !== App.serverAddress
                    color: Theme.warning
                    text: qsTr("Takes effect the next time you connect.")
                }
            }

            TitledSeparator { text: qsTr("Startup") }
            UCheckBox {
                text: qsTr("Connect automatically on startup")
                checked: draft.autoConnect
                onToggled: draft.autoConnect = checked
            }
            Indented {
                Help {
                    text: draft.rememberTopic
                          ? qsTr("Reconnects to the last topic, using its remembered invite link.")
                          : qsTr("Needs “Remember the invite link” under Security.")
                }
            }
        }
    }

    Component {
        id: devicePage
        ColumnLayout {
            spacing: 8
            TitledSeparator { text: qsTr("Identity") }
            RowLayout {
                FieldLabel { text: qsTr("Device name:") }
                UTextField {
                    Layout.preferredWidth: 320
                    text: draft.deviceName
                    maximumLength: 64
                    invalid: text.trim().length === 0
                    onTextEdited: draft.deviceName = text
                }
            }
            Indented {
                Layout.leftMargin: 148
                Help {
                    text: qsTr("How this device appears to your other devices. Connected devices see a change right away.")
                }
            }
            RowLayout {
                Layout.topMargin: 6
                FieldLabel { text: qsTr("Fingerprint:") }
                Text {
                    text: App.myFingerprint
                    color: Theme.text
                    font.family: "Consolas, monospace"
                    font.pixelSize: 15
                }
                ULink {
                    text: qsTr("Copy")
                    onClicked: App.copyText(App.myFingerprint)
                }
            }
            Indented {
                Layout.leftMargin: 148
                Help {
                    text: qsTr("This device's identity. Before another device trusts this one, its user should check that it shows exactly this fingerprint.")
                }
            }
        }
    }

    Component {
        id: permissionsPage
        ColumnLayout {
            spacing: 8

            TitledSeparator { text: qsTr("Default Permissions") }
            Help {
                text: qsTr("What every device you have not trusted may do here, including devices whose identity is unknown. A device you trust starts with these, and then has its own.")
            }
            PermissionEditor {
                Layout.fillWidth: true
                Layout.topMargin: 4
                granted: draft.defaultPermissions
                onToggled: (permission, on) => dlg.setDefaultPermission(permission, on)
            }
            Banner {
                Layout.fillWidth: true
                visible: draft.defaultPermissions.length > 0
                kind: "error"
                text: qsTr("Any device with this topic's invite link gets these permissions without being trusted.")
            }

            TitledSeparator { text: qsTr("Trusted Devices") }
            Help {
                visible: App.trustedDevices.length === 0
                text: qsTr("No trusted devices yet. Trust a device from its tab, or by right-clicking it in Devices.")
            }
            Repeater {
                model: App.trustedDevices
                delegate: RowLayout {
                    id: trustedRow
                    required property var modelData
                    readonly property bool removing: draft.untrust.indexOf(modelData.key) >= 0

                    Layout.fillWidth: true
                    spacing: 12
                    opacity: removing ? 0.5 : 1

                    Avatar {
                        name: trustedRow.modelData.name
                        seed: trustedRow.modelData.key
                        size: 28
                    }
                    ColumnLayout {
                        spacing: 1
                        Text {
                            text: trustedRow.modelData.name
                            textFormat: Text.PlainText
                            color: Theme.text
                            font.strikeout: trustedRow.removing
                        }
                        Text {
                            text: trustedRow.modelData.fingerprint
                            color: Theme.textSecondary
                            font.family: "Consolas, monospace"
                            font.pixelSize: Theme.smallFontSize
                        }
                    }
                    Item { Layout.fillWidth: true }
                    Text {
                        color: Theme.textSecondary
                        font.pixelSize: Theme.smallFontSize
                        text: {
                            const n = trustedRow.modelData.permissions.length
                            return n === 0 ? qsTr("No permissions") : n === 1 ? qsTr("1 permission") : qsTr("%1 permissions").arg(n)
                        }
                    }
                    UButton {
                        text: trustedRow.removing ? qsTr("Keep") : qsTr("Untrust")
                        onClicked: dlg.toggleUntrust(trustedRow.modelData.key)
                    }
                }
            }
            Help {
                visible: App.trustedDevices.length > 0
                text: qsTr("Change a trusted device's own permissions from its tab, or with Permissions in its right-click menu.")
            }
        }
    }

    Component {
        id: servicesPage
        ColumnLayout {
            spacing: 8
            TitledSeparator { text: qsTr("Offered to Other Devices") }
            Repeater {
                model: Theme.services
                delegate: ColumnLayout {
                    required property string modelData
                    spacing: 2
                    UCheckBox {
                        text: Theme.serviceLabel(modelData)
                        checked: dlg.offers(modelData)
                        onToggled: dlg.setOffered(modelData, checked)
                    }
                    Indented {
                        Help { text: Theme.serviceDescription(modelData) }
                    }
                }
            }
            Banner {
                Layout.fillWidth: true
                Layout.topMargin: 12
                text: qsTr("Camera, screen sharing and remote control are not implemented yet; offering one of them only advertises it.")
            }
        }
    }

    Component {
        id: storagePage
        ColumnLayout {
            id: storage
            property int selected: -1
            spacing: 8

            TitledSeparator { text: qsTr("Availability") }
            UCheckBox {
                text: qsTr("Offer Storage to other devices in the topic")
                checked: dlg.offers("storage")
                onToggled: dlg.setOffered("storage", checked)
            }
            Indented {
                Help {
                    text: qsTr("Devices can browse, download or upload files in these folders when you grant the corresponding permission. Uploads add new files without overwriting existing ones.")
                }
            }

            TitledSeparator { text: qsTr("Shared Folders") }
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: Math.min(220, Math.max(96, folderList.contentHeight + 2))
                radius: Theme.radius
                color: Theme.input
                border.color: Theme.inputBorder

                ListView {
                    id: folderList
                    anchors.fill: parent
                    anchors.margins: 1
                    clip: true
                    model: draft.sharedFolders
                    delegate: Rectangle {
                        id: folderRow
                        required property int index
                        required property string modelData
                        width: ListView.view.width
                        height: 26
                        color: index === storage.selected ? Theme.selection
                               : folderArea.containsMouse ? Theme.hover : "transparent"
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            x: 10
                            width: parent.width - readOnlyBox.width - 30
                            text: folderRow.modelData
                            textFormat: Text.PlainText
                            elide: Text.ElideMiddle
                            color: Theme.text
                        }
                        MouseArea {
                            id: folderArea
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: storage.selected = folderRow.index
                        }
                        UCheckBox {
                            id: readOnlyBox
                            anchors.right: parent.right
                            anchors.rightMargin: 10
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("Read-only")
                            checked: draft.readOnlyFolders.indexOf(folderRow.modelData) >= 0
                            onToggled: dlg.setReadOnly(folderRow.modelData, checked)
                        }
                    }
                    Text {
                        anchors.centerIn: parent
                        visible: folderList.count === 0
                        text: qsTr("No folders shared yet.")
                        color: Theme.textSecondary
                    }
                }
            }
            RowLayout {
                spacing: 8
                UButton {
                    text: qsTr("Add Folder...")
                    onClicked: shareFolderDialog.open()
                }
                UButton {
                    text: qsTr("Remove")
                    enabled: storage.selected >= 0 && storage.selected < draft.sharedFolders.length
                    onClicked: {
                        draft.sharedFolders = draft.sharedFolders.filter((f, i) => i !== storage.selected)
                        storage.selected = -1
                    }
                }
            }
            Banner {
                Layout.fillWidth: true
                Layout.topMargin: 4
                visible: dlg.offers("storage") && draft.sharedFolders.length > 0
                kind: "error"
                text: qsTr("Everything inside these folders, including hidden files and subfolders, can be read by every device that has this topic's invite link.")
                      + " " + qsTr("Read-only folders cannot be changed by other devices, whatever you allow them.")
            }
            Help {
                visible: dlg.offers("storage") && draft.sharedFolders.length === 0
                color: Theme.warning
                text: qsTr("Storage is offered, but no folder is shared yet.")
            }

            TitledSeparator { text: qsTr("Uploads from other devices") }
            RowLayout {
                spacing: 8
                UCheckBox {
                    id: limitUploads
                    text: qsTr("Refuse files larger than")
                    checked: draft.maxUploadMB > 0
                    onToggled: draft.maxUploadMB = checked ? Math.max(1, Number(uploadLimitField.text) || 1024) : 0
                }
                UTextField {
                    id: uploadLimitField
                    implicitWidth: 90
                    enabled: limitUploads.checked
                    text: draft.maxUploadMB > 0 ? String(draft.maxUploadMB) : "1024"
                    validator: IntValidator { bottom: 1; top: 100000000 }
                    onTextEdited: if (acceptableInput) draft.maxUploadMB = Number(text)
                }
                Text {
                    text: qsTr("MB")
                    color: Theme.text
                }
                Item { Layout.fillWidth: true }
            }
            Indented {
                Help { text: qsTr("Uploads are also refused when they would leave this device's disk with less than 256 MB free.") }
            }

            TitledSeparator { text: qsTr("Downloads") }
            RowLayout {
                spacing: 8
                FieldLabel { text: qsTr("Save downloads to:") }
                UTextField {
                    Layout.fillWidth: true
                    text: draft.downloadFolder
                    readOnly: true
                }
                UButton {
                    text: qsTr("Browse...")
                    onClicked: downloadFolderDialog.open()
                }
            }
            Indented {
                Layout.leftMargin: 148
                Help { text: qsTr("Files you download from other devices are saved here. Existing files are never overwritten.") }
            }
        }
    }

    Component {
        id: servicePage
        ColumnLayout {
            id: svcPage
            readonly property string service: dlg.currentPage
            spacing: 8
            TitledSeparator { text: qsTr("Availability") }
            UCheckBox {
                text: qsTr("Offer %1 to other devices in the topic").arg(Theme.serviceLabel(svcPage.service))
                checked: dlg.offers(svcPage.service)
                onToggled: dlg.setOffered(svcPage.service, checked)
            }
            Indented {
                Help { text: Theme.serviceDescription(svcPage.service) }
            }
            Banner {
                Layout.fillWidth: true
                Layout.topMargin: 8
                visible: svcPage.service === "control"
                kind: "error"
                text: qsTr("Remote control lets other devices in the topic use this computer's mouse and keyboard.")
            }
            TitledSeparator { text: qsTr("Options") }
            Help {
                text: qsTr("Options for this service will appear here once it is implemented.")
            }
        }
    }

    Component {
        id: securityPage
        ColumnLayout {
            spacing: 8
            TitledSeparator { text: qsTr("Invite Link") }
            UCheckBox {
                text: qsTr("Remember the invite link on this device")
                checked: draft.rememberTopic
                onToggled: draft.rememberTopic = checked
            }
            Indented {
                Help {
                    text: qsTr("The link contains the topic's secret key. When remembered, it is stored unencrypted in this user's application settings. Turning this off also deletes the stored link.")
                }
            }
        }
    }

    Component {
        id: advancedPage
        ColumnLayout {
            spacing: 8
            TitledSeparator { text: qsTr("Diagnostics") }
            UCheckBox {
                text: qsTr("Verbose uConnect library logging")
                checked: draft.verboseLogging
                onToggled: draft.verboseLogging = checked
            }
            Indented {
                Help { text: qsTr("Takes effect the next time you connect.") }
            }
            UCheckBox {
                visible: App.debugAvailable
                text: qsTr("Open the debug window on startup")
                checked: draft.debugOnStartup
                onToggled: draft.debugOnStartup = checked
            }
        }
    }

    function pageComponent(id) {
        switch (id) {
        case "connection": return connectionPage
        case "device": return devicePage
        case "services": return servicesPage
        case "storage": return storagePage
        case "camera":
        case "screen":
        case "control": return servicePage
        case "permissions": return permissionsPage
        case "security": return securityPage
        case "advanced": return advancedPage
        }
        return connectionPage
    }

    // --- layout ------------------------------------------------------------
    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // Category tree
            ColumnLayout {
                Layout.preferredWidth: 250
                Layout.fillWidth: false
                Layout.fillHeight: true
                Layout.margins: 8
                spacing: 6

                UTextField {
                    id: searchField
                    Layout.fillWidth: true
                    placeholderText: qsTr("Search settings")
                    onTextEdited: {
                        dlg.query = text
                        const visible = dlg.visiblePages
                        if (visible.length > 0 && !visible.some(p => p.id === dlg.currentPage))
                            dlg.currentPage = visible[0].id
                    }
                }

                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: dlg.visiblePages
                    delegate: Rectangle {
                        id: entry
                        required property var modelData
                        readonly property bool selected: modelData.id === dlg.currentPage

                        width: ListView.view.width
                        height: 26
                        radius: Theme.radius
                        color: selected ? Theme.selection : entryArea.containsMouse ? Theme.hover : "transparent"

                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            x: 10 + entry.modelData.depth * 18
                            text: entry.modelData.title
                            color: Theme.text
                            font.bold: dlg.query.length > 0 && dlg.matches(entry.modelData)
                        }
                        MouseArea {
                            id: entryArea
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: dlg.currentPage = entry.modelData.id
                        }
                    }

                    Text {
                        anchors.centerIn: parent
                        visible: parent.count === 0
                        text: qsTr("Nothing found")
                        color: Theme.textSecondary
                    }
                }
            }

            Rectangle {
                Layout.fillHeight: true
                implicitWidth: 1
                color: Theme.divider
            }

            // Page
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0

                Text {
                    Layout.margins: 16
                    Layout.leftMargin: 20
                    Layout.bottomMargin: 4
                    font.pixelSize: 15
                    color: Theme.text
                    text: {
                        const p = dlg.pageById(dlg.currentPage)
                        if (!p) return ""
                        return p.parent ? dlg.pageById(p.parent).title + "  ›  " + p.title : p.title
                    }
                }

                Flickable {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    contentWidth: width
                    contentHeight: pageLoader.implicitHeight + 32
                    boundsBehavior: Flickable.StopAtBounds
                    ScrollBar.vertical: ScrollBar {}

                    Loader {
                        id: pageLoader
                        x: 20
                        y: 4
                        width: parent.width - 40
                        sourceComponent: dlg.pageComponent(dlg.currentPage)
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 1
            color: Theme.divider
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 12
            spacing: 8

            Text {
                Layout.fillWidth: true
                elide: Text.ElideRight
                color: dlg.valid ? Theme.textSecondary : Theme.error
                font.pixelSize: Theme.smallFontSize
                text: dlg.valid ? "" : qsTr("Fix the highlighted fields to save.")
            }
            UButton {
                text: qsTr("OK")
                primary: true
                enabled: dlg.valid
                onClicked: {
                    if (dlg.modified) dlg.apply()
                    dlg.close()
                }
            }
            UButton {
                text: qsTr("Cancel")
                onClicked: dlg.close()
            }
            UButton {
                text: qsTr("Apply")
                enabled: dlg.modified && dlg.valid
                onClicked: dlg.apply()
            }
        }
    }
}
