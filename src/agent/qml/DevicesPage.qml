import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Paired and nearby devices, pairing and manual discovery.
ScrollView {
    id: page

    required property Backend backend

    contentWidth: availableWidth

    ColumnLayout {
        width: parent.width
        spacing: 8

        Label {
            text: qsTr("Paired devices")
            font.bold: true
            Layout.leftMargin: 16
            Layout.topMargin: 12
        }
        Label {
            visible: page.backend.trustedPeers.length === 0
            text: qsTr("No paired devices yet.")
            opacity: 0.6
            Layout.leftMargin: 16
        }
        Repeater {
            model: page.backend.trustedPeers
            delegate: PeerRow {
                required property var modelData
                peer: modelData
                Layout.fillWidth: true
                actionText: qsTr("Forget")
                showViewSwitch: true
                viewSwitchEnabled: page.backend.remoteViewEnabled
                showUpdate: true
                updatesConfigured: page.backend.updatesConfigured
                updatesSupported: page.backend.updatesSupported
                updateBusy: page.backend.updateState !== ""
                updateState: page.backend.updatePeerId === peer.id ? page.backend.updateState : ""
                updateProgress: page.backend.updateProgress
                onActionClicked: page.backend.unpair(peer.id)
                onViewAllowedToggled: on => page.backend.setViewAllowed(peer.id, on)
                onUpdateClicked: page.backend.updateFrom(peer.id)
            }
        }

        Label {
            text: qsTr("Nearby devices")
            font.bold: true
            Layout.leftMargin: 16
            Layout.topMargin: 16
        }
        Label {
            visible: page.backend.nearbyPeers.length === 0
            text: page.backend.exploring
                  ? qsTr("Looking for devices on the local network…")
                  : qsTr("Turn on discovery to look for devices.")
            opacity: 0.6
            Layout.leftMargin: 16
        }
        Repeater {
            model: page.backend.nearbyPeers
            delegate: PeerRow {
                required property var modelData
                peer: modelData
                Layout.fillWidth: true
                actionText: qsTr("Pair")
                actionEnabled: peer.exploring
                hint: peer.exploring ? "" : qsTr("Not in discovery mode")
                onActionClicked: page.backend.pair(peer.id)
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 16
            Layout.topMargin: 24
            spacing: 8

            TextField {
                id: hostField
                placeholderText: qsTr("Add by IP (e.g. 192.168.1.20)")
                Layout.fillWidth: true
                onAccepted: addButton.clicked()
            }
            Button {
                id: addButton
                text: qsTr("Find")
                enabled: hostField.text.length > 0 && page.backend.serviceConnected
                onClicked: {
                    const parts = hostField.text.split(":")
                    page.backend.addPeer(parts[0], parts.length > 1 ? parseInt(parts[1]) : 0)
                    hostField.clear()
                }
            }
        }
    }
}
