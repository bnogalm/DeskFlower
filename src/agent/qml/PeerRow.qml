pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Material
import QtQuick.Layouts

ItemDelegate {
    id: root

    // The peer shown: the entry of the list this row is a delegate for (a Repeater's model).
    required property peerInfo modelData
    property alias peer: root.modelData
    property string actionText
    property bool actionEnabled: true
    property string hint
    property bool showViewSwitch: false  // paired peers: may view this screen without asking
    property bool viewSwitchEnabled: true
    property bool showTrustChooser: false // paired peers: guest, this device only or all my devices
    // Paired peers offering a newer signed package (peer.update). updateState: the update in
    // progress from this peer ("", downloading, verifying, installing).
    property bool showUpdate: false
    property bool updatesConfigured: false
    property bool updatesSupported: false
    property bool updateBusy: false  // an update is in progress, from any device
    property string updateState
    property real updateProgress

    signal actionClicked()
    signal viewAllowedToggled(bool on)
    signal swapCtrlCmdToggled(bool on)
    signal trustPicked(string trust) // "guest", "device" or "group" (confirmed for guest and group)
    signal updateClicked()
    signal updateCancelClicked()

    // PeerRow shows peer.trust, the level given here, and peer.effectiveTrust, the stricter of
    // both sides (levels and their texts: TrustLevels).
    function trustIndex(level: string): int { return TrustLevels.index(level) }
    // The non-empty parts, joined as one line of details.
    function joined(parts: list<string>): string {
        let text = ""
        for (let i = 0; i < parts.length; ++i) {
            if (parts[i].length > 0)
                text += (text.length > 0 ? "  ·  " : "") + parts[i]
        }
        return text
    }

    implicitHeight: Math.max(64, implicitContentHeight + topPadding + bottomPadding)

    // Guest and group change a lot with one click (a guest loses this keyboard and mouse and its
    // view permission; a group member is trusted by every member without a PIN): asked first.
    Dialog {
        id: confirmTrust
        property string level
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        // The level's index first: TrustLevels.texts is read after the call, which compiles.
        title: {
            const index = TrustLevels.index(confirmTrust.level)
            return qsTr("Make %1 \u201C%2\u201D?").arg(root.peer.name || qsTr("(unnamed)"))
                                             .arg(TrustLevels.texts[index])
        }
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAccepted: root.trustPicked(level)
        ColumnLayout {
            spacing: 12
            Label {
                text: TrustLevels.description(confirmTrust.level)
                wrapMode: Text.WordWrap
                Layout.preferredWidth: 360
            }
            Label {
                text: confirmTrust.level === "guest"
                      ? qsTr("It will no longer drive this computer or view its screen, and the permission to view it without asking is removed. It still gets this clipboard, but only when you paste on it.")
                      : qsTr("Once it chooses \u201CAll my devices\u201D for this device too, your other devices in the group trust it without a PIN, and it trusts them. Forgetting it later does not remove it from those devices.")
                opacity: 0.8
                wrapMode: Text.WordWrap
                Layout.preferredWidth: 360
            }
        }
    }

    contentItem: RowLayout {
        spacing: 12

        Rectangle {
            implicitWidth: 10
            implicitHeight: 10
            radius: 5
            // Material.color(Material.Green) and Material.color(Material.Grey), spelled out so they
            // compile to C++.
            color: root.peer.connected ? "#4caf50" : (root.peer.trusted ? "#9e9e9e" : "transparent")
            border.width: root.peer.trusted ? 0 : 1
            border.color: "#9e9e9e"
        }

        ColumnLayout {
            spacing: 2
            Layout.fillWidth: true

            Label {
                text: root.peer.name || qsTr("(unnamed)")
                font.bold: true
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
            Label {
                text: root.joined([root.peer.os,
                                   root.peer.version !== "" ? "v" + root.peer.version : "",
                                   // The endpoint in use; through a router that translates addresses
                                   // (NAT), the router's, which says why direct connections may need
                                   // another path.
                                   root.peer.address !== ""
                                   ? root.peer.address + ":" + root.peer.port
                                     + (root.peer.viaNat ? " " + qsTr("(via router, NAT)") : "") : "",
                                   root.peer.trusted ? (root.peer.connected ? qsTr("connected") : qsTr("disconnected"))
                                                     : root.hint,
                                   // Another protocol: transfers with it are off (Main.qml says why).
                                   root.peer.protocolMismatch !== ""
                                   ? qsTr("protocol %1: transfers unavailable").arg(root.peer.protocol) : ""])
                opacity: 0.7
                font.pixelSize: 12
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
            Label {
                text: root.peer.fingerprint
                font.family: "monospace"
                font.pixelSize: 10
                opacity: 0.5
            }
            RowLayout {
                // What the peer is here: a guest (cannot use this keyboard and mouse or view this
                // screen; this clipboard reaches it sealed and opens only when its user pastes),
                // one of the user's devices trusted by this one only (it pastes freely), or a
                // member of the group of all the user's devices.
                visible: root.showTrustChooser
                spacing: 8
                Layout.topMargin: 4

                Label {
                    text: qsTr("Trust")
                    font.pixelSize: 12
                }
                ComboBox {
                    id: trustBox
                    font.pixelSize: 12
                    textRole: "text"
                    valueRole: "value"
                    model: TrustLevels.levels
                    // Wide enough for the longest level, so no text is cut in the control or
                    // in the popup.
                    implicitContentWidthPolicy: ComboBox.WidestText
                    Layout.preferredWidth: Math.max(implicitWidth, 180)
                    // Bumped after each pick, which does not remove the binding: currentIndex reads it
                    // (always >= 0), so it is evaluated again and shows the level in force until the
                    // service reports the new one.
                    property int picks: 0
                    currentIndex: picks >= 0 ? root.trustIndex(root.peer.trust) : 0
                    onActivated: {
                        const level = TrustLevels.values[currentIndex] // the one picked (currentValue)
                        ++picks
                        if (level === root.peer.trust)
                            return
                        if (level === "guest" || level === "group") {
                            confirmTrust.level = level
                            confirmTrust.open()
                        } else {
                            root.trustPicked(level)
                        }
                    }
                    ToolTip.visible: hovered && !popup.visible
                    ToolTip.delay: 500
                    ToolTip.text: TrustLevels.description(root.peer.trust)
                    delegate: ItemDelegate {
                        required property int index
                        width: ListView.view ? ListView.view.width : implicitWidth
                        text: TrustLevels.texts[index]
                        font.pixelSize: 12
                        highlighted: trustBox.highlightedIndex === index
                        hoverEnabled: true
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        ToolTip.text: TrustLevels.descriptions[index]
                    }
                }
                Label {
                    // The other side chose less for us: that is what the pair shares.
                    visible: root.trustIndex(root.peer.effectiveTrust) < root.trustIndex(root.peer.trust)
                    text: root.peer.effectiveTrust === "guest" ? qsTr("Guest on its side")
                                                                : qsTr("This device only on its side")
                    font.pixelSize: 12
                    opacity: 0.7
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                    HoverHandler { id: effectiveHover }
                    ToolTip.visible: effectiveHover.hovered
                    ToolTip.delay: 500
                    ToolTip.text: qsTr("The other device chose this level for us; the pair shares the stricter of the two")
                }
            }
            Switch {
                // Given in advance or by the prompt ("Always allow"); taken back here.
                visible: root.showViewSwitch && root.peer.trust !== "guest"
                enabled: root.viewSwitchEnabled
                text: root.viewSwitchEnabled ? qsTr("May view this screen without asking")
                                             : qsTr("May view this screen without asking (remote view is off)")
                font.pixelSize: 12
                // Only the horizontal padding goes: without the vertical one the Material indicator
                // is taller than the control and covers the labels above.
                leftPadding: 0
                rightPadding: 0
                Layout.topMargin: 4
                checked: root.peer.allowView
                // toggle() puts the switch back without removing its binding (a click does not
                // either): it changes when the service reports the new permission.
                onToggled: {
                    root.viewAllowedToggled(checked)
                    toggle()
                }
            }
            Switch {
                // A Mac driven from a PC keyboard: Ctrl+C there is Cmd+C. Paired Macs only (the
                // same rows as the view switch).
                visible: root.showViewSwitch && root.peer.os === "macos" && root.peer.trust !== "guest"
                text: qsTr("Ctrl acts as ⌘ Command when typing on this Mac")
                font.pixelSize: 12
                leftPadding: 0
                rightPadding: 0
                checked: root.peer.swapCtrlCmd
                onToggled: {
                    root.swapCtrlCmdToggled(checked)
                    toggle()
                }
            }
            RowLayout {
                visible: root.showUpdate && root.peer.update !== ""
                spacing: 8
                Layout.fillWidth: true

                Label {
                    text: !root.updatesConfigured
                          ? qsTr("DeskFlower %1 is available, but updates are not configured in this build").arg(root.peer.update)
                          : !root.updatesSupported
                            ? qsTr("DeskFlower %1 is available, but updating is not supported on this platform yet").arg(root.peer.update)
                            : qsTr("DeskFlower %1 is available from this device").arg(root.peer.update)
                    font.pixelSize: 12
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
                Button {
                    visible: root.updatesConfigured && root.updatesSupported && root.updateState === ""
                    enabled: !root.updateBusy
                    text: qsTr("Update")
                    onClicked: root.updateClicked()
                }
            }
            RowLayout {
                visible: root.updateState !== ""
                spacing: 8
                Layout.fillWidth: true
                ProgressBar {
                    value: root.updateProgress
                    indeterminate: root.updateState !== "downloading"
                    Layout.fillWidth: true
                }
                Button {
                    // Only while downloading: verifying and installing cannot be stopped halfway.
                    visible: root.updateState === "downloading"
                    text: qsTr("Cancel")
                    flat: true
                    onClicked: root.updateCancelClicked()
                }
            }
            Label {
                visible: root.updateState !== ""
                text: root.updateState === "downloading"
                      ? qsTr("Downloading the update… %1%").arg(Math.floor(root.updateProgress * 100))
                      : root.updateState === "verifying" ? qsTr("Checking the signature…")
                                                         : qsTr("Installing the update…")
                font.pixelSize: 12
                opacity: 0.7
            }
        }

        Button {
            text: root.actionText
            enabled: root.actionEnabled
            flat: root.peer.trusted
            onClicked: root.actionClicked()
        }
    }
}
