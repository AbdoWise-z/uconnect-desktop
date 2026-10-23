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

    property string currentPage: "general"
    property string query: ""

    readonly property var pages: [
        { id: "general", title: qsTr("General"), depth: 0, icon: "settings", keywords: "appearance color colour scheme theme dark light system startup automatically tray close minimize background login sign in boot hidden" },
        { id: "connection", title: qsTr("Connection"), depth: 0, icon: "dns", keywords: "rendezvous server host port address relay direct nat" },
        { id: "device", title: qsTr("Device"), depth: 0, icon: "computer", keywords: "name computer" },
        { id: "services", title: qsTr("Services"), depth: 0, icon: "apps", keywords: "offer share" },
        { id: "storage", title: Theme.serviceLabel("storage"), depth: 1, parent: "services", icon: "folder", keywords: "files shared folders download upload drag drop" },
        { id: "camera", title: Theme.serviceLabel("camera"), depth: 1, parent: "services", icon: "videocam", keywords: "video webcam" },
        { id: "screen", title: Theme.serviceLabel("screen"), depth: 1, parent: "services", icon: "screen", keywords: "display" },
        { id: "control", title: Theme.serviceLabel("control"), depth: 1, parent: "services", icon: "mouse", keywords: "mouse keyboard" },
        { id: "display", title: Theme.serviceLabel("display"), depth: 1, parent: "services", icon: "cast", keywords: "monitor second screen extend driver tablet" },
        { id: "permissions", title: qsTr("Permissions"), depth: 0, icon: "shield", keywords: "special rules identity fingerprint default access device" },
        { id: "network", title: qsTr("Network Rules"), depth: 1, parent: "permissions", icon: "lan", keywords: "lan local network subnet ip address pattern rules copy" },
        { id: "security", title: qsTr("Security"), depth: 0, icon: "lock", keywords: "invite link key remember store qr code scan phone" },
        { id: "advanced", title: qsTr("Advanced"), depth: 0, icon: "code", keywords: "debug verbose logging diagnostics" }
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
        property string colorScheme: "dark"
        property int relayMode: 0
        property bool debugOnStartup
        property bool closeToTray
        property bool launchAtLogin
        property bool startHidden
        property var sharedFolders: []
        property var readOnlyFolders: []
        property string downloadFolder
        property int maxUploadMB: 0
        property var defaultPermissions: []
        property var removeSpecial: []  // public keys whose special rules to drop
        property var networkRules: []   // this topic's, [{pattern,label,perms}]
        property var screen: ({})
        property var camera: ({})
        property var display: ({})
    }

    function sameServices(a, b) {
        return a.slice().sort().join() === b.slice().sort().join()
    }

    readonly property bool valid: App.isValidServer(draft.server) && draft.deviceName.trim().length > 0
                                  && draft.networkRules.every(r => App.networkPatternProblem(r.pattern) === "")

    // --- network rules (draft) ---
    function withRules(change) {
        const next = JSON.parse(JSON.stringify(draft.networkRules))
        change(next)
        draft.networkRules = next
    }
    function setRule(i, key, value) { withRules(r => { r[i][key] = value }) }
    function setRulePermission(i, permission, on) {
        withRules(r => {
            const perms = (r[i].perms || []).filter(p => p !== permission)
            if (on) perms.push(permission)
            r[i].perms = perms.sort()
        })
    }
    function moveRule(i, delta) {
        withRules(r => { const [rule] = r.splice(i, 1); r.splice(i + delta, 0, rule) })
    }
    function removeRule(i) { withRules(r => r.splice(i, 1)) }
    function addRule() { withRules(r => r.push({ pattern: "m.m.m.x", label: "", perms: [] })) }

    // Copying another device's rules (rules.get / rules.list).
    readonly property var connectedDevices: { App.peers.revision; return App.peers.connectedDevices() }
    property string copyStatus: ""
    property var offeredRules: []
    property string offeredFrom: ""
    property string offeredFingerprint: ""
    // Names can be copied: a device is told apart by its fingerprint, whose
    // start is shown next to its name ("not proven" until it proved it).
    function shortFingerprint(device) {
        return device && device.identityState === "verified" && device.fingerprint
               ? device.fingerprint.substring(0, 9) : qsTr("not proven")
    }
    function deviceLabel(device) { return device.displayName + "  ·  " + shortFingerprint(device) }
    Connections {
        target: App
        function onNetworkRulesOffered(devId, deviceName, rules, error) {
            if (error) {
                dlg.copyStatus = error === "forbidden"
                    ? qsTr("%1 would not share its rules: it has no special or network rules for this device.").arg(deviceName)
                    : qsTr("%1 could not share its rules (%2).").arg(deviceName).arg(error)
                return
            }
            const device = App.peers.get(devId)
            dlg.offeredRules = rules
            dlg.offeredFrom = deviceName
            dlg.offeredFingerprint = device.identityState === "verified" ? device.fingerprint : ""
            dlg.copyStatus = ""
            importRules.open()
        }
    }
    Popup {
        id: importRules
        anchors.centerIn: Overlay.overlay
        modal: true
        padding: 20
        width: 520
        background: Rectangle { color: Theme.panel; border.color: Theme.popupBorder; radius: 8 }
        ColumnLayout {
            width: parent.width
            spacing: 10
            Text {
                text: qsTr("Rules from %1").arg(dlg.offeredFrom)
                textFormat: Text.PlainText
                color: Theme.text
                font.pixelSize: 16
                font.bold: true
            }
            RowLayout {
                spacing: 8
                UIcon {
                    name: dlg.offeredFingerprint ? "fingerprint" : "warning"
                    size: 16
                    color: dlg.offeredFingerprint ? Theme.textSecondary : Theme.error
                }
                Text {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    textFormat: Text.PlainText
                    color: dlg.offeredFingerprint ? Theme.text : Theme.error
                    font.family: dlg.offeredFingerprint ? "Consolas" : ""
                    text: dlg.offeredFingerprint ? dlg.offeredFingerprint
                                                 : qsTr("This device has not proved its identity.")
                }
            }
            Help {
                text: dlg.offeredRules.length === 0
                      ? qsTr("It has no network rules on this topic.")
                      : qsTr("Its network rules on this topic, first match first:")
            }
            Repeater {
                model: dlg.offeredRules
                delegate: Text {
                    required property var modelData
                    required property int index
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    textFormat: Text.PlainText
                    color: Theme.text
                    text: (index + 1) + ".  " + modelData.pattern + (modelData.label ? "  —  " + modelData.label : "")
                          + "  ·  " + (modelData.perms.length === 0 ? qsTr("no permissions")
                                       : modelData.perms.map(id => dlg.permissionLabel(id)).join(", "))
                }
            }
            RowLayout {
                Layout.topMargin: 6
                Item { Layout.fillWidth: true }
                UButton { text: qsTr("Cancel"); onClicked: importRules.close() }
                UButton {
                    text: qsTr("Add to Mine")
                    enabled: dlg.offeredRules.length > 0
                    onClicked: {
                        dlg.withRules(r => { for (const rule of dlg.offeredRules) r.push(rule) })
                        dlg.copyStatus = qsTr("Added %1 rule(s). Click OK or Apply to save.").arg(dlg.offeredRules.length)
                        importRules.close()
                    }
                }
                UButton {
                    text: qsTr("Replace Mine")
                    primary: true
                    onClicked: {
                        draft.networkRules = JSON.parse(JSON.stringify(dlg.offeredRules))
                        dlg.copyStatus = qsTr("Replaced with %1 rule(s). Click OK or Apply to save.").arg(dlg.offeredRules.length)
                        importRules.close()
                    }
                }
            }
        }
    }
    function permissionLabel(id) {
        const d = App.permissionDefinitions.find(p => p.id === id)
        return d ? Theme.serviceLabel(d.service) + " › " + d.label : id
    }
    readonly property bool modified: draft.server.trim() !== App.serverAddress
                                     || draft.deviceName.trim() !== App.deviceName
                                     || !sameServices(draft.services, App.offeredServices)
                                     || draft.autoConnect !== App.autoConnect
                                     || draft.rememberTopic !== App.rememberTopic
                                     || draft.verboseLogging !== App.verboseLogging
                                     || draft.colorScheme !== App.colorScheme
                                     || draft.relayMode !== App.relayMode
                                     || draft.debugOnStartup !== App.debugOnStartup
                                     || draft.closeToTray !== App.closeToTray
                                     || draft.launchAtLogin !== App.launchAtLogin
                                     || draft.startHidden !== App.startHidden
                                     || draft.sharedFolders.join("
") !== App.sharedFolders.join("
")
                                     || !sameServices(draft.readOnlyFolders, App.readOnlyFolders)
                                     || draft.downloadFolder !== App.downloadFolder
                                     || draft.maxUploadMB !== App.maxUploadMB
                                     || !sameServices(draft.defaultPermissions, App.defaultPermissions)
                                     || draft.removeSpecial.length > 0
                                     || JSON.stringify(draft.networkRules) !== JSON.stringify(App.networkRules)
                                     || JSON.stringify(draft.screen) !== JSON.stringify(App.screen.settings)
                                     || JSON.stringify(draft.camera) !== JSON.stringify(App.camera.settings)
                                     || JSON.stringify(draft.display) !== JSON.stringify(App.display.settings)

    function load() {
        draft.server = App.serverAddress
        draft.deviceName = App.deviceName
        draft.services = App.offeredServices.slice()
        draft.autoConnect = App.autoConnect
        draft.rememberTopic = App.rememberTopic
        draft.verboseLogging = App.verboseLogging
        draft.colorScheme = App.colorScheme
        draft.relayMode = App.relayMode
        draft.debugOnStartup = App.debugOnStartup
        draft.closeToTray = App.closeToTray
        draft.launchAtLogin = App.launchAtLogin
        draft.startHidden = App.startHidden
        draft.sharedFolders = App.sharedFolders.slice()
        draft.readOnlyFolders = App.readOnlyFolders.slice()
        draft.downloadFolder = App.downloadFolder
        draft.maxUploadMB = App.maxUploadMB
        draft.defaultPermissions = App.defaultPermissions.slice()
        draft.removeSpecial = []
        draft.networkRules = JSON.parse(JSON.stringify(App.networkRules))
        draft.screen = Object.assign({}, App.screen.settings)
        draft.camera = Object.assign({}, App.camera.settings)
        draft.display = Object.assign({}, App.display.settings)
    }

    function apply() {
        App.applySettings({
            server: draft.server,
            deviceName: draft.deviceName,
            services: draft.services,
            autoConnect: draft.autoConnect,
            rememberTopic: draft.rememberTopic,
            verboseLogging: draft.verboseLogging,
            colorScheme: draft.colorScheme,
            relayMode: draft.relayMode,
            debugOnStartup: draft.debugOnStartup,
            closeToTray: draft.closeToTray,
            launchAtLogin: draft.launchAtLogin,
            startHidden: draft.startHidden,
            sharedFolders: draft.sharedFolders,
            readOnlyFolders: draft.readOnlyFolders.filter(f => draft.sharedFolders.indexOf(f) >= 0),
            downloadFolder: draft.downloadFolder,
            maxUploadMB: draft.maxUploadMB,
            defaultPermissions: draft.defaultPermissions,
            removeSpecial: draft.removeSpecial,
            networkRules: draft.networkRules,
            screen: draft.screen,
            camera: draft.camera,
            display: draft.display
        })
        load()  // the draft now matches what was applied
    }

    function setDefaultPermission(permission, on) {
        const next = draft.defaultPermissions.filter(p => p !== permission)
        if (on) next.push(permission)
        draft.defaultPermissions = next
    }
    function setCameraSetting(key, value) {
        const next = Object.assign({}, draft.camera)
        next[key] = value
        draft.camera = next
    }
    function setScreenSetting(key, value) {
        const next = Object.assign({}, draft.screen)
        next[key] = value
        draft.screen = next
    }
    function setDisplaySetting(key, value) {
        const next = Object.assign({}, draft.display)
        next[key] = value
        draft.display = next
    }
    function toggleRemoveSpecial(key) {
        draft.removeSpecial = draft.removeSpecial.indexOf(key) >= 0 ? draft.removeSpecial.filter(k => k !== key)
                                                                    : draft.removeSpecial.concat([key])
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
        currentPage = pageId && pageById(pageId) ? pageId : "general"
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
        Layout.minimumWidth: 0
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

            TitledSeparator { text: qsTr("Relay") }
            RowLayout {
                FieldLabel { text: qsTr("Connection:") }
                UComboBox {
                    Layout.preferredWidth: 220
                    model: [qsTr("Auto"), qsTr("Always relay"), qsTr("Direct only")]
                    currentIndex: draft.relayMode
                    onActivated: draft.relayMode = currentIndex
                }
            }
            Indented {
                Layout.leftMargin: 148
                Help {
                    text: draft.relayMode === 1
                          ? qsTr("Skip NAT punching and always connect through the server's relay.")
                          : draft.relayMode === 2
                            ? qsTr("Never send screen or file data through the relay; a direct connection is required.")
                            : qsTr("Connect directly when possible, and through the relay when it is not.")
                }
                Help {
                    visible: App.active && draft.relayMode !== App.relayMode
                    color: Theme.warning
                    text: qsTr("Reconnects to apply.")
                }
            }
        }
    }

    // How the app looks and behaves on this computer.
    Component {
        id: generalPage
        ColumnLayout {
            spacing: 8
            TitledSeparator { text: qsTr("Appearance") }
            RowLayout {
                FieldLabel { text: qsTr("Color scheme:") }
                UComboBox {
                    Layout.preferredWidth: 220
                    model: Theme.schemes.map(s => s.label)
                    currentIndex: Math.max(0, Theme.schemes.findIndex(s => s.id === draft.colorScheme))
                    onActivated: draft.colorScheme = Theme.schemes[currentIndex].id
                }
            }
            Indented {
                Layout.leftMargin: 148
                Help {
                    text: qsTr("Recolors the app when you save. “Follow system” matches your OS light or dark setting.")
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
            UCheckBox {
                text: qsTr("Start uConnect when you sign in")
                checked: draft.launchAtLogin
                onToggled: draft.launchAtLogin = checked
            }
            UCheckBox {
                Layout.leftMargin: 24
                text: qsTr("Start hidden in the system tray")
                enabled: draft.launchAtLogin && App.tray.available
                checked: draft.startHidden && App.tray.available
                onToggled: draft.startHidden = checked
            }
            Indented {
                Layout.leftMargin: 48
                Help {
                    text: App.tray.available
                          ? qsTr("When it starts at sign-in, it waits in the tray without opening its window. Opening uConnect yourself always shows it.")
                          : qsTr("This desktop has no system tray, so the window always opens.")
                }
            }

            TitledSeparator { text: qsTr("Closing the window") }
            UCheckBox {
                text: qsTr("Keep running in the system tray")
                enabled: App.tray.available
                checked: draft.closeToTray
                onToggled: draft.closeToTray = checked
            }
            Indented {
                Help {
                    text: App.tray.available
                          ? qsTr("Stays connected, and keeps sharing, with the window closed. Open it again or exit from the tray icon.")
                          : qsTr("This desktop has no system tray, so closing the window exits.")
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
                    text: qsTr("This device's identity. Before another device gives this one special rules, its user should check that it shows exactly this fingerprint.")
                }
            }
        }
    }

    Component {
        id: permissionsPage
        ColumnLayout {
            spacing: 8

            TitledSeparator { text: qsTr("How Permissions Are Decided") }
            Help {
                text: qsTr("Each device gets what its special rules, this topic's network rule it matches and the defaults below grant, together. Special rules marked as an exception apply alone.")
            }
            ULink {
                text: qsTr("Network rules for this topic...")
                onClicked: dlg.currentPage = "network"
            }

            TitledSeparator { text: qsTr("Default Permissions") }
            Help {
                text: qsTr("What every device may do here, including devices whose identity is unknown. Special and network rules add to these.")
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
                text: draft.defaultPermissions.some(id => App.permissionDefinitions.some(d => d.id === id && d.sensitive))
                      ? qsTr("Any device with this topic's invite link gets these permissions, including camera, screen or control. Special or network rules are usually the better place for those.")
                      : qsTr("Any device with this topic's invite link gets these permissions.")
            }

            TitledSeparator { text: qsTr("Devices With Special Rules") }
            Help {
                visible: App.specialDevices.length === 0
                text: qsTr("None yet. Give a device special rules from its tab, or by right-clicking it in Devices.")
            }
            Repeater {
                model: App.specialDevices
                delegate: RowLayout {
                    id: specialRow
                    required property var modelData
                    readonly property bool removing: draft.removeSpecial.indexOf(modelData.key) >= 0

                    Layout.fillWidth: true
                    spacing: 12
                    opacity: removing ? 0.5 : 1

                    Avatar {
                        name: specialRow.modelData.name
                        seed: specialRow.modelData.key
                        size: 28
                    }
                    ColumnLayout {
                        spacing: 1
                        Text {
                            text: specialRow.modelData.name
                            textFormat: Text.PlainText
                            color: Theme.text
                            font.strikeout: specialRow.removing
                        }
                        Text {
                            text: specialRow.modelData.fingerprint
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
                            const n = specialRow.modelData.permissions.length
                            const count = n === 0 ? qsTr("No permissions") : n === 1 ? qsTr("1 permission") : qsTr("%1 permissions").arg(n)
                            return specialRow.modelData.exception ? count + "  ·  " + qsTr("exception") : count
                        }
                    }
                    UButton {
                        text: specialRow.removing ? qsTr("Keep") : qsTr("Remove")
                        onClicked: dlg.toggleRemoveSpecial(specialRow.modelData.key)
                    }
                }
            }
            Help {
                visible: App.specialDevices.length > 0
                text: qsTr("Change a device's special rules from its tab. Removing them leaves it to the network rules and defaults.")
            }
        }
    }

    Component {
        id: networkPage
        ColumnLayout {
            spacing: 8

            TitledSeparator { text: qsTr("Network Rules for This Topic") }
            Help {
                text: qsTr("Choose what devices may do here by where they connect from. The first rule a device matches applies, adding to the defaults and to its special rules (unless those are an exception). A device matches only once it has proved its identity, over a direct connection: through the relay its address is unknown.")
            }
            Help {
                text: qsTr("Patterns have four parts: x for any value, m for the same as this device's address, or a number. m.m.m.x is this device's /24 network, 100.x.x.x anything starting with 100, x.x.x.x any address.")
            }
            Help {
                color: Theme.text
                text: App.localAddresses.length > 0 ? qsTr("This device: %1").arg(App.localAddresses.join(",  "))
                                                    : qsTr("This device has no IPv4 address right now.")
            }
            Help {
                visible: !App.topicLinkValid
                color: Theme.warning
                text: qsTr("Join a topic to set its rules: they are kept per topic.")
            }

            // By count, so editing a field does not rebuild (and unfocus) it.
            Repeater {
                model: draft.networkRules.length
                delegate: Rectangle {
                    id: ruleCard
                    required property int index
                    readonly property var rule: draft.networkRules[index] || ({ pattern: "", label: "", perms: [] })
                    readonly property string problem: App.networkPatternProblem(rule.pattern)

                    Layout.fillWidth: true
                    Layout.topMargin: 4
                    implicitHeight: ruleColumn.implicitHeight + 24
                    radius: 6
                    color: Theme.canvas
                    border.color: problem !== "" ? Theme.error : Theme.border

                    ColumnLayout {
                        id: ruleColumn
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 8
                        RowLayout {
                            spacing: 8
                            Text {
                                text: (ruleCard.index + 1) + "."
                                color: Theme.textSecondary
                                font.bold: true
                            }
                            UTextField {
                                Layout.preferredWidth: 150
                                text: ruleCard.rule.pattern
                                placeholderText: "m.m.m.x"
                                font.family: "Consolas, monospace"
                                invalid: ruleCard.problem !== ""
                                onTextEdited: dlg.setRule(ruleCard.index, "pattern", text)
                            }
                            UTextField {
                                Layout.fillWidth: true
                                text: ruleCard.rule.label
                                placeholderText: qsTr("Name (optional), e.g. Home")
                                maximumLength: 64
                                onTextEdited: dlg.setRule(ruleCard.index, "label", text)
                            }
                            UToolButton {
                                text: "↑"
                                tip: qsTr("Check this rule earlier")
                                enabled: ruleCard.index > 0
                                onClicked: dlg.moveRule(ruleCard.index, -1)
                            }
                            UToolButton {
                                text: "↓"
                                tip: qsTr("Check this rule later")
                                enabled: ruleCard.index < draft.networkRules.length - 1
                                onClicked: dlg.moveRule(ruleCard.index, 1)
                            }
                            UButton {
                                text: qsTr("Remove")
                                onClicked: dlg.removeRule(ruleCard.index)
                            }
                        }
                        Help {
                            visible: ruleCard.problem !== ""
                            color: Theme.error
                            text: ruleCard.problem
                        }
                        PermissionEditor {
                            Layout.fillWidth: true
                            granted: ruleCard.rule.perms || []
                            allowSensitive: true
                            onToggled: (permission, on) => dlg.setRulePermission(ruleCard.index, permission, on)
                        }
                    }
                }
            }
            UButton {
                Layout.topMargin: 4
                text: qsTr("Add Rule")
                enabled: App.topicLinkValid && draft.networkRules.length < 64
                onClicked: dlg.addRule()
            }

            TitledSeparator { text: qsTr("Copy From Another Device") }
            Help {
                text: qsTr("Take the rules another of your devices uses on this topic, to set up quickly on a network you already use. It shares them only if it has special or network rules for this device. You review them before anything changes.")
            }
            RowLayout {
                spacing: 8
                UComboBox {
                    id: copyFrom
                    Layout.preferredWidth: 320
                    model: dlg.connectedDevices.map(d => dlg.deviceLabel(d))
                    enabled: dlg.connectedDevices.length > 0
                }
                UButton {
                    text: qsTr("Request Rules")
                    enabled: dlg.connectedDevices.length > 0 && copyFrom.currentIndex >= 0
                    onClicked: {
                        dlg.copyStatus = qsTr("Asking %1...").arg(dlg.deviceLabel(dlg.connectedDevices[copyFrom.currentIndex]))
                        App.requestNetworkRules(dlg.connectedDevices[copyFrom.currentIndex].devId)
                    }
                }
            }
            Help {
                visible: dlg.connectedDevices.length === 0
                text: qsTr("No device is connected right now.")
            }
            Help {
                visible: dlg.copyStatus !== ""
                color: Theme.text
                text: dlg.copyStatus
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
                text: qsTr("Screen and camera requests need special or network rules that allow them, and host consent. Remote control also requires a control grant.")
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
                visible: svcPage.service === "control"
                text: qsTr("Keyboard release and related options are configured under Screen Sharing.")
            }
            ColumnLayout {
                visible: svcPage.service === "camera"
                Layout.fillWidth: true
                spacing: 8
                Help {
                    text: App.camera.available
                          ? qsTr("Only devices your special or network rules allow may request your camera, and each request asks first. The viewer picks a camera and a quality up to the caps below.")
                          : qsTr("This build needs Qt Multimedia and a shared LGPL FFmpeg SDK for camera sharing.")
                }
                FieldLabel { text: qsTr("Cameras to offer") }
                Help {
                    visible: (App.camera.cameras || []).length === 0
                    text: qsTr("No cameras found on this device.")
                }
                Repeater {
                    model: App.camera.cameras || []
                    delegate: UCheckBox {
                        required property var modelData
                        text: modelData.name
                        // An empty offered list means every camera is offered.
                        checked: (draft.camera.offered || []).length === 0
                                 || (draft.camera.offered || []).indexOf(modelData.id) >= 0
                        onToggled: {
                            const all = (App.camera.cameras || []).map(c => c.id)
                            let set = (draft.camera.offered || []).slice()
                            if (set.length === 0) set = all.slice()  // expand "all" before editing
                            const i = set.indexOf(modelData.id)
                            if (checked && i < 0) set.push(modelData.id)
                            else if (!checked && i >= 0) set.splice(i, 1)
                            if (set.length === 0) { checked = true; return }  // always offer at least one
                            const offeredLen = set.length
                            if (set.length === all.length) set = []  // all ticked = "offer all"
                            dlg.setCameraSetting("offered", set)
                            // The default index points into the offered list; keep it in range.
                            if ((draft.camera.monitor || 0) >= offeredLen) dlg.setCameraSetting("monitor", 0)
                        }
                    }
                }
                FieldLabel { text: qsTr("Camera to offer by default") }
                UComboBox {
                    Layout.preferredWidth: 280
                    readonly property var offeredCams: {
                        const off = draft.camera.offered || []
                        const all = App.camera.cameras || []
                        return off.length === 0 ? all : all.filter(c => off.indexOf(c.id) >= 0)
                    }
                    enabled: offeredCams.length > 1
                    model: offeredCams.map(c => c.name)
                    currentIndex: draft.camera.monitor || 0
                    onActivated: index => dlg.setCameraSetting("monitor", index)
                }
                GridLayout {
                    columns: 2
                    columnSpacing: 12
                    rowSpacing: 8
                    FieldLabel { Layout.preferredWidth: 200; text: qsTr("Maximum FPS") }
                    USpinBox { from: 1; to: 120; value: draft.camera.maxFps || 30; onValueModified: dlg.setCameraSetting("maxFps", value) }
                    FieldLabel { Layout.preferredWidth: 200; text: qsTr("Maximum width") }
                    USpinBox { from: 320; to: 3840; stepSize: 160; value: draft.camera.maxWidth || 1280; onValueModified: dlg.setCameraSetting("maxWidth", value) }
                    FieldLabel { Layout.preferredWidth: 200; text: qsTr("Maximum viewers") }
                    USpinBox { from: 1; to: 8; value: draft.camera.maxViewers || 1; onValueModified: dlg.setCameraSetting("maxViewers", value) }
                }
            }
            Help {
                visible: svcPage.service !== "screen" && svcPage.service !== "camera" && svcPage.service !== "control" && svcPage.service !== "display"
                text: qsTr("Options for this service will appear here once it is implemented.")
            }
            ColumnLayout {
                visible: svcPage.service === "display"
                Layout.fillWidth: true
                spacing: 8
                Help { text: qsTr("Offering it lets devices your rules allow ask to use this screen as a monitor; each request asks first unless you allow it without asking. To use another device as a monitor for this one, pick it under Services > Extended Display.") }
                GridLayout {
                    columns: 2
                    columnSpacing: 12
                    rowSpacing: 8
                    FieldLabel { Layout.preferredWidth: 200; text: qsTr("Maximum bitrate (Mbit/s)") }
                    USpinBox { from: 2; to: 100; value: (draft.display.maxBitrate || 40000000) / 1000000; onValueModified: dlg.setDisplaySetting("maxBitrate", value * 1000000) }
                }
                UCheckBox {
                    text: qsTr("Keep the screen awake while showing a display")
                    checked: draft.display.keepAwake !== false
                    onToggled: dlg.setDisplaySetting("keepAwake", checked)
                }
                TitledSeparator { text: qsTr("Virtual Display Driver"); visible: App.display.driver.needsDriver === true }
                ColumnLayout {
                    visible: App.display.driver.needsDriver === true
                    Layout.fillWidth: true
                    spacing: 6
                    Help {
                        text: App.display.driver.state === "ready" ? qsTr("Installed. Windows asks for administrator permission to change it.")
                              : App.display.driver.detail || ""
                    }
                    RowLayout {
                        spacing: 8
                        UButton {
                            text: App.display.driver.state === "outdated" ? qsTr("Update") : App.display.driver.state === "ready" ? qsTr("Reinstall") : qsTr("Install")
                            enabled: App.display.driver.bundled === true && App.display.driver.busy !== true
                            onClicked: App.display.installDriver()
                        }
                        UButton {
                            text: qsTr("Remove")
                            enabled: App.display.driver.state !== "notInstalled" && App.display.driver.busy !== true
                            onClicked: App.display.removeDriver()
                        }
                        UButton { text: qsTr("Check again"); onClicked: App.display.refreshDriver() }
                    }
                    Help {
                        visible: App.display.driver.bundled !== true
                        text: qsTr("This build has no driver package (drivers/uconnect-vdd next to the program).")
                    }
                }
            }
            ColumnLayout {
                visible: svcPage.service === "screen"
                Layout.fillWidth: true
                spacing: 8
                Help { text: App.screen.available ? qsTr("Only devices your special or network rules allow may request your screen. Each request asks for consent unless you grant view without asking.") : qsTr("This build needs Qt Multimedia and a shared LGPL FFmpeg SDK for screen sharing.") }
                GridLayout {
                    columns: 2
                    columnSpacing: 12
                    rowSpacing: 8
                    FieldLabel { Layout.preferredWidth: 200; text: qsTr("Default monitor (0 = first)") }
                    USpinBox { from: 0; to: 15; value: draft.screen.monitor || 0; onValueModified: dlg.setScreenSetting("monitor", value) }
                    FieldLabel { Layout.preferredWidth: 200; text: qsTr("Maximum FPS") }
                    USpinBox { from: 1; to: 165; value: draft.screen.maxFps || 30; onValueModified: dlg.setScreenSetting("maxFps", value) }
                    FieldLabel { Layout.preferredWidth: 200; text: qsTr("Maximum width") }
                    USpinBox { from: 320; to: 3840; stepSize: 160; value: draft.screen.maxWidth || 1920; onValueModified: dlg.setScreenSetting("maxWidth", value) }
                    FieldLabel { Layout.preferredWidth: 200; text: qsTr("Maximum bitrate (Mbit/s)") }
                    USpinBox { from: 1; to: 20; value: (draft.screen.maxBitrate || 8000000) / 1000000; onValueModified: dlg.setScreenSetting("maxBitrate", value * 1000000) }
                    FieldLabel { Layout.preferredWidth: 200; text: qsTr("Maximum viewers") }
                    USpinBox { from: 1; to: 8; value: draft.screen.maxViewers || 1; onValueModified: dlg.setScreenSetting("maxViewers", value) }
                    FieldLabel { Layout.preferredWidth: 200; text: qsTr("Stop sharing shortcut") }
                    UTextField { Layout.preferredWidth: 220; text: draft.screen.stopShortcut || "Ctrl+Alt+Shift+S"; onTextEdited: dlg.setScreenSetting("stopShortcut", text) }
                    FieldLabel { Layout.preferredWidth: 200; text: qsTr("Release keyboard") }
                    UComboBox {
                        model: [qsTr("Right Ctrl"), qsTr("Escape"), "F12"]
                        currentIndex: [228, 41, 69].indexOf(draft.screen.releaseUsage || 228)
                        onActivated: index => dlg.setScreenSetting("releaseUsage", [228, 41, 69][index])
                    }
                }
                Help { text: qsTr("Viewers share one source and encoder. One viewer may control it at a time. Camera sharing uses these quality limits too.") }
                UCheckBox {
                    text: qsTr("Use native GPU capture when available")
                    checked: draft.screen.gpuCapture !== false
                    onToggled: dlg.setScreenSetting("gpuCapture", checked)
                }
                UCheckBox {
                    text: qsTr("Enable software fallback")
                    checked: draft.screen.softwareCodec || false
                    onToggled: dlg.setScreenSetting("softwareCodec", checked)
                }
                Help { text: "OpenH264 Video Codec provided by Cisco Systems, Inc." }
                UTextField {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    placeholderText: qsTr("Path to separately installed Cisco OpenH264 2.6 library")
                    text: draft.screen.codecLibrary || ""
                    onTextEdited: dlg.setScreenSetting("codecLibrary", text)
                }
                Help { text: qsTr("Use scripts/install-openh264.py to download the Cisco binary separately. Its license is in docs/OpenH264-BINARY-LICENSE.txt.") }
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

            TitledSeparator { text: qsTr("Invite QR Code") }
            ColumnLayout {
                id: qrSection
                property bool shown: false
                spacing: 8
                Help {
                    text: App.topicLinkValid
                          ? qsTr("Scan it with a phone's camera to open uConnect there and join this topic.")
                          : qsTr("Connect to a topic to get its invite code.")
                }
                UButton {
                    visible: App.topicLinkValid
                    text: qrSection.shown ? qsTr("Hide QR Code") : qsTr("Show QR Code")
                    onClicked: qrSection.shown = !qrSection.shown
                }
                Rectangle {
                    visible: qrSection.shown && App.topicLinkValid
                    Layout.topMargin: 4
                    implicitWidth: 200
                    implicitHeight: 200
                    radius: 10
                    color: "white"
                    QrCode {
                        anchors.fill: parent
                        anchors.margins: 10
                        text: qrSection.shown ? App.topicLink : ""
                        color: "black"
                        background: "white"
                    }
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
        case "general": return generalPage
        case "connection": return connectionPage
        case "device": return devicePage
        case "services": return servicesPage
        case "storage": return storagePage
        case "camera":
        case "screen":
        case "control":
        case "display": return servicePage
        case "permissions": return permissionsPage
        case "network": return networkPage
        case "security": return securityPage
        case "advanced": return advancedPage
        }
        return generalPage
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
                    id: navList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: dlg.visiblePages
                    // A page under another (Services > Storage...) hangs off a
                    // vertical guide that runs down from its parent's icon.
                    delegate: Item {
                        id: entry
                        required property var modelData
                        required property int index
                        readonly property bool selected: modelData.id === dlg.currentPage
                        readonly property bool child: modelData.depth > 0
                        readonly property var next: index + 1 < dlg.visiblePages.length ? dlg.visiblePages[index + 1] : null
                        readonly property bool lastChild: child && (!next || next.depth === 0)
                        // The guide's x: under the parent's icon.
                        readonly property int guideX: 19

                        width: ListView.view.width
                        height: child ? 28 : 32

                        Rectangle {
                            visible: entry.child
                            x: entry.guideX
                            width: 1
                            y: 0
                            height: entry.lastChild ? parent.height / 2 : parent.height
                            color: Theme.textDisabled
                            opacity: 0.6
                        }
                        Rectangle {
                            visible: entry.child
                            x: entry.guideX
                            y: parent.height / 2
                            width: 9
                            height: 1
                            color: Theme.textDisabled
                            opacity: 0.6
                        }

                        Rectangle {
                            id: pill
                            x: entry.child ? entry.guideX + 12 : 0
                            width: parent.width - x
                            height: parent.height - 2
                            anchors.verticalCenter: parent.verticalCenter
                            radius: Theme.radius
                            color: entry.selected ? Theme.selection : entryArea.containsMouse ? Theme.hover : "transparent"

                            // The current page's accent bar.
                            Rectangle {
                                visible: entry.selected
                                x: 0
                                width: 3
                                height: parent.height - 10
                                anchors.verticalCenter: parent.verticalCenter
                                radius: 1.5
                                color: Theme.accent
                            }
                            UIcon {
                                id: navIcon
                                x: entry.child ? 8 : 10
                                anchors.verticalCenter: parent.verticalCenter
                                name: entry.modelData.icon || ""
                                size: entry.child ? 15 : 17
                                color: entry.selected ? Theme.accent : Theme.textSecondary
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.left: navIcon.right
                                anchors.leftMargin: 9
                                anchors.right: parent.right
                                anchors.rightMargin: 6
                                elide: Text.ElideRight
                                text: entry.modelData.title
                                color: entry.child && !entry.selected ? Theme.textSecondary : Theme.text
                                font.bold: dlg.query.length > 0 && dlg.matches(entry.modelData)
                            }
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
