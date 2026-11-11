pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Material
import QtQuick.Layouts

// Advanced settings, in a window of their own: remote view at all (applied at once) and the network
// ports. The service saves the ports and uses them from its next start, so saving offers to
// restart it.
ApplicationWindow {
    id: root

    required property Backend backend

    readonly property portsInfo ports: backend.ports
    readonly property int defaultData: 24801
    readonly property int defaultDiscovery: 24800
    readonly property int defaultPhone: 24802
    property string error

    title: qsTr("Advanced settings")
    width: 520
    height: 660
    minimumWidth: 420
    minimumHeight: 460
    visible: false

    Material.theme: Material.System
    Material.accent: Material.Teal
    Material.primary: Material.BlueGrey

    function load(): void {
        dataField.value = ports.data || defaultData
        discoveryField.value = ports.discovery || defaultDiscovery
        phoneField.value = ports.phone || defaultPhone
        error = ""
    }

    readonly property bool changed: dataField.value !== (ports.data || defaultData)
                                    || discoveryField.value !== (ports.discovery || defaultDiscovery)
                                    || phoneField.value !== (ports.phone || defaultPhone)
    // Saved ports that the running service does not use yet (0: not known).
    readonly property bool pendingRestart: (ports.data !== 0 && ports.dataInUse !== 0 && ports.data !== ports.dataInUse)
                                           || (ports.discovery !== 0 && ports.discoveryInUse !== 0
                                               && ports.discovery !== ports.discoveryInUse)
                                           || (ports.phone !== 0 && ports.phoneInUse !== 0 && ports.phone !== ports.phoneInUse)

    onVisibleChanged: if (visible) load()

    // A port in use as shown, "–" when not known.
    function inUse(port: int): string { return port !== 0 ? "" + port : "–" }

    Connections {
        target: root.backend
        function onServiceError(message: string): void {
            if (root.visible)
                root.error = message
        }
    }

    component PortField: SpinBox {
        from: 1024
        to: 65535
        editable: true
        textFromValue: function(value: int): string { return String(value) } // no thousands separator
        valueFromText: function(text: string): int { return parseInt(text, 10) }
        Layout.preferredWidth: 160
    }

    footer: Pane {
        padding: 8
        RowLayout {
            anchors.fill: parent
            spacing: 8
            Item { Layout.fillWidth: true }
            Button {
                text: qsTr("Close")
                flat: true
                onClicked: root.close()
            }
            Button {
                text: qsTr("Save")
                enabled: root.backend.serviceConnected && root.changed
                onClicked: {
                    root.error = ""
                    root.backend.setPorts(dataField.value, discoveryField.value, phoneField.value)
                }
            }
            Button {
                text: qsTr("Restart service")
                highlighted: true
                enabled: root.backend.serviceConnected && root.pendingRestart && !root.changed
                onClicked: root.backend.restartService()
            }
        }
    }

    ScrollView {
        anchors.fill: parent
        contentWidth: availableWidth

        ColumnLayout {
            width: parent.width
            spacing: 12

            Label {
                text: qsTr("Remote view")
                font.pixelSize: 18
                font.bold: true
                Layout.topMargin: 16
                Layout.leftMargin: 16
            }
            Switch {
                text: qsTr("Allow remote view")
                enabled: root.backend.serviceConnected
                checked: root.backend.remoteViewEnabled
                onToggled: {
                    root.backend.setRemoteViewEnabled(checked)
                    // Back until the service reports it: toggle(), like the click, keeps the binding.
                    toggle()
                }
                Layout.leftMargin: 8
            }
            Label {
                text: qsTr("Off: no paired device can see this screen or its previews, even those allowed to view it "
                           + "without asking, and this device shows nobody else's screen (crossing into a device "
                           + "marked \"Remote view\" moves the cursor there as usual). The permissions given are kept "
                           + "for when it is turned back on.")
                wrapMode: Text.WordWrap
                opacity: 0.8
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
            }

            Label {
                text: qsTr("Pasting files")
                visible: root.backend.pasteMoveSupported
                font.pixelSize: 18
                font.bold: true
                Layout.topMargin: 16
                Layout.leftMargin: 16
            }
            Switch {
                text: qsTr("Large pastes: move the received files instead of copying them")
                visible: root.backend.pasteMoveSupported
                enabled: root.backend.serviceConnected
                checked: root.backend.pasteMove
                onToggled: {
                    root.backend.setPasteMove(checked)
                    toggle() // back until the service reports it (see above)
                }
                Layout.leftMargin: 8
            }
            Label {
                visible: root.backend.pasteMoveSupported
                text: qsTr("Files copied on another device are pasted in File Explorer as they arrive, with "
                           + "Explorer's own progress. For large pastes (2,000 items or more, or 1 GB or more) "
                           + "this setting instead receives them first, with DeskFlower's progress and Cancel, "
                           + "and then lets Explorer move them into place: much faster for many files, as nothing "
                           + "is written twice. Turn it off if Explorer misbehaves with it. Applies to what is "
                           + "copied from then on.")
                wrapMode: Text.WordWrap
                opacity: 0.8
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
            }

            Label {
                text: qsTr("Other networks")
                visible: root.backend.otherSubnetsSupported
                font.pixelSize: 18
                font.bold: true
                Layout.topMargin: 16
                Layout.leftMargin: 16
            }
            Switch {
                text: qsTr("Allow devices on other networks")
                visible: root.backend.otherSubnetsSupported
                enabled: root.backend.serviceConnected
                checked: root.backend.otherSubnets
                onToggled: {
                    root.backend.setOtherSubnets(checked)
                    toggle() // back until the service reports it (see above)
                }
                Layout.leftMargin: 8
            }
            Label {
                visible: root.backend.otherSubnetsSupported
                text: qsTr("On networks Windows calls public (many Wi-Fi networks), the firewall lets in only "
                           + "devices on the same subnet. On: also devices on other private networks (behind "
                           + "another router, a virtual machine host's network, a carrier's NAT). Paired devices "
                           + "still have to authenticate; others get nothing.")
                wrapMode: Text.WordWrap
                opacity: 0.8
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
            }

            // Keyboard diagnostics: only in builds with DESKFLOWER_KEY_DIAGNOSTICS.
            Label {
                visible: root.backend.keyDiagnosticsAvailable
                text: qsTr("Keyboard diagnostics")
                font.pixelSize: 18
                font.bold: true
                Layout.topMargin: 16
                Layout.leftMargin: 16
            }
            Label {
                visible: root.backend.keyDiagnosticsAvailable
                text: qsTr("For troubleshooting a key or shortcut that does nothing on another device (Win+D, for "
                           + "example): for 2 minutes, records each key that goes between this device and the "
                           + "others, from the keyboard that typed it to the device that should type it, here and "
                           + "on the connected devices. Start it, press the shortcut a few times on the device "
                           + "with the keyboard while the cursor is on the other one, then copy the result. Only "
                           + "keys sent to or received from another device are recorded, nothing typed for this "
                           + "device itself, and nothing once it stops. Do not type passwords or anything secret "
                           + "on the other device while it records: each key is kept in the log of both devices.")
                wrapMode: Text.WordWrap
                opacity: 0.8
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
            }
            RowLayout {
                visible: root.backend.keyDiagnosticsAvailable
                spacing: 8
                Layout.leftMargin: 8
                Layout.rightMargin: 16
                Button {
                    text: root.backend.keyDiagActive ? qsTr("Stop") : qsTr("Start keyboard diagnostics")
                    highlighted: !root.backend.keyDiagActive
                    enabled: root.backend.serviceConnected
                    onClicked: root.backend.setKeyDiagnostics(!root.backend.keyDiagActive)
                }
                Button {
                    text: qsTr("Copy")
                    flat: true
                    enabled: diagText.text !== ""
                    onClicked: {
                        diagText.selectAll()
                        diagText.copy()
                        diagText.deselect()
                    }
                }
                Label {
                    id: diagStatus
                    property double now: Date.now()
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: !root.backend.keyDiagActive ? ""
                          : (root.backend.keyDiagBy !== "" ? qsTr("Recording, asked by %1").arg(root.backend.keyDiagBy)
                                                           : qsTr("Recording"))
                            + qsTr(" (%1 s left)").arg(Math.max(0, Math.round((root.backend.keyDiagEndsAt - now) / 1000)))
                    color: "#f44336" // Material.color(Material.Red), spelled out so it compiles to C++
                    Timer {
                        interval: 1000
                        repeat: true
                        running: root.visible && root.backend.keyDiagActive
                        triggeredOnStart: true
                        onTriggered: diagStatus.now = Date.now()
                    }
                }
            }
            ScrollView {
                id: diagScroll
                visible: root.backend.keyDiagnosticsAvailable && diagText.text !== ""
                Layout.fillWidth: true
                Layout.preferredHeight: 200
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                TextArea {
                    id: diagText
                    readOnly: true
                    selectByMouse: true
                    wrapMode: TextEdit.NoWrap
                    font.family: "monospace"
                    font.pixelSize: 11
                    text: root.backend.keyDiagText
                    // Follows the newest line while recording.
                    onTextChanged: if (root.backend.keyDiagActive) cursorPosition = length
                }
            }

            Label {
                text: qsTr("Network ports")
                font.pixelSize: 18
                font.bold: true
                Layout.topMargin: 16
                Layout.leftMargin: 16
            }
            Label {
                text: qsTr("Change them only if another program uses the same ports or a firewall asks for "
                           + "specific ones. They take effect when the DeskFlower service restarts.")
                wrapMode: Text.WordWrap
                opacity: 0.8
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
            }

            GridLayout {
                columns: 2
                columnSpacing: 16
                rowSpacing: 4
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16

                Label { text: qsTr("Connections (UDP and TCP)") }
                PortField { id: dataField }
                Label {
                    Layout.columnSpan: 2
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: 12
                    opacity: 0.7
                    text: qsTr("Encrypted traffic with paired devices. Other devices learn it automatically. "
                               + "In use: UDP %1, TCP %2.").arg(root.inUse(root.ports.dataInUse)).arg(root.inUse(root.ports.tcpInUse))
                }

                Label { text: qsTr("Discovery (UDP)"); Layout.topMargin: 8 }
                PortField { id: discoveryField; Layout.topMargin: 8 }
                Label {
                    Layout.columnSpan: 2
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: 12
                    opacity: 0.7
                    text: qsTr("Devices find each other on this port: it must be the same on all of them. In use: %1.")
                          .arg(root.inUse(root.ports.discoveryInUse))
                }

                Label { text: qsTr("Phone access (HTTPS)"); Layout.topMargin: 8 }
                PortField { id: phoneField; Layout.topMargin: 8 }
                Label {
                    Layout.columnSpan: 2
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: 12
                    opacity: 0.7
                    text: root.ports.phoneInUse !== 0 ? qsTr("The address in the QR code. In use: %1.").arg(root.ports.phoneInUse)
                                                      : qsTr("The address in the QR code (phone access is off).")
                }
            }

            Button {
                text: qsTr("Restore defaults")
                flat: true
                Layout.leftMargin: 8
                onClicked: {
                    dataField.value = root.defaultData
                    discoveryField.value = root.defaultDiscovery
                    phoneField.value = root.defaultPhone
                }
            }

            Pane {
                visible: root.error !== ""
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Material.background: "#ffcdd2" // Material.color(Material.Red, Material.Shade100)
                Label {
                    anchors.fill: parent
                    wrapMode: Text.WordWrap
                    color: "#7f0000"
                    text: root.error
                }
            }
            Pane {
                visible: root.error === "" && root.pendingRestart && !root.changed
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                Material.background: "#ffe0b2" // Material.color(Material.Orange, Material.Shade100)
                Label {
                    anchors.fill: parent
                    wrapMode: Text.WordWrap
                    color: "#5d3a00"
                    text: qsTr("The service is not using these ports yet: restart it to apply them (connections with "
                               + "other devices and phones drop for a few seconds). A port that stays different "
                               + "after restarting is taken by another program.")
                }
            }
            Item { implicitHeight: 8 }
        }
    }
}
