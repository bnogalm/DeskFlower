import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Arrangement editor: every device is a "pack" of its monitors (kept in their real relative
// positions). Dragging a pack moves it in the shared plane; edges snap to other packs.
Item {
    id: page

    required property Backend backend
    property bool active: false

    readonly property var devices: backend.devices
    readonly property int margin: 24
    // View transform (layout units -> pixels). Recomputed when the model or size changes,
    // never during a drag, so the scene does not move under the cursor.
    property real viewScale: 0.05
    property real originX: 0
    property real originY: 0
    property bool overlapping: false

    onActiveChanged: if (active) backend.refreshThumbnails()
    onDevicesChanged: relayout()

    function localBounds(dev) {
        let l = Infinity, t = Infinity, r = -Infinity, b = -Infinity
        for (const s of dev.screens) {
            l = Math.min(l, s.x); t = Math.min(t, s.y)
            r = Math.max(r, s.x + s.w); b = Math.max(b, s.y + s.h)
        }
        return { x: l, y: t, w: r - l, h: b - t }
    }

    function globalRect(dev, x, y) {
        const b = localBounds(dev)
        return { l: x + b.x, t: y + b.y, r: x + b.x + b.w, b: y + b.y + b.h }
    }

    function relayout() {
        if (devices.length === 0 || canvas.width <= 0 || canvas.height <= 0)
            return
        let l = Infinity, t = Infinity, r = -Infinity, b = -Infinity
        let overlap = false
        for (let i = 0; i < devices.length; ++i) {
            const g = globalRect(devices[i], devices[i].x, devices[i].y)
            l = Math.min(l, g.l); t = Math.min(t, g.t); r = Math.max(r, g.r); b = Math.max(b, g.b)
            for (let j = 0; j < i; ++j) {
                const o = globalRect(devices[j], devices[j].x, devices[j].y)
                if (g.l < o.r && o.l < g.r && g.t < o.b && o.t < g.b)
                    overlap = true
            }
        }
        overlapping = overlap
        // Leave room around the packs so they can be dragged anywhere.
        const w = (r - l) * 1.3, h = (b - t) * 1.3
        const availW = canvas.width - 2 * margin, availH = canvas.height - 2 * margin
        viewScale = Math.min(availW / w, availH / h)
        originX = (l + r) / 2 - availW / viewScale / 2
        originY = (t + b) / 2 - availH / viewScale / 2
    }

    // Snaps a pack offset so its edges line up with the edges of other packs.
    function snap(dev, x, y) {
        const me = globalRect(dev, x, y)
        const threshold = 24 / viewScale
        let bestDx = null, bestDy = null
        for (const other of devices) {
            if (other.id === dev.id)
                continue
            const o = globalRect(other, other.x, other.y)
            for (const dx of [o.r - me.l, o.l - me.r, o.l - me.l, o.r - me.r]) {
                if (Math.abs(dx) < threshold && (bestDx === null || Math.abs(dx) < Math.abs(bestDx)))
                    bestDx = dx
            }
            for (const dy of [o.b - me.t, o.t - me.b, o.t - me.t, o.b - me.b]) {
                if (Math.abs(dy) < threshold && (bestDy === null || Math.abs(dy) < Math.abs(bestDy)))
                    bestDy = dy
            }
        }
        return { x: x + (bestDx ?? 0), y: y + (bestDy ?? 0) }
    }

    function commit(movedId, x, y) {
        backend.setLayout(devices.map(d => ({
            id: d.id,
            x: d.id === movedId ? Math.round(x) : d.x,
            y: d.id === movedId ? Math.round(y) : d.y
        })))
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 12
            Layout.rightMargin: 12
            Layout.topMargin: 8
            spacing: 12

            Switch {
                id: shareSwitch
                text: qsTr("Share this keyboard and mouse")
                checked: page.backend.sharing
                enabled: page.backend.serviceConnected
                onToggled: {
                    page.backend.setSharing(checked)
                    checked = Qt.binding(() => page.backend.sharing)
                }
            }
            Switch {
                id: clipboardSwitch
                text: qsTr("Share clipboard")
                checked: page.backend.clipboardSharing
                enabled: page.backend.serviceConnected
                onToggled: {
                    page.backend.setClipboardSharing(checked)
                    checked = Qt.binding(() => page.backend.clipboardSharing)
                }
            }
            Item { Layout.fillWidth: true }
            Button {
                text: qsTr("Refresh previews")
                flat: true
                enabled: page.backend.serviceConnected
                onClicked: page.backend.refreshThumbnails()
            }
        }

        Label {
            Layout.fillWidth: true
            Layout.leftMargin: 20
            Layout.rightMargin: 12
            elide: Text.ElideRight
            opacity: 0.7
            text: page.backend.sharing
                  ? qsTr("Paired devices follow this keyboard and mouse.")
                  : page.backend.masterName.length > 0
                    ? qsTr("Keyboard and mouse shared by %1.").arg(page.backend.masterName)
                    : qsTr("No device is sharing its keyboard and mouse.")
        }

        Pane {
            visible: page.backend.serviceConnected && !page.backend.sessionRunning
            Layout.fillWidth: true
            Layout.leftMargin: 12
            Layout.rightMargin: 12
            Material.background: Material.color(Material.Orange, Material.Shade100)
            Label {
                anchors.fill: parent
                wrapMode: Text.WordWrap
                color: "#5d3a00"
                text: qsTr("The session helper is not running: this device cannot report its screens or share its keyboard and mouse.")
            }
        }

        Item {
            id: canvas
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            onWidthChanged: page.relayout()
            onHeightChanged: page.relayout()

            Label {
                anchors.centerIn: parent
                visible: page.devices.length === 0
                opacity: 0.6
                text: qsTr("Waiting for screen information…")
            }

            Repeater {
                model: page.devices

                delegate: Item {
                    id: pack

                    required property var modelData
                    readonly property var bounds: page.localBounds(modelData)
                    readonly property bool isMaster: modelData.id === page.backend.masterId
                    readonly property bool hasCursor: page.backend.sharing && modelData.id === page.backend.activeId

                    x: page.margin + (modelData.x + bounds.x - page.originX) * page.viewScale
                    y: page.margin + (modelData.y + bounds.y - page.originY) * page.viewScale
                    width: bounds.w * page.viewScale
                    height: bounds.h * page.viewScale
                    z: dragArea.drag.active ? 10 : 1
                    opacity: modelData.connected ? 1 : 0.45

                    Label {
                        anchors.bottom: parent.top
                        anchors.bottomMargin: 4
                        anchors.left: parent.left
                        font.bold: true
                        text: pack.modelData.name
                              + (pack.modelData.self ? qsTr(" (this device)") : "")
                              + (pack.modelData.connected ? "" : qsTr(" · offline"))
                              + (pack.hasCursor ? qsTr(" · cursor here") : "")
                    }
                    CheckBox {
                        id: remoteViewBox
                        visible: !pack.modelData.self
                        // Below the "shares keyboard and mouse" line when there is one: side by
                        // side they overlap on narrow screens.
                        anchors.top: masterLabel.visible ? masterLabel.bottom : parent.bottom
                        anchors.left: parent.left
                        anchors.leftMargin: -8 // the indicator's own padding
                        text: qsTr("Remote view")
                        enabled: page.backend.remoteViewEnabled
                        checked: pack.modelData.remoteView === true
                        onToggled: {
                            page.backend.setRemoteView(pack.modelData.id, checked)
                            checked = Qt.binding(() => pack.modelData.remoteView === true)
                        }
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        ToolTip.text: page.backend.remoteViewEnabled
                                      ? qsTr("Crossing into this device shows its screens full screen on your monitor, one at a time, instead of moving the cursor to them.")
                                      : qsTr("Remote view is off on this device (Advanced settings).")
                    }
                    Label {
                        id: masterLabel
                        visible: pack.isMaster
                        anchors.top: parent.bottom
                        anchors.topMargin: 4
                        anchors.left: parent.left
                        color: Material.accent
                        font.bold: true
                        text: qsTr("● shares keyboard and mouse")
                    }

                    Repeater {
                        model: pack.modelData.screens

                        delegate: Rectangle {
                            id: screenRect

                            required property var modelData

                            x: (modelData.x - pack.bounds.x) * page.viewScale
                            y: (modelData.y - pack.bounds.y) * page.viewScale
                            width: modelData.w * page.viewScale
                            height: modelData.h * page.viewScale
                            color: Material.color(Material.BlueGrey, Material.Shade700)
                            border.width: 2
                            border.color: pack.modelData.remoteView === true ? Material.color(Material.Purple, Material.Shade200)
                                        : pack.isMaster ? Material.accent
                                                        : (pack.modelData.self ? Material.color(Material.BlueGrey, Material.Shade200)
                                                                               : Material.color(Material.Grey))
                            clip: true

                            Image {
                                anchors.fill: parent
                                anchors.margins: 2
                                source: "image://thumbs/" + screenRect.modelData.thumbKey + "?r=" + page.backend.thumbRevision
                                fillMode: Image.PreserveAspectCrop
                                cache: false
                                asynchronous: true
                                visible: pack.modelData.thumbsDenied !== true
                            }

                            // Geometry is shared with every paired device, pixels only with a view grant.
                            Label {
                                anchors.centerIn: parent
                                width: parent.width - 12
                                visible: pack.modelData.thumbsDenied === true && screenRect.height > 60
                                horizontalAlignment: Text.AlignHCenter
                                wrapMode: Text.WordWrap
                                color: "#ccffffff"
                                font.pixelSize: 11
                                text: qsTr("No permission to preview this screen")
                            }

                            Rectangle {
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                anchors.margins: 2
                                height: caption.implicitHeight + 6
                                visible: screenRect.height > 40
                                color: "#99000000"

                                Column {
                                    id: caption
                                    anchors.centerIn: parent
                                    width: parent.width - 8

                                    Label {
                                        width: parent.width
                                        horizontalAlignment: Text.AlignHCenter
                                        elide: Text.ElideRight
                                        color: "white"
                                        font.pixelSize: 11
                                        text: (screenRect.modelData.p ? "★ " : "") + screenRect.modelData.n
                                    }
                                    Label {
                                        width: parent.width
                                        horizontalAlignment: Text.AlignHCenter
                                        color: "#ccffffff"
                                        font.pixelSize: 10
                                        text: screenRect.modelData.w + " × " + screenRect.modelData.h
                                    }
                                }
                            }
                        }
                    }

                    MouseArea {
                        id: dragArea
                        anchors.fill: parent
                        cursorShape: drag.active ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                        drag.target: pack
                        onReleased: {
                            const x = (pack.x - page.margin) / page.viewScale + page.originX - pack.bounds.x
                            const y = (pack.y - page.margin) / page.viewScale + page.originY - pack.bounds.y
                            if (Math.abs(x - pack.modelData.x) < 1 && Math.abs(y - pack.modelData.y) < 1)
                                return
                            const snapped = page.snap(pack.modelData, x, y)
                            page.commit(pack.modelData.id, snapped.x, snapped.y)
                        }
                    }
                }
            }
        }

        Label {
            Layout.fillWidth: true
            Layout.margins: 12
            wrapMode: Text.WordWrap
            opacity: 0.7
            color: page.overlapping ? Material.color(Material.Orange) : Material.foreground
            text: page.overlapping
                  ? qsTr("Some devices overlap. Move them so that their screens only touch at the edges.")
                  : qsTr("Drag each device so its screens match how they are placed on your desk. The mouse moves to a neighbouring device across touching edges.")
        }
    }
}
