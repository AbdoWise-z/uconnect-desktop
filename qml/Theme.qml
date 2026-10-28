pragma Singleton
import QtQuick
import UConnect

// Colours and metrics. The default palette is modelled on the JetBrains
// "New UI" dark theme; several alternates are offered and the chosen one is
// stored as App.colorScheme. Every colour below reads from the active
// palette (`pal`), so switching schemes restyles the whole app live.
QtObject {
    id: theme

    // The palettes, keyed by scheme id. Each defines the same colour roles.
    readonly property var palettes: ({
        "dark": {
            canvas: "#1E1F22", panel: "#2B2D30", popup: "#2B2D30", divider: "#1E1F22",
            border: "#393B40", popupBorder: "#43454A", hover: "#393B40", pressed: "#43454A",
            selection: "#2E436E", selectionInactive: "#43454A",
            input: "#2B2D30", inputBorder: "#4E5157", focus: "#3574F0",
            text: "#DFE1E5", textSecondary: "#868A91", textDisabled: "#5A5D63", link: "#548AF7",
            accent: "#3574F0", accentHover: "#366ACE", accentPressed: "#2F5EB8",
            onAccent: "#FFFFFF", success: "#5FB865", warning: "#F2C55C", error: "#DB5C5C",
            errorBackground: "#402929", infoBackground: "#25324D"
        },
        // A deeper, near-black dark for low-light use.
        "darker": {
            canvas: "#121316", panel: "#1A1B1E", popup: "#1A1B1E", divider: "#0D0E10",
            border: "#2A2C30", popupBorder: "#34363B", hover: "#26282C", pressed: "#303236",
            selection: "#26406B", selectionInactive: "#303236",
            input: "#17181B", inputBorder: "#3A3D43", focus: "#3574F0",
            text: "#E6E8EC", textSecondary: "#7E828A", textDisabled: "#4E5157", link: "#548AF7",
            accent: "#3574F0", accentHover: "#366ACE", accentPressed: "#2F5EB8",
            onAccent: "#FFFFFF", success: "#5FB865", warning: "#F2C55C", error: "#DB5C5C",
            errorBackground: "#3A2525", infoBackground: "#1E2A42"
        },
        // A bright, clean light theme.
        "light": {
            canvas: "#FFFFFF", panel: "#F7F8FA", popup: "#FFFFFF", divider: "#EBECF0",
            border: "#D3D5DB", popupBorder: "#C9CCD6", hover: "#EDEEF2", pressed: "#E0E2E8",
            selection: "#D4E2FF", selectionInactive: "#E3E5E9",
            input: "#FFFFFF", inputBorder: "#BCBFC7", focus: "#3574F0",
            text: "#1E1F22", textSecondary: "#6C707A", textDisabled: "#A8ADB5", link: "#2E6CE6",
            accent: "#3574F0", accentHover: "#2F68DD", accentPressed: "#2A5FCC",
            onAccent: "#FFFFFF", success: "#3F9142", warning: "#B07D16", error: "#D13C3C",
            errorBackground: "#FBE9E9", infoBackground: "#E7EFFD"
        },
        // A vibrant, tinted dark with an indigo canvas and violet accent.
        "colorful": {
            canvas: "#1B1A2E", panel: "#252442", popup: "#252442", divider: "#15142A",
            border: "#3A3860", popupBorder: "#45426F", hover: "#302E52", pressed: "#3A3763",
            selection: "#3D3A7A", selectionInactive: "#3A3763",
            input: "#221F3D", inputBorder: "#4A4775", focus: "#8B7CFF",
            text: "#ECEAFF", textSecondary: "#A6A0D0", textDisabled: "#6E699A", link: "#9D8BFF",
            accent: "#7C6CFF", accentHover: "#6C5CF0", accentPressed: "#5D4EDB",
            onAccent: "#FFFFFF", success: "#46C8A0", warning: "#F2B84C", error: "#FF6B8A",
            errorBackground: "#3A2238", infoBackground: "#262450"
        }
    })

    // The schemes offered in Settings, in display order.
    readonly property var schemes: [
        { id: "system", label: qsTr("Follow system") },
        { id: "dark", label: qsTr("Dark") },
        { id: "darker", label: qsTr("Darker") },
        { id: "light", label: qsTr("Light") },
        { id: "colorful", label: qsTr("Colorful") }
    ]

    // The chosen scheme id, persisted in settings. "system" resolves to the
    // OS light/dark preference at runtime.
    readonly property string scheme: App.colorScheme.length > 0 ? App.colorScheme : "dark"
    readonly property string effectiveScheme: {
        if (scheme === "system")
            return Application.styleHints.colorScheme === Qt.Light ? "light" : "dark"
        return palettes.hasOwnProperty(scheme) ? scheme : "dark"
    }
    readonly property bool isDark: effectiveScheme !== "light"
    readonly property var pal: palettes[effectiveScheme]

    // Surfaces
    readonly property color canvas: pal.canvas            // editor area, main canvas
    readonly property color panel: pal.panel              // tool windows, bars, dialogs
    readonly property color popup: pal.popup
    readonly property color divider: pal.divider          // between panels
    readonly property color border: pal.border            // around components
    readonly property color popupBorder: pal.popupBorder
    readonly property color hover: pal.hover
    readonly property color pressed: pal.pressed
    readonly property color selection: pal.selection      // focused selection
    readonly property color selectionInactive: pal.selectionInactive

    // Inputs
    readonly property color input: pal.input
    readonly property color inputBorder: pal.inputBorder
    readonly property color focus: pal.focus

    // Text
    readonly property color text: pal.text
    readonly property color textSecondary: pal.textSecondary
    readonly property color textDisabled: pal.textDisabled
    readonly property color link: pal.link

    // Accents
    readonly property color accent: pal.accent
    readonly property color accentHover: pal.accentHover
    readonly property color accentPressed: pal.accentPressed
    readonly property color onAccent: pal.onAccent        // text/icons over an accent fill
    readonly property color success: pal.success
    readonly property color warning: pal.warning
    readonly property color error: pal.error
    readonly property color errorBackground: pal.errorBackground
    readonly property color infoBackground: pal.infoBackground

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

    readonly property var services: ["storage", "camera", "screen", "control", "display", "sync"]

    function serviceLabel(service) {
        switch (service) {
        case "storage": return qsTr("Storage")
        case "camera": return qsTr("Camera")
        case "screen": return qsTr("Screen Sharing")
        case "control": return qsTr("Remote Control")
        case "display": return qsTr("Extended Display")
        case "sync": return qsTr("Folder Sync")
        }
        return service
    }

    // The UIcon for a service, and the grant it takes to use it on a peer.
    function serviceIcon(service) {
        switch (service) {
        case "storage": return "folder"
        case "camera": return "videocam"
        case "screen": return "screen"
        case "control": return "mouse"
        case "display": return "cast"
        case "sync": return "sync"
        }
        return "apps"
    }
    function serviceGrant(service) {
        switch (service) {
        case "storage": return "storage.observe"
        case "camera": return "camera.view"
        case "screen": return "screen.view"
        case "control": return "control.input"
        case "display": return "display.show"
        }
        return ""
    }

    function serviceDescription(service) {
        switch (service) {
        case "storage": return qsTr("Browse shared folders, download files and upload new ones.")
        case "camera": return qsTr("View a live feed from the device's camera.")
        case "screen": return qsTr("Watch the device's screen.")
        case "control": return qsTr("See the device's screen and control its mouse and keyboard.")
        case "display": return qsTr("Let a nearby device you allow use this screen as an extra monitor.")
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
    // A rough time left: "45 s", "3 min", "2 h 10 min"; "" when unknown.
    function formatDuration(seconds) {
        if (seconds === undefined || seconds < 0 || !isFinite(seconds)) return ""
        if (seconds < 60) return qsTr("%1 s").arg(Math.max(1, Math.round(seconds)))
        if (seconds < 3600) return qsTr("%1 min").arg(Math.round(seconds / 60))
        const h = Math.floor(seconds / 3600)
        const m = Math.round((seconds % 3600) / 60)
        return m > 0 ? qsTr("%1 h %2 min").arg(h).arg(m) : qsTr("%1 h").arg(h)
    }
}
