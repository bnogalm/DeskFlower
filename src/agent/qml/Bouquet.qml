pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Shapes

// A small animated bouquet with one flower per desktop: this one and every connected paired device.
// Everything is derived from the device identifiers, so paired devices show the same bouquet
// without exchanging anything: the 32 places come from a fixed seed, and each device's flower
// (kind, colour, size, place) from a hash of its id. Purely decorative.
Item {
    id: root

    property var deviceIds: []       // hex ids: this device and the available ones
    property bool running: true      // off while the window is hidden

    readonly property int maxFlowers: 32

    implicitWidth: 76
    implicitHeight: 56

    // Kinds of flower: petal count and shape, heart colour.
    readonly property var kinds: [
        { petals: 5, width: 0.46, length: 0.5, diamond: false, heart: "#ffd54f" },   // round
        { petals: 9, width: 0.2, length: 0.54, diamond: false, heart: "#ffca28" },   // daisy
        { petals: 6, width: 0.3, length: 0.42, diamond: true, heart: "#fff59d" },    // star
        { petals: 12, width: 0.17, length: 0.5, diamond: false, heart: "#8d6e63" }   // sunflower
    ]

    // FNV-1a over the id, then a small PRNG (mulberry32) seeded with it.
    function hash(text) {
        let h = 0x811c9dc5
        for (let i = 0; i < text.length; ++i) {
            h ^= text.charCodeAt(i)
            h = Math.imul(h, 0x01000193) >>> 0
        }
        return h >>> 0
    }
    function rng(seed) {
        let a = seed >>> 0
        return function() {
            a = (a + 0x6D2B79F5) >>> 0
            let t = a
            t = Math.imul(t ^ (t >>> 15), t | 1)
            t ^= t + Math.imul(t ^ (t >>> 7), t | 61)
            return ((t ^ (t >>> 14)) >>> 0) / 4294967296
        }
    }

    // The 32 places, identical everywhere: place 0 is the front centre, the others fan out. Each new
    // place is the candidate farthest from those before it, so the first ones are well spread.
    readonly property var places: {
        const r = rng(0xDF10)
        const out = [{ angle: 0, reach: 0.6 }]
        while (out.length < maxFlowers) {
            let best = null
            let bestGap = -1
            for (let c = 0; c < 12; ++c) {
                const cand = { angle: (r() * 2 - 1) * 62, reach: 0.42 + r() * 0.5 }
                let gap = Infinity
                for (const p of out)
                    gap = Math.min(gap, headDistance(cand, p))
                if (gap > bestGap) {
                    best = cand
                    bestGap = gap
                }
            }
            out.push(best)
        }
        return out
    }

    // Where a place puts its flower, at the default size (the drawing scales with the item).
    function headAt(p) {
        const reach = p.reach * (implicitHeight - 8)
        const rad = p.angle * Math.PI / 180
        return { x: Math.sin(rad) * reach * 0.9, y: -Math.cos(rad) * reach }
    }
    function headDistance(p, q) {
        const a = headAt(p)
        const b = headAt(q)
        return Math.hypot(a.x - b.x, a.y - b.y)
    }
    readonly property real minGap: 16   // between flower centres, in pixels

    // One flower per device, in a place derived from its id (collisions resolved in id order).
    // Kind, colour and size depend on the id only, so a device keeps its flower everywhere.
    readonly property var flowers: {
        const ids = deviceIds.slice(0, maxFlowers).sort()
        const taken = []
        const out = []
        for (const id of ids) {
            const r = rng(hash(id))
            // The first id (the same on every device) takes the front centre.
            let place = out.length === 0 ? 0 : Math.floor(r() * maxFlowers)
            r() // keeps the following draws independent of the place rule
            // From that place on, the first free one at least minGap away from the others; when
            // the bouquet is too full for that, the free one with the most room.
            let chosen = -1
            let roomiest = -1
            let mostRoom = -1
            for (let k = 0; k < maxFlowers && chosen < 0; ++k) {
                const cand = (place + k) % maxFlowers
                if (taken.indexOf(cand) >= 0)
                    continue
                let room = Infinity
                for (const t of taken)
                    room = Math.min(room, headDistance(places[cand], places[t]))
                if (room >= minGap)
                    chosen = cand
                else if (room > mostRoom) {
                    roomiest = cand
                    mostRoom = room
                }
            }
            place = chosen >= 0 ? chosen : roomiest
            taken.push(place)
            out.push({ place: place,
                       kind: Math.floor(r() * kinds.length),
                       color: Qt.hsla(r(), 0.55 + r() * 0.3, 0.55 + r() * 0.13, 1),
                       size: (place === 0 ? 20 : 14) + r() * 5 })
        }
        return out
    }

    Item {
        id: bunch
        anchors.fill: parent
        transformOrigin: Item.Bottom

        // The ribbon where the stems meet.
        readonly property point base: Qt.point(width / 2, height - 6)

        SequentialAnimation on rotation {
            running: root.running
            loops: Animation.Infinite
            NumberAnimation { from: -3; to: 3; duration: 2600; easing.type: Easing.InOutSine }
            NumberAnimation { from: 3; to: -3; duration: 2600; easing.type: Easing.InOutSine }
        }

        Repeater {
            model: root.maxFlowers

            Item {
                id: layer
                required property int index
                readonly property var place: root.places[index]
                readonly property var spec: {
                    for (const f of root.flowers)
                        if (f.place === index)
                            return f
                    return null
                }
                readonly property bool on: spec !== null
                readonly property var kind: root.kinds[on ? spec.kind : 0]
                readonly property real rad: place.angle * Math.PI / 180
                readonly property real reach: place.reach * (bunch.height - 8)
                readonly property point head: Qt.point(bunch.base.x + Math.sin(rad) * reach * 0.9,
                                                       bunch.base.y - Math.cos(rad) * reach)

                anchors.fill: parent
                // Farther flowers behind, the centre one in front.
                z: index === 0 ? 100 : 50 - place.reach * 40
                opacity: on ? 1 : 0
                visible: opacity > 0
                Behavior on opacity { NumberAnimation { duration: 700 } }

                // Stem from the ribbon to the flower: it leaves the ribbon upwards and bends out
                // towards the flower; the centre one leans a little to one side.
                Shape {
                    anchors.fill: parent
                    preferredRendererType: Shape.CurveRenderer

                    ShapePath {
                        strokeColor: "#5f8f4e"
                        strokeWidth: 1.4
                        fillColor: "transparent"
                        capStyle: ShapePath.RoundCap
                        startX: bunch.base.x
                        startY: bunch.base.y

                        PathQuad {
                            readonly property real dx: layer.head.x - bunch.base.x
                            readonly property real lean: layer.index === 0 ? 3 : 0
                            x: layer.head.x
                            y: layer.head.y
                            controlX: bunch.base.x + dx * 0.12 + lean
                            controlY: bunch.base.y + (layer.head.y - bunch.base.y) * 0.7
                        }
                    }
                }

                // The flower: petals around a heart, opening when it appears.
                Item {
                    id: flower
                    width: layer.on ? layer.spec.size : 16
                    height: width
                    x: layer.head.x - width / 2
                    y: layer.head.y - height / 2
                    scale: layer.on ? 1 : 0.2
                    Behavior on scale { NumberAnimation { duration: 700; easing.type: Easing.OutBack } }

                    readonly property color petal: layer.on ? layer.spec.color : "transparent"

                    // A slow turn of its own.
                    RotationAnimation on rotation {
                        running: root.running && layer.on
                        loops: Animation.Infinite
                        from: 0
                        to: (layer.index % 2 ? -360 : 360)
                        duration: 24000 + layer.index * 700
                    }

                    Repeater {
                        model: layer.kind.petals
                        // Each petal hangs from the centre, turned into place.
                        Item {
                            required property int index
                            width: flower.width * layer.kind.width
                            height: flower.height * layer.kind.length
                            x: (flower.width - width) / 2
                            y: flower.height / 2 - height
                            transformOrigin: Item.Bottom
                            rotation: index * 360 / layer.kind.petals

                            Rectangle {
                                // Rounded petal, or a diamond for the star.
                                anchors.centerIn: parent
                                width: layer.kind.diamond ? parent.width * 0.75 : parent.width
                                height: layer.kind.diamond ? width : parent.height
                                radius: layer.kind.diamond ? 1 : width / 2
                                rotation: layer.kind.diamond ? 45 : 0
                                scale: layer.kind.diamond ? Math.SQRT2 * parent.height / parent.width / 1.6 : 1
                                color: flower.petal
                                border.width: 0.6
                                border.color: Qt.darker(flower.petal, 1.35)
                                antialiasing: true
                            }
                        }
                    }
                    Rectangle {
                        width: flower.width * 0.32
                        height: width
                        radius: width / 2
                        anchors.centerIn: parent
                        color: layer.kind.heart
                        border.width: 0.6
                        border.color: Qt.darker(layer.kind.heart, 1.4)
                        antialiasing: true
                    }
                }
            }
        }

        // Ribbon.
        Rectangle {
            z: 200
            width: 12
            height: 5
            radius: 2
            x: bunch.base.x - width / 2
            y: bunch.base.y - 2
            color: "#e57373"
        }
    }
}
