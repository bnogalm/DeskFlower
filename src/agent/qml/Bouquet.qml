pragma ComponentBehavior: Bound
import QtQuick
import QtQuick.Shapes

// A small animated bouquet with one flower per desktop: this one and every connected paired device.
// Everything is derived from the device identifiers, so paired devices show the same bouquet
// without exchanging anything: the 32 places come from a fixed seed, and each device's flower
// (kind, colour, size, place) from a hash of its id. Purely decorative.
Item {
    id: root

    property list<string> deviceIds: []  // hex ids: this device and the available ones
    property bool running: true      // off while the window is hidden

    readonly property int maxFlowers: 32

    implicitWidth: 76
    implicitHeight: 56

    // Kinds of flower: petal count and shape, heart colour. Parallel typed lists, one entry per
    // kind, in this order: round, daisy, star, sunflower. (The hearts are strings: a list<color>
    // literal does not compile to C++, reading a string into a color property does.)
    readonly property list<int> kindPetals: [5, 9, 6, 12]
    readonly property list<real> kindWidths: [0.46, 0.2, 0.3, 0.17]
    readonly property list<real> kindLengths: [0.5, 0.54, 0.42, 0.5]
    readonly property list<bool> kindDiamonds: [false, false, true, false]
    readonly property list<string> kindHearts: ["#ffd54f", "#ffca28", "#fff59d", "#8d6e63"]

    // FNV-1a over the id, then a small PRNG (mulberry32) seeded with it. (hash stays interpreted:
    // charCodeAt does not compile to C++.)
    function hash(text: string): real {
        let h = 0x811c9dc5
        for (let i = 0; i < text.length; ++i) {
            h ^= text.charCodeAt(i)
            h = Math.imul(h, 0x01000193) >>> 0
        }
        return h >>> 0
    }
    // Draw n (from 0) of mulberry32 seeded with seed. Its state after n + 1 draws is
    // seed + (n + 1) * 0x6D2B79F5 (mod 2^32), so each draw is a pure function of the seed and its
    // number: callers count their draws instead of holding a closure (which does not compile).
    function random(seed: real, n: int): real {
        let t = ((seed >>> 0) + (n + 1) * 0x6D2B79F5) >>> 0
        t = Math.imul(t ^ (t >>> 15), t | 1)
        t ^= t + Math.imul(t ^ (t >>> 7), t | 61)
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296
    }

    // The 32 places, identical everywhere: place 0 is the front centre, the others fan out. Each new
    // place is the candidate farthest from those before it, so the first ones are well spread.
    // Pairs in one typed list: place i's angle (degrees from upright) is places[2 * i], its reach
    // (fraction of the height) places[2 * i + 1]. This binding and flowers stay interpreted (a list
    // built in a loop does not compile to C++); the functions they call and their readers do.
    readonly property list<real> places: {
        const seed = 0xDF10
        let n = 0
        const out = [0, 0.6]
        while (out.length < 2 * maxFlowers) {
            let bestAngle = 0
            let bestReach = 0
            let bestGap = -1
            for (let c = 0; c < 12; ++c) {
                const angle = (random(seed, n++) * 2 - 1) * 62
                const reach = 0.42 + random(seed, n++) * 0.5
                let gap = Infinity
                for (let i = 0; i < out.length; i += 2)
                    gap = Math.min(gap, headDistance(angle, reach, out[i], out[i + 1]))
                if (gap > bestGap) {
                    bestAngle = angle
                    bestReach = reach
                    bestGap = gap
                }
            }
            out.push(bestAngle, bestReach)
        }
        return out
    }

    // Distance between the flowers of two places (angle, reach), at the default size (the drawing
    // scales with the item).
    function headDistance(angleA: real, reachA: real, angleB: real, reachB: real): real {
        const ra = reachA * (implicitHeight - 8)
        const radA = angleA * Math.PI / 180
        const rb = reachB * (implicitHeight - 8)
        const radB = angleB * Math.PI / 180
        return hypot(Math.sin(radA) * ra * 0.9 - Math.sin(radB) * rb * 0.9,
                     -Math.cos(radA) * ra - -Math.cos(radB) * rb)
    }
    // Math.hypot(x, y) as the JavaScript engine computes it (Qt's scaled sum: the larger magnitude
    // times the root of 1 plus the squared ratio), for the finite values used here. Math.hypot
    // itself does not compile to C++, and std::hypot may round differently: written out, the
    // places and the bouquet stay exactly the same.
    function hypot(x: real, y: real): real {
        const ax = Math.abs(x)
        const ay = Math.abs(y)
        if (!(ay > 0))
            return ax > 0 ? ax : 0
        if (!(ax > 0))
            return ay
        if (ay > ax) {
            const r = ax / ay
            return ay * Math.sqrt(r * r + 1)
        }
        const r = ay / ax
        return ax * Math.sqrt(1 + r * r)
    }
    readonly property real minGap: 16   // between flower centres, in pixels

    // One flower per device, in a place derived from its id (collisions resolved in id order).
    // Kind, colour and size depend on the id only, so a device keeps its flower everywhere.
    // Per place, flowerFields values in one typed list: kind (-1 for an empty place), hue,
    // saturation, lightness, size.
    readonly property int flowerFields: 5
    readonly property list<real> flowers: {
        const ids = deviceIds.slice(0, maxFlowers).sort()
        const out = []
        for (let i = 0; i < maxFlowers; ++i)
            out.push(-1, 0, 0, 0, 0)
        const taken = []
        for (let i = 0; i < ids.length; ++i) {
            const seed = hash(ids[i])
            let n = 0
            // The first id (the same on every device) takes the front centre.
            let place = taken.length === 0 ? 0 : Math.floor(random(seed, n++) * maxFlowers)
            ++n // a draw skipped: keeps the following draws independent of the place rule
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
                for (let j = 0; j < taken.length; ++j) {
                    const t = taken[j]
                    room = Math.min(room, headDistance(places[2 * cand], places[2 * cand + 1],
                                                       places[2 * t], places[2 * t + 1]))
                }
                if (room >= minGap)
                    chosen = cand
                else if (room > mostRoom) {
                    roomiest = cand
                    mostRoom = room
                }
            }
            place = chosen >= 0 ? chosen : roomiest
            taken.push(place)
            const at = place * flowerFields
            out[at] = Math.floor(random(seed, n++) * kindPetals.length)
            out[at + 1] = random(seed, n++)
            out[at + 2] = 0.55 + random(seed, n++) * 0.3
            out[at + 3] = 0.55 + random(seed, n++) * 0.13
            out[at + 4] = (place === 0 ? 20 : 14) + random(seed, n++) * 5
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
                // What the drawing reads from the place, the flower and its kind, typed.
                readonly property real angle: root.places[2 * index]
                readonly property real placeReach: root.places[2 * index + 1]
                readonly property int at: index * root.flowerFields
                readonly property bool on: root.flowers[at] >= 0
                readonly property int kind: on ? root.flowers[at] : 0
                readonly property int petals: root.kindPetals[kind]
                readonly property real petalWidth: root.kindWidths[kind]
                readonly property real petalLength: root.kindLengths[kind]
                readonly property bool diamond: root.kindDiamonds[kind]
                readonly property color heart: root.kindHearts[kind]
                readonly property real size: on ? root.flowers[at + 4] : 16
                // Interpreted: Qt.hsla does not compile to C++.
                readonly property color petalColor: on ? Qt.hsla(root.flowers[at + 1],
                                                                  root.flowers[at + 2],
                                                                  root.flowers[at + 3], 1)
                                                       : "transparent"
                readonly property real rad: angle * Math.PI / 180
                readonly property real reach: placeReach * (bunch.height - 8)
                readonly property point head: Qt.point(bunch.base.x + Math.sin(rad) * reach * 0.9,
                                                       bunch.base.y - Math.cos(rad) * reach)

                anchors.fill: parent
                // Farther flowers behind, the centre one in front.
                z: index === 0 ? 100 : 50 - placeReach * 40
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
                    width: layer.size
                    height: width
                    x: layer.head.x - width / 2
                    y: layer.head.y - height / 2
                    scale: layer.on ? 1 : 0.2
                    Behavior on scale { NumberAnimation { duration: 700; easing.type: Easing.OutBack } }

                    readonly property color petal: layer.petalColor

                    // A slow twist one way and back (a full turn looks like a wheel); neighbours
                    // twist the other way and at their own pace.
                    readonly property real twist: (layer.index % 2 ? -1 : 1) * (14 + layer.index % 3 * 4)
                    SequentialAnimation on rotation {
                        running: root.running && layer.on
                        loops: Animation.Infinite
                        NumberAnimation {
                            from: -flower.twist; to: flower.twist
                            duration: 5200 + layer.index * 450; easing.type: Easing.InOutSine
                        }
                        NumberAnimation {
                            from: flower.twist; to: -flower.twist
                            duration: 5200 + layer.index * 450; easing.type: Easing.InOutSine
                        }
                    }

                    Repeater {
                        model: layer.petals
                        // Each petal hangs from the centre, turned into place.
                        Item {
                            required property int index
                            width: flower.width * layer.petalWidth
                            height: flower.height * layer.petalLength
                            x: (flower.width - width) / 2
                            y: flower.height / 2 - height
                            transformOrigin: Item.Bottom
                            rotation: index * 360 / layer.petals

                            Rectangle {
                                // Rounded petal, or a diamond for the star.
                                anchors.centerIn: parent
                                width: layer.diamond ? parent.width * 0.75 : parent.width
                                height: layer.diamond ? width : parent.height
                                radius: layer.diamond ? 1 : width / 2
                                rotation: layer.diamond ? 45 : 0
                                scale: layer.diamond ? Math.SQRT2 * parent.height / parent.width / 1.6 : 1
                                color: flower.petal
                                border.width: 0.6
                                border.color: flower.petal.darker(1.35)
                                antialiasing: true
                            }
                        }
                    }
                    Rectangle {
                        width: flower.width * 0.32
                        height: width
                        radius: width / 2
                        anchors.centerIn: parent
                        color: layer.heart
                        border.width: 0.6
                        border.color: layer.heart.darker(1.4)
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
