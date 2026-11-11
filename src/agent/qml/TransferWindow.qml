pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Material
import QtQuick.Layouts

// DeskFlower's small progress window, in a corner of the screen and above the others, without
// taking the focus: the pasted files' transfers that no file manager shows itself (on Windows a
// streaming paste has Explorer's own dialog; the deferred paste, and every paste on Linux and
// macOS, have only this), with progress, rate, time left and Cancel, then how each one ended for a
// few seconds. Also the copied files a paste of which was refused for want of a paste gesture,
// with Receive to take them anyway. main.cpp places it at the bottom right of the primary screen.
Window {
    id: root

    required property Backend backend

    readonly property bool waitingShown: backend.waitingCount > 0 && !backend.receiving
    readonly property bool hasContent: rows.count > 0 || waitingShown
    // Shown once there has been something to show for a moment: the transfer of a paste Explorer
    // shows itself is recognised a little after its first report, and must not flash here.
    property bool shown: false

    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint | Qt.WindowDoesNotAcceptFocus
    transientParent: null // shown also while the main window is hidden in the tray
    title: qsTr("DeskFlower transfers")
    width: 340
    height: frame.implicitHeight
    visible: shown

    Material.theme: Material.System
    Material.accent: Material.Teal
    Material.primary: Material.BlueGrey

    onHasContentChanged: {
        if (hasContent) {
            showDelay.restart()
        } else {
            showDelay.stop()
            shown = false
        }
    }

    Timer {
        id: showDelay
        interval: 600
        onTriggered: root.shown = root.hasContent
    }

    Pane {
        id: frame
        anchors.fill: parent
        padding: 8
        background: Rectangle {
            color: frame.Material.backgroundColor
            border.color: "#90a4ae" // Material BlueGrey, Shade300
            border.width: 1
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 6

            // The copied files a paste of which was refused: Receive takes them with no paste.
            ColumnLayout {
                visible: root.waitingShown
                Layout.fillWidth: true
                spacing: 2

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        text: qsTr("%n item(s) copied on %1", "", root.backend.waitingCount).arg(root.backend.waitingFrom)
                    }
                    Button {
                        text: qsTr("Receive")
                        highlighted: true
                        topInset: 0
                        bottomInset: 0
                        implicitHeight: 32
                        onClicked: root.backend.receiveClipboard()
                    }
                    Button {
                        text: "✕"
                        flat: true
                        topInset: 0
                        bottomInset: 0
                        implicitHeight: 32
                        Layout.preferredWidth: implicitHeight
                        onClicked: root.backend.dismissWaiting()
                    }
                }
                Label {
                    visible: text !== ""
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    font.pixelSize: 12
                    color: "#c62828" // Material Red, Shade800
                    text: root.backend.waitingError
                }
            }

            Repeater {
                id: rows
                model: root.backend.windowTransfers
                delegate: TransferRow {
                    backend: root.backend
                    detailed: true
                    Layout.fillWidth: true
                }
            }
        }
    }
}
