pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Material
import QtQuick.Layouts
import QtQuick.Window

// Where this device shows the screens of a viewed device: for each of its screens, the local
// monitor it goes on when the cursor enters that device, or none. Edits a draft; Apply sends
// it to the service as the device's view plan (remote screen id -> local screen id or "-").
Dialog {
    id: root

    property Backend backend
    property deviceInfo device
    // The plan being edited, one entry per screen of `device` (in its order): the local screen id,
    // "-" (not shown) or "" (the monitor where the edge is crossed). Reassigned on every change so
    // bindings re-evaluate.
    property list<string> draft: []
    // The local monitor a monitor list is pointing at (its highlighted entry while the list is
    // open), and the remote screen that row is for: that monitor says "Show here".
    property string previewLocal: ""
    property string previewRemote: ""

    readonly property string hidden: "-"
    // This device's screen ids, which the monitor lists offer.
    readonly property list<string> localIds: backend ? backend.selfDevice.screenIds() : []
    // The texts of the monitor lists' entries: the monitor where the edge is crossed, each local
    // screen (in localIds' order), not shown. (A list built in a loop: this binding stays
    // interpreted.)
    readonly property list<string> choiceTexts: {
        const list = [qsTr("Monitor where I cross")]
        if (backend) {
            const screens = backend.selfDevice.screens
            for (let i = 0; i < screens.length; ++i) {
                const s = screens[i]
                list.push((s.p ? "★ " : "") + s.n + " (" + s.w + " × " + s.h + ")")
            }
        }
        list.push(qsTr("Don't show"))
        return list
    }

    function isLocal(id: string): bool {
        return localIds.indexOf(id) >= 0
    }

    function openFor(deviceId: string): void {
        device = backend.device(deviceId)
        // A local monitor that is gone counts as the default, as the service treats it.
        draft = device.editablePlan(localIds)
        open()
    }

    // The plan's entry for screen `index` of the device.
    function planned(index: int): string {
        return index >= 0 && index < draft.length ? draft[index] : ""
    }
    // Whether screen `index` may be hidden: some other screen stays visible.
    function canHide(index: int): bool {
        for (let i = 0; i < draft.length; ++i)
            if (i !== index && draft[i] !== hidden)
                return true
        return false
    }
    // What entry `choice` of a monitor list sets: "" (where the edge is crossed), a local screen
    // id, or hidden; and back, the entry showing screen `index`'s plan.
    function choiceValue(choice: int): string {
        if (choice >= 1 && choice <= localIds.length)
            return localIds[choice - 1]
        return choice === localIds.length + 1 ? hidden : ""
    }
    function choiceIndex(index: int): int {
        const v = planned(index)
        if (v === "")
            return 0
        const i = localIds.indexOf(v)
        if (i >= 0)
            return i + 1
        return v === hidden ? localIds.length + 1 : 0
    }
    function choose(index: int, value: string): void {
        if (value === hidden && !canHide(index))
            return
        const plan = draft
        plan[index] = value
        draft = plan
    }

    // The local screen id of a Qt screen (LocalScreens::screenId: the screen's name, or
    // name@x,y when two monitors share it).
    function qtScreenId(screen: ScreenInfo): string {
        const screens = Application.screens
        let same = 0
        for (let i = 0; i < screens.length; ++i)
            if (screens[i].name === screen.name)
                ++same
        return same <= 1 ? screen.name : screen.name + "@" + screen.virtualX + "," + screen.virtualY
    }
    function localName(id: string): string {
        if (!isLocal(id))
            return ""
        const s = backend.selfDevice.screen(id)
        return (s.p ? "★ " : "") + s.n
    }

    title: qsTr("Where to show the screens of %1").arg(device.name)
    modal: true
    dim: false // the arrangement behind stays visible (the dialog can be dragged aside)
    onClosed: {
        previewLocal = ""
        previewRemote = ""
    }

    // The title is the handle that moves the dialog (kept inside the window).
    header: Label {
        text: root.title
        elide: Text.ElideRight
        font.pixelSize: 20
        font.bold: true
        padding: 24
        bottomPadding: 0

        HoverHandler { cursorShape: Qt.SizeAllCursor }
        DragHandler {
            property real startX
            property real startY
            target: null
            onActiveChanged: {
                if (active) {
                    startX = root.x
                    startY = root.y
                }
            }
            onTranslationChanged: {
                if (!active || !root.parent)
                    return
                root.x = Math.max(0, Math.min(root.parent.width - root.width, startX + translation.x))
                root.y = Math.max(0, Math.min(root.parent.height - root.height, startY + translation.y))
            }
        }
    }

    // On every local monitor while the dialog is open: its name, big, at the top left, so the
    // names in the lists can be told apart; the one a list points at also says "Show here".
    Instantiator {
        active: root.opened
        model: Application.screens

        delegate: Window {
            id: marker
            required property ScreenInfo modelData
            readonly property string localId: root.qtScreenId(modelData)
            readonly property bool target: root.previewLocal !== "" && root.previewLocal === localId

            screen: modelData
            x: modelData.virtualX + 32
            y: modelData.virtualY + 32
            width: card.width
            height: card.height
            color: "transparent"
            flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
                   | Qt.WindowTransparentForInput | Qt.WindowDoesNotAcceptFocus
            visible: root.localName(localId) !== ""

            Rectangle {
                id: card
                width: lines.implicitWidth + 48
                height: lines.implicitHeight + 32
                radius: 14
                color: marker.target ? "#e66a1b9a" : "#cc000000"
                border.width: marker.target ? 3 : 0
                border.color: "white"

                Column {
                    id: lines
                    anchors.centerIn: parent
                    spacing: 4

                    Label {
                        visible: marker.target
                        color: "white"
                        font.pixelSize: 44
                        font.bold: true
                        text: qsTr("Show here")
                    }
                    Label {
                        visible: marker.target && root.previewRemote !== ""
                        color: "white"
                        font.pixelSize: 24
                        text: root.previewRemote
                    }
                    Label {
                        color: "white"
                        opacity: marker.target ? 0.8 : 1
                        font.pixelSize: marker.target ? 20 : 36
                        font.bold: !marker.target
                        text: root.localName(marker.localId)
                    }
                }
            }
        }
    }
    standardButtons: Dialog.Apply | Dialog.Cancel
    // Never taller than the window: with many screens the rows scroll and the buttons stay put.
    height: Math.min(implicitHeight, parent ? parent.height - 32 : implicitHeight)
    onApplied: {
        backend.setViewPlan(device.id, device.screenIds(), draft) // only the screens with a choice go out
        close()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 12

        Label {
            text: qsTr("Screens on different monitors are shown at the same time. Screens sharing a monitor are shown one at a time: push the cursor past the edge to switch.")
            opacity: 0.7
            wrapMode: Text.WordWrap
            font.pixelSize: 12
            Layout.preferredWidth: 420
        }
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredHeight: rows.implicitHeight
            clip: true
            contentWidth: availableWidth

            GridLayout {
                id: rows
                width: parent.width
                columns: 2
                columnSpacing: 16
                rowSpacing: 8

                Repeater {
                    model: root.device.screens

                    delegate: Label {
                        required property int index
                        required property deviceScreen modelData
                        Layout.row: index
                        Layout.column: 0
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        text: (modelData.p ? "★ " : "") + modelData.n + "  " + modelData.w + " × " + modelData.h
                    }
                }
                Repeater {
                    model: root.device.screens

                    delegate: ComboBox {
                        id: monitorBox
                        required property int index
                        required property deviceScreen modelData
                        Layout.row: index
                        Layout.column: 1
                        model: root.choiceTexts
                        implicitContentWidthPolicy: ComboBox.WidestText
                        // Bumped after each pick, which does not remove the binding: currentIndex
                        // reads it (always >= 0), so it is evaluated again and shows the draft
                        // (also when the pick was refused).
                        property int picks: 0
                        currentIndex: picks >= 0 ? root.choiceIndex(index) : 0
                        onActivated: {
                            root.choose(index, root.choiceValue(currentIndex))
                            ++picks
                        }
                        // The monitor this row points at: the highlighted entry while the list
                        // is open, the chosen one while hovered.
                        readonly property string pointedAt: {
                            if (popup.visible && highlightedIndex >= 0)
                                return root.choiceValue(highlightedIndex)
                            return hovered ? root.choiceValue(currentIndex) : ""
                        }
                        hoverEnabled: true
                        onPointedAtChanged: {
                            if (pointedAt !== "" && pointedAt !== root.hidden) {
                                root.previewLocal = pointedAt
                                root.previewRemote = (modelData.p ? "★ " : "") + modelData.n
                            } else if (root.previewRemote === (modelData.p ? "★ " : "") + modelData.n) {
                                root.previewLocal = ""
                                root.previewRemote = ""
                            }
                        }
                        delegate: ItemDelegate {
                            required property int index
                            required property string modelData
                            width: ListView.view ? ListView.view.width : implicitWidth
                            text: modelData
                            highlighted: monitorBox.highlightedIndex === index
                            // Never hide every screen: the last visible one cannot be hidden.
                            enabled: root.choiceValue(index) !== root.hidden || root.canHide(monitorBox.index)
                        }
                    }
                }
            }
        }
    }
}
