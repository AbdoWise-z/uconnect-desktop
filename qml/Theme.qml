pragma Singleton
import QtQuick

// Colours and metrics modelled on the JetBrains "New UI" dark theme.
QtObject {
    // Surfaces
    readonly property color canvas: "#1E1F22"      // editor area, main canvas
    readonly property color panel: "#2B2D30"       // tool windows, bars, dialogs
    readonly property color popup: "#2B2D30"
    readonly property color divider: "#1E1F22"     // between panels
    readonly property color border: "#393B40"      // around components
    readonly property color popupBorder: "#43454A"
    readonly property color hover: "#393B40"
    readonly property color pressed: "#43454A"
    readonly property color selection: "#2E436E"   // focused selection
    readonly property color selectionInactive: "#43454A"

    // Inputs
    readonly property color input: "#2B2D30"
    readonly property color inputBorder: "#4E5157"
    readonly property color focus: "#3574F0"

    // Text
    readonly property color text: "#DFE1E5"
    readonly property color textSecondary: "#868A91"
    readonly property color textDisabled: "#5A5D63"
    readonly property color link: "#548AF7"

    // Accents
    readonly property color accent: "#3574F0"
    readonly property color accentHover: "#366ACE"
    readonly property color accentPressed: "#2F5EB8"
    readonly property color success: "#5FB865"
    readonly property color warning: "#F2C55C"
    readonly property color error: "#DB5C5C"
    readonly property color errorBackground: "#402929"
    readonly property color infoBackground: "#25324D"

    // Metrics
    readonly property int fontSize: 13
    readonly property int smallFontSize: 12
    readonly property int controlHeight: 28
    readonly property int radius: 4

    // Mirrors uconnect::PeerState.
    readonly property int stateUnknown: 0
    readonly property int stateProbing: 1
    readonly property int stateHandshaking: 2
    readonly property int stateConnected: 3
    readonly property int stateFailed: 4
    readonly property int stateClosed: 5

    readonly property var services: ["storage", "camera", "screen", "control"]

    function serviceLabel(service) {
        switch (service) {
        case "storage": return qsTr("Storage")
        case "camera": return qsTr("Camera")
        case "screen": return qsTr("Screen Sharing")
        case "control": return qsTr("Remote Control")
        }
        return service
    }

    function serviceDescription(service) {
        switch (service) {
        case "storage": return qsTr("Browse shared folders and download files from them.")
        case "camera": return qsTr("View a live feed from the device's camera.")
        case "screen": return qsTr("Watch the device's screen.")
        case "control": return qsTr("See the device's screen and control its mouse and keyboard.")
        }
        return ""
    }

    function stateColor(code) {
        switch (code) {
        case stateConnected: return success
        case stateProbing:
        case stateHandshaking: return warning
        case stateFailed: return error
        }
        return textSecondary
    }

    function stateLabel(code, closeReason) {
        switch (code) {
        case stateConnected: return qsTr("Connected")
        case stateProbing: return qsTr("Reaching...")
        case stateHandshaking: return qsTr("Securing...")
        case stateFailed: return qsTr("Unreachable")
        case stateClosed: return closeReason ? qsTr("Disconnected (%1)").arg(closeReason) : qsTr("Disconnected")
        }
        return qsTr("Available")
    }

    readonly property var avatarColors: ["#C75450", "#D9A343", "#5C9A5C", "#3574F0", "#9C6ADE", "#E07B53", "#3FA3A3"]

    function avatarColor(seed) {
        let h = 0
        for (let i = 0; i < seed.length; ++i)
            h = (h * 31 + seed.charCodeAt(i)) % 1000003
        return avatarColors[h % avatarColors.length]
    }

    function initials(name) {
        const words = name.trim().split(/\s+/).filter(w => w.length > 0)
        if (words.length === 0) return "?"
        if (words.length === 1) return words[0].substring(0, 2).toUpperCase()
        return (words[0][0] + words[1][0]).toUpperCase()
    }

    function formatBytes(n) {
        if (n === undefined || n === null) return "0 B"
        const units = ["B", "KB", "MB", "GB", "TB"]
        let i = 0
        while (n >= 1024 && i < units.length - 1) {
            n /= 1024
            i++
        }
        return (i === 0 ? n : n.toFixed(1)) + " " + units[i]
    }
}
