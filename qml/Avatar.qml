import QtQuick

// A coloured square with a device-type glyph (phone or computer) when the OS is
// known, and JetBrains-style initials otherwise.
Rectangle {
    id: avatar

    property string name: ""
    property string seed: name
    property string os: ""
    property int size: 20

    implicitWidth: size
    implicitHeight: size
    radius: Math.max(3, size / 5)
    color: Theme.avatarColor(seed)

    readonly property string kind: {
        const o = avatar.os.toLowerCase()
        if (o.indexOf("android") >= 0 || o.indexOf("ios") >= 0 || o.indexOf("iphone") >= 0
                || o.indexOf("ipad") >= 0 || o.indexOf("phone") >= 0) return "phone"
        if (o.indexOf("win") >= 0 || o.indexOf("mac") >= 0 || o.indexOf("darwin") >= 0
                || o.indexOf("linux") >= 0 || o.indexOf("desktop") >= 0) return "computer"
        return ""
    }

    Text {
        anchors.centerIn: parent
        visible: avatar.kind === ""
        text: Theme.initials(avatar.name)
        textFormat: Text.PlainText
        color: "#FFFFFF"
        font.pixelSize: Math.round(avatar.size * 0.42)
        font.bold: true
    }

    Canvas {
        id: glyph
        anchors.centerIn: parent
        visible: avatar.kind !== ""
        width: avatar.size
        height: avatar.size
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            ctx.strokeStyle = "#FFFFFF"
            ctx.lineWidth = Math.max(1, avatar.size * 0.06)
            ctx.lineJoin = "round"
            ctx.lineCap = "round"
            const s = avatar.size
            if (avatar.kind === "phone") {
                const bw = s * 0.42, bh = s * 0.62, x = (s - bw) / 2, y = (s - bh) / 2, r = bw * 0.22
                roundRect(ctx, x, y, bw, bh, r); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(x + bw * 0.3, y + bh * 0.12); ctx.lineTo(x + bw * 0.7, y + bh * 0.12); ctx.stroke()
            } else {
                const sw = s * 0.64, sh = s * 0.44, x = (s - sw) / 2, y = s * 0.24, r = sh * 0.16
                roundRect(ctx, x, y, sw, sh, r); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(s * 0.5, y + sh); ctx.lineTo(s * 0.5, s * 0.74); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(s * 0.34, s * 0.76); ctx.lineTo(s * 0.66, s * 0.76); ctx.stroke()
            }
        }
        function roundRect(ctx, x, y, w, h, r) {
            ctx.beginPath()
            ctx.moveTo(x + r, y); ctx.lineTo(x + w - r, y); ctx.arcTo(x + w, y, x + w, y + r, r)
            ctx.lineTo(x + w, y + h - r); ctx.arcTo(x + w, y + h, x + w - r, y + h, r)
            ctx.lineTo(x + r, y + h); ctx.arcTo(x, y + h, x, y + h - r, r)
            ctx.lineTo(x, y + r); ctx.arcTo(x, y, x + r, y, r); ctx.closePath()
        }
        Connections {
            target: avatar
            function onKindChanged() { glyph.requestPaint() }
            function onSizeChanged() { glyph.requestPaint() }
        }
    }
}
