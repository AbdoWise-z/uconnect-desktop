import QtQuick

// A small vector icon drawn with Canvas (the app has no icon font). Pick one by
// `name`; sizes and colours follow the Theme. Used by UIconButton and toolbars.
Item {
    id: root
    property string name: ""
    property int size: 16
    property color color: Theme.text

    implicitWidth: size
    implicitHeight: size

    Canvas {
        id: canvas
        anchors.fill: parent
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            ctx.strokeStyle = root.color
            ctx.fillStyle = root.color
            ctx.lineWidth = Math.max(1.4, root.size / 11)
            ctx.lineJoin = "round"
            ctx.lineCap = "round"
            const s = root.size
            const m = s * 0.17          // outer margin
            const a = s * 0.24          // corner-arm length

            switch (root.name) {
            case "fullscreen": {        // brackets at the corners, elbow outward
                ctx.beginPath(); ctx.moveTo(m, m + a); ctx.lineTo(m, m); ctx.lineTo(m + a, m); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(s - m - a, m); ctx.lineTo(s - m, m); ctx.lineTo(s - m, m + a); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(m, s - m - a); ctx.lineTo(m, s - m); ctx.lineTo(m + a, s - m); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(s - m - a, s - m); ctx.lineTo(s - m, s - m); ctx.lineTo(s - m, s - m - a); ctx.stroke()
                break
            }
            case "fullscreen-exit": {   // brackets inset, elbow toward centre
                const g = s * 0.34
                ctx.beginPath(); ctx.moveTo(g, m); ctx.lineTo(g, g); ctx.lineTo(m, g); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(s - g, m); ctx.lineTo(s - g, g); ctx.lineTo(s - m, g); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(g, s - m); ctx.lineTo(g, s - g); ctx.lineTo(m, s - g); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(s - g, s - m); ctx.lineTo(s - g, s - g); ctx.lineTo(s - m, s - g); ctx.stroke()
                break
            }
            case "stop": {              // a filled rounded square
                const p = s * 0.28, w = s - 2 * p, r = s * 0.08
                ctx.beginPath()
                ctx.moveTo(p + r, p)
                ctx.arcTo(p + w, p, p + w, p + w, r)
                ctx.arcTo(p + w, p + w, p, p + w, r)
                ctx.arcTo(p, p + w, p, p, r)
                ctx.arcTo(p, p, p + w, p, r)
                ctx.closePath(); ctx.fill()
                break
            }
            case "tune": {              // three sliders with knobs
                const rows = [0.3, 0.5, 0.7]
                const knobs = [0.66, 0.36, 0.58]
                const kr = s * 0.1
                for (let i = 0; i < 3; ++i) {
                    const y = s * rows[i]
                    ctx.beginPath(); ctx.moveTo(m, y); ctx.lineTo(s - m, y); ctx.stroke()
                    ctx.beginPath(); ctx.arc(s * knobs[i], y, kr, 0, 2 * Math.PI)
                    ctx.fillStyle = root.color; ctx.fill()
                    ctx.beginPath(); ctx.arc(s * knobs[i], y, kr, 0, 2 * Math.PI); ctx.stroke()
                }
                break
            }
            case "mouse": {             // a mouse body with a scroll wheel
                const x = s * 0.3, y = s * 0.13, w = s * 0.4, h = s * 0.74, r = w / 2
                ctx.beginPath()
                ctx.moveTo(x, y + r)
                ctx.arcTo(x, y, x + r, y, r)
                ctx.arcTo(x + w, y, x + w, y + r, r)
                ctx.lineTo(x + w, y + h - r)
                ctx.arcTo(x + w, y + h, x + w - r, y + h, r)
                ctx.arcTo(x, y + h, x, y + h - r, r)
                ctx.closePath(); ctx.stroke()
                ctx.beginPath(); ctx.moveTo(s * 0.5, y + s * 0.1); ctx.lineTo(s * 0.5, y + s * 0.26); ctx.stroke()
                break
            }
            case "keyboard": {          // a key panel with dots and a spacebar
                const x = s * 0.1, y = s * 0.27, w = s * 0.8, h = s * 0.46, r = s * 0.08
                ctx.beginPath()
                ctx.moveTo(x + r, y)
                ctx.arcTo(x + w, y, x + w, y + h, r)
                ctx.arcTo(x + w, y + h, x, y + h, r)
                ctx.arcTo(x, y + h, x, y, r)
                ctx.arcTo(x, y, x + w, y, r)
                ctx.closePath(); ctx.stroke()
                const dots = [0.25, 0.42, 0.59, 0.75]
                for (let i = 0; i < dots.length; ++i) {
                    ctx.beginPath(); ctx.arc(s * dots[i], s * 0.42, s * 0.025, 0, 2 * Math.PI); ctx.fill()
                }
                ctx.beginPath(); ctx.moveTo(s * 0.34, s * 0.6); ctx.lineTo(s * 0.66, s * 0.6); ctx.stroke()
                break
            }
            }
        }
    }

    onNameChanged: canvas.requestPaint()
    onColorChanged: canvas.requestPaint()
    onSizeChanged: canvas.requestPaint()
}
