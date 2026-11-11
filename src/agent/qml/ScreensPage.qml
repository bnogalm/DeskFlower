pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Material
import QtQuick.Layouts

// Arrangement editor: every device is a "pack" of its monitors (kept in their real relative
// positions). Dragging a pack moves it in the shared plane; edges snap to other packs.
Item {
    id: page

    required property Backend backend
    property bool active: false

    // Where the packs are in the shared plane: left, top, right and bottom, four numbers per
    // device of backend.devices (same order).
    readonly property list<real> packEdges: backend.packEdges
    readonly property int margin: 24
    // View transform (layout units -> pixels). Recomputed when the model or size changes,
    // never during a drag, so the scene does not move under the cursor.
    property real viewScale: 0.05
    property real originX: 0
    property real originY: 0
    property bool overlapping: false
    // Material.accent as a colour (Material.accent itself is a QVariant), so that the colour
    // bindings below compile.
    readonly property color accentColor: Material.accentColor
    // Material palette colours, spelled out so that the bindings using them compile to C++
    // (Material.color() does not).
    readonly property color remoteViewColor: "#ce93d8" // Material.color(Material.Purple, Material.Shade200)
    readonly property color orangeColor: "#ff9800"     // Material.color(Material.Orange)
    readonly property color greyColor: "#9e9e9e"       // Material.color(Material.Grey)
    readonly property color screenColor: "#455a64"     // Material.color(Material.BlueGrey, Material.Shade700)
    readonly property color selfBorderColor: "#b0bec5" // Material.color(Material.BlueGrey, Material.Shade200)

    onActiveChanged: if (active) backend.refreshThumbnails()
    onPackEdgesChanged: relayout()

    function relayout(): void {
        const e = packEdges
        if (e.length === 0 || canvas.width <= 0 || canvas.height <= 0)
            return
        let l = Infinity, t = Infinity, r = -Infinity, b = -Infinity
        let overlap = false
        for (let i = 0; i < e.length; i += 4) {
            l = Math.min(l, e[i]); t = Math.min(t, e[i + 1]); r = Math.max(r, e[i + 2]); b = Math.max(b, e[i + 3])
            for (let j = 0; j < i; j += 4) {
                if (e[i] < e[j + 2] && e[j] < e[i + 2] && e[i + 1] < e[j + 3] && e[j + 1] < e[i + 3])
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

    // A move d replaces best when it is under the threshold and shorter (of equal ones, the first
    // stays). best starts as Infinity: no move yet.
    function closer(best: real, d: real, threshold: real): real {
        return Math.abs(d) < threshold && Math.abs(d) < Math.abs(best) ? d : best
    }

    // Snaps the offset of pack `index` (its device's bounds at (x, y)) so its edges line up with
    // the edges of other packs.
    function snap(index: int, bounds: rect, x: real, y: real): point {
        const left = x + bounds.x, top = y + bounds.y
        const right = left + bounds.width, bottom = top + bounds.height
        const threshold = 24 / viewScale
        let bestDx = Infinity, bestDy = Infinity
        for (let i = 0; i < packEdges.length; i += 4) {
            if (i === 4 * index)
                continue
            const l = packEdges[i], t = packEdges[i + 1], r = packEdges[i + 2], b = packEdges[i + 3]
            bestDx = closer(bestDx, r - left, threshold)
            bestDx = closer(bestDx, l - right, threshold)
            bestDx = closer(bestDx, l - left, threshold)
            bestDx = closer(bestDx, r - right, threshold)
            bestDy = closer(bestDy, b - top, threshold)
            bestDy = closer(bestDy, t - bottom, threshold)
            bestDy = closer(bestDy, t - top, threshold)
            bestDy = closer(bestDy, b - bottom, threshold)
        }
        return Qt.point(x + (bestDx === Infinity ? 0 : bestDx), y + (bestDy === Infinity ? 0 : bestDy))
    }

    // Name of a local monitor (the self device's screen with that id), empty when it is gone.
    function localScreenName(id: string): string {
        return backend.selfDevice.screen(id).n
    }

    ViewPlanDialog {
        id: planDialog
        parent: Overlay.overlay
        // Centred when it opens; the user may then drag it by its title.
        x: Math.round((parent.width - width) / 2)
        y: Math.round((parent.height - height) / 2)
        backend: page.backend
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
                // toggle() puts the switch back without removing its binding (a click does not
                // either): it changes when the service reports the new state.
                onToggled: {
                    page.backend.setSharing(checked)
                    toggle()
                }
            }
            Switch {
                id: clipboardSwitch
                text: qsTr("Share clipboard")
                checked: page.backend.clipboardSharing
                enabled: page.backend.serviceConnected
                onToggled: {
                    page.backend.setClipboardSharing(checked)
                    toggle()
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
            Material.background: "#ffe0b2" // Material.color(Material.Orange, Material.Shade100)
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
                visible: page.packEdges.length === 0
                opacity: 0.6
                text: qsTr("Waiting for screen information…")
            }

            Repeater {
                model: page.backend.devices

                delegate: Item {
                    id: pack

                    required property int index
                    required property deviceInfo modelData
                    readonly property rect bounds: modelData.bounds
                    readonly property bool isMaster: modelData.id === page.backend.masterId
                    readonly property bool hasCursor: page.backend.sharing && modelData.id === page.backend.activeId

                    x: page.margin + (modelData.x + bounds.x - page.originX) * page.viewScale
                    y: page.margin + (modelData.y + bounds.y - page.originY) * page.viewScale
                    width: bounds.width * page.viewScale
                    height: bounds.height * page.viewScale
                    // Lifted above the other packs while dragged or while its header is hovered.
                    z: dragArea.drag.active ? 10 : (header.hovered ? 9 : 1)

                    // Name, badges and the Remote view switch above the pack. Hovering it puts a
                    // backdrop behind it (readable where packs overlap) and lifts the pack.
                    Item {
                        id: header
                        readonly property bool hovered: headerHover.hovered
                        anchors.bottom: parent.top
                        anchors.bottomMargin: 6
                        anchors.left: parent.left
                        width: headerRow.implicitWidth
                        height: headerRow.implicitHeight

                        HoverHandler { id: headerHover }

                        Rectangle {
                            anchors.fill: parent
                            anchors.leftMargin: -8
                            anchors.rightMargin: -8
                            anchors.topMargin: -5
                            anchors.bottomMargin: -5
                            radius: 10
                            color: Material.dialogColor
                            border.width: 1
                            border.color: Material.dividerColor
                            opacity: header.hovered ? 1 : 0
                            Behavior on opacity { NumberAnimation { duration: 120 } }
                        }

                        Row {
                            id: headerRow
                            spacing: 8

                            Label {
                                id: nameLabel
                                anchors.verticalCenter: parent.verticalCenter
                                font.bold: true
                                opacity: pack.modelData.connected ? 1 : 0.6
                                text: pack.modelData.name
                                      + (pack.modelData.self ? qsTr(" (this device)") : "")
                                      + (pack.modelData.connected ? "" : qsTr(" · offline"))
                                      + (pack.hasCursor ? qsTr(" · cursor here") : "")
                            }
                            // Guests either way: the cursor does not go there, or its user does not
                            // drive this device, and layout edits do not travel.
                            Label {
                                id: guestBadge
                                readonly property bool guestHere: pack.modelData.trust === "guest"
                                readonly property bool guestThere: pack.modelData.guestThere
                                visible: guestHere || guestThere
                                anchors.verticalCenter: parent.verticalCenter
                                color: page.orangeColor
                                font.bold: true
                                text: "\u{1F512} " + (guestHere ? qsTr("Guest") : qsTr("You are its guest"))
                                HoverHandler { id: guestHover }
                                ToolTip.visible: guestHover.hovered
                                ToolTip.delay: 300
                                ToolTip.text: guestHere
                                              ? qsTr("A guest here: it cannot drive this computer or view its screen, and gets this clipboard only when you paste on it.")
                                              : qsTr("This device treats yours as a guest: it does not take your keyboard and mouse, and ignores your changes to the arrangement.")
                            }
                            // Remote view as a pill of constant size: outlined when off, filled
                            // with the remote view colour (the screens' border) when on.
                            AbstractButton {
                                id: remoteViewBox
                                readonly property color tint: page.remoteViewColor
                                visible: !pack.modelData.self
                                anchors.verticalCenter: parent.verticalCenter
                                checkable: true
                                hoverEnabled: true
                                enabled: page.backend.remoteViewEnabled
                                checked: pack.modelData.remoteView
                                opacity: enabled ? 1 : 0.5
                                leftPadding: 8
                                rightPadding: 10
                                topPadding: 3
                                bottomPadding: 3
                                // toggle() puts it back without removing its binding (a click
                                // does not either): it changes when the service reports it.
                                onToggled: {
                                    page.backend.setRemoteView(pack.modelData.id, checked)
                                    toggle()
                                }
                                contentItem: Row {
                                    spacing: 6
                                    Rectangle {
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: 8
                                        height: 8
                                        radius: 4
                                        color: remoteViewBox.checked ? remoteViewBox.tint : "transparent"
                                        border.width: 1.5
                                        border.color: remoteViewBox.checked ? remoteViewBox.tint : Material.hintTextColor
                                    }
                                    Label {
                                        anchors.verticalCenter: parent.verticalCenter
                                        font.pixelSize: 12
                                        color: remoteViewBox.checked ? remoteViewBox.tint : Material.secondaryTextColor
                                        text: qsTr("Remote view")
                                    }
                                }
                                background: Rectangle {
                                    radius: height / 2
                                    // Material.primaryTextColor: Material.foreground as a colour
                                    // (no foreground is set on this page).
                                    color: remoteViewBox.checked
                                           ? Qt.alpha(remoteViewBox.tint, remoteViewBox.hovered ? 0.30 : 0.18)
                                           : (remoteViewBox.hovered ? Qt.alpha(Material.primaryTextColor, 0.08) : "transparent")
                                    border.width: 1
                                    border.color: remoteViewBox.checked ? remoteViewBox.tint : Material.dividerColor
                                }
                                ToolTip.visible: hovered
                                ToolTip.delay: 500
                                ToolTip.text: qsTr("Crossing into this device shows its screens on your monitors instead of moving the cursor to them. Use Monitors... to choose which monitor shows each screen.")
                            }
                            // Whenever Remote view is on for this device.
                            AbstractButton {
                                id: monitorsButton
                                visible: remoteViewBox.visible && remoteViewBox.checked
                                         && page.backend.remoteViewEnabled
                                anchors.verticalCenter: parent.verticalCenter
                                hoverEnabled: true
                                leftPadding: 4
                                rightPadding: 4
                                topPadding: 3
                                bottomPadding: 3
                                onClicked: planDialog.openFor(pack.modelData.id)
                                contentItem: Label {
                                    font.pixelSize: 12
                                    font.underline: monitorsButton.hovered
                                    color: page.accentColor
                                    text: qsTr("Monitors...")
                                }
                                background: Item {}
                                ToolTip.visible: hovered
                                ToolTip.delay: 500
                                ToolTip.text: qsTr("Choose which of your monitors shows each screen of this device.")
                            }
                        }
                    }
                    // Why the switch is greyed out (a disabled control gets no hover of its own).
                    ToolTip {
                        parent: remoteViewBox
                        visible: !page.backend.remoteViewEnabled && remoteViewBox.visible && header.hovered
                        delay: 500
                        text: qsTr("Remote view is off on this device (Advanced settings).")
                    }

                    Column {
                        id: footer
                        // Under the pack, unless the canvas clips it there: then above the header.
                        readonly property bool fits: pack.y + pack.height + 4 + height <= canvas.height
                        y: fits ? pack.height + 4 : -(header.height + 12 + height)
                        anchors.left: parent.left
                        spacing: 0

                        Label {
                            id: masterLabel
                            visible: pack.isMaster
                            color: page.accentColor
                            font.bold: true
                            text: qsTr("● shares keyboard and mouse")
                        }
                    }

                    Repeater {
                        model: pack.modelData.screens

                        delegate: Rectangle {
                            id: screenRect

                            required property deviceScreen modelData
                            // Where this screen goes under the device's view plan: "-" not shown,
                            // a local screen id, or "" for the monitor where the edge is crossed
                            // (also when the chosen monitor is gone).
                            readonly property string placement: {
                                if (pack.modelData.self || !pack.modelData.remoteView)
                                    return ""
                                const v = pack.modelData.placement(modelData.i)
                                return v === "-" || page.localScreenName(v) !== "" ? v : ""
                            }
                            readonly property bool hidden: placement === "-"

                            x: (modelData.x - pack.bounds.x) * page.viewScale
                            y: (modelData.y - pack.bounds.y) * page.viewScale
                            width: modelData.w * page.viewScale
                            height: modelData.h * page.viewScale
                            opacity: (pack.modelData.connected ? 1 : 0.45) * (hidden ? 0.5 : 1)
                            color: page.screenColor
                            border.width: 2
                            border.color: pack.modelData.remoteView ? page.remoteViewColor
                                        : pack.isMaster ? page.accentColor
                                                        : (pack.modelData.self ? page.selfBorderColor : page.greyColor)
                            clip: true

                            Image {
                                anchors.fill: parent
                                anchors.margins: 2
                                source: "image://thumbs/" + screenRect.modelData.thumbKey + "?r=" + page.backend.thumbRevision
                                fillMode: Image.PreserveAspectCrop
                                cache: false
                                asynchronous: true
                                visible: !pack.modelData.thumbsDenied
                            }

                            // Geometry is shared with every paired device, pixels only with a view grant.
                            Label {
                                anchors.centerIn: parent
                                width: parent.width - 12
                                visible: pack.modelData.thumbsDenied && screenRect.height > 60
                                horizontalAlignment: Text.AlignHCenter
                                wrapMode: Text.WordWrap
                                color: "#ccffffff"
                                font.pixelSize: 11
                                text: qsTr("No permission to preview this screen")
                            }

                            // View plan badge: the local monitor this screen is shown on, or hidden.
                            Rectangle {
                                anchors.left: parent.left
                                anchors.top: parent.top
                                anchors.margins: 4
                                width: badge.implicitWidth + 10
                                height: badge.implicitHeight + 4
                                radius: 3
                                visible: screenRect.placement !== "" && screenRect.height > 40
                                color: "#b3000000"

                                Label {
                                    id: badge
                                    anchors.centerIn: parent
                                    color: "white"
                                    font.pixelSize: 10
                                    text: screenRect.hidden ? qsTr("hidden")
                                                            : "→ " + page.localScreenName(screenRect.placement)
                                }
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
                            const snapped = page.snap(pack.index, pack.bounds, x, y)
                            page.backend.moveDevice(pack.modelData.id, snapped.x, snapped.y)
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
            color: page.overlapping ? page.orangeColor : Material.primaryTextColor // Material.foreground, as above
            text: page.overlapping
                  ? qsTr("Some devices overlap. Move them so that their screens only touch at the edges.")
                  : qsTr("Drag each device so its screens match how they are placed on your desk. The mouse moves to a neighbouring device across touching edges.")
        }
    }
}
