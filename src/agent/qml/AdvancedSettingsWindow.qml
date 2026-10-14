import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Advanced settings, in a window of their own: the network ports. The service saves them and uses
// them from its next start, so saving offers to restart it.
ApplicationWindow {
    id: root

    required property Backend backend

    readonly property var ports: backend.ports || ({})
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

    function load() {
        dataField.value = ports.data || defaultData
        discoveryField.value = ports.discovery || defaultDiscovery
        phoneField.value = ports.phone || defaultPhone
        error = ""
    }

    readonly property bool changed: dataField.value !== (ports.data || defaultData)
                                    || discoveryField.value !== (ports.discovery || defaultDiscovery)
                                    || phoneField.value !== (ports.phone || defaultPhone)
    readonly property bool pendingRestart: (ports.data && ports.dataInUse && ports.data !== ports.dataInUse)
                                           || (ports.discovery && ports.discoveryInUse
                                               && ports.discovery !== ports.discoveryInUse)
                                           || (ports.phone && ports.phoneInUse && ports.phone !== ports.phoneInUse)

    onVisibleChanged: if (visible) load()

    Connections {
        target: root.backend
        function onServiceError(message) {
            if (root.visible)
                root.error = message
        }
    }

    component PortField: SpinBox {
        from: 1024
        to: 65535
        editable: true
        textFromValue: (value, locale) => String(value) // no thousands separator
        valueFromText: (text, locale) => parseInt(text, 10)
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
                               + "In use: UDP %1, TCP %2.").arg(root.ports.dataInUse || "–").arg(root.ports.tcpInUse || "–")
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
                          .arg(root.ports.discoveryInUse || "–")
                }

                Label { text: qsTr("Phone access (HTTPS)"); Layout.topMargin: 8 }
                PortField { id: phoneField; Layout.topMargin: 8 }
                Label {
                    Layout.columnSpan: 2
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: 12
                    opacity: 0.7
                    text: root.ports.phoneInUse ? qsTr("The address in the QR code. In use: %1.").arg(root.ports.phoneInUse)
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
                Material.background: Material.color(Material.Red, Material.Shade100)
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
                Material.background: Material.color(Material.Orange, Material.Shade100)
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
