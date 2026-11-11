pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Material
import QtQuick.Layouts

// Paired and nearby devices, pairing and manual discovery.
ScrollView {
    id: page

    required property Backend backend

    contentWidth: availableWidth

    // Not an item, so the ScrollView keeps the column below as its content; shown centred in
    // the window.
    PairModeDialog {
        id: pairMode
        parent: Overlay.overlay
        anchors.centerIn: parent
        onChosen: (peerId, trust) => page.backend.pair(peerId, trust)
    }

    // Forget is final as far as the group goes: said before, so that nobody expects the other
    // devices to bring it back.
    Dialog {
        id: confirmForget
        property string peerId
        property string peerName
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: qsTr("Forget %1?").arg(peerName)
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAccepted: page.backend.unpair(peerId)
        Label {
            width: 380
            wrapMode: Text.WordWrap
            text: qsTr("It is removed from this device and told to forget this one. Your other devices will not add it back through the group: to use it here again, pair it with a PIN, or allow it again under Forgotten devices.")
        }
    }

    ColumnLayout {
        width: parent.width
        spacing: 8

        Label {
            text: qsTr("Paired devices")
            font.bold: true
            Layout.leftMargin: 16
            Layout.topMargin: 12
        }
        // The emptiness checks below count the rows of each Repeater (one per entry of its
        // list): the lengths of these lists do not compile to C++.
        Label {
            visible: trustedRows.count === 0
            text: qsTr("No paired devices yet.")
            opacity: 0.6
            Layout.leftMargin: 16
        }
        Repeater {
            id: trustedRows
            model: page.backend.trustedPeers
            delegate: PeerRow {
                Layout.fillWidth: true
                actionText: qsTr("Forget")
                showViewSwitch: true
                showTrustChooser: true
                viewSwitchEnabled: page.backend.remoteViewEnabled
                showUpdate: true
                updatesConfigured: page.backend.updatesConfigured
                updatesSupported: page.backend.updatesSupported
                updateBusy: page.backend.updateState !== ""
                updateState: page.backend.updatePeerId === peer.id ? page.backend.updateState : ""
                updateProgress: page.backend.updateProgress
                onActionClicked: {
                    confirmForget.peerId = peer.id
                    confirmForget.peerName = peer.name || qsTr("(unnamed)")
                    confirmForget.open()
                }
                onViewAllowedToggled: on => page.backend.setViewAllowed(peer.id, on)
                onSwapCtrlCmdToggled: on => page.backend.setSwapCtrlCmd(peer.id, on)
                onTrustPicked: trust => page.backend.setTrust(peer.id, trust)
                onUpdateClicked: page.backend.updateFrom(peer.id)
                onUpdateCancelClicked: page.backend.cancelUpdate()
            }
        }

        // Forgotten devices: group members do not introduce them again until allowed here.
        Label {
            visible: forgottenRows.count > 0
            text: qsTr("Forgotten devices")
            font.bold: true
            Layout.leftMargin: 16
            Layout.topMargin: 16
        }
        Label {
            visible: forgottenRows.count > 0
            text: qsTr("Your other devices cannot add these back through the group. Pairing with a PIN always works.")
            opacity: 0.6
            font.pixelSize: 12
            wrapMode: Text.WordWrap
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Layout.fillWidth: true
        }
        Repeater {
            id: forgottenRows
            model: page.backend.forgottenPeers
            delegate: RowLayout {
                id: forgottenRow
                required property peerInfo modelData
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                spacing: 12
                Label {
                    text: forgottenRow.modelData.name || forgottenRow.modelData.shortId
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
                Button {
                    text: qsTr("Allow again")
                    flat: true
                    enabled: page.backend.serviceConnected
                    onClicked: page.backend.unforget(forgottenRow.modelData.id)
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    ToolTip.text: qsTr("Your devices in the group may add it again (it is not paired by this)")
                }
            }
        }

        Label {
            text: qsTr("Nearby devices")
            font.bold: true
            Layout.leftMargin: 16
            Layout.topMargin: 16
        }
        Label {
            visible: nearbyRows.count === 0
            text: page.backend.exploring
                  ? qsTr("Looking for devices on the local network…")
                  : qsTr("Turn on discovery to look for devices.")
            opacity: 0.6
            Layout.leftMargin: 16
        }
        Repeater {
            id: nearbyRows
            model: page.backend.nearbyPeers
            delegate: PeerRow {
                Layout.fillWidth: true
                actionText: qsTr("Pair")
                actionEnabled: peer.exploring
                hint: peer.exploring ? "" : qsTr("Not in discovery mode")
                // The level is chosen first (guest, this device only, all my devices).
                onActionClicked: pairMode.show(peer.id, peer.name, peer.trust)
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
