import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ItemDelegate {
    id: root

    property var peer
    property string actionText
    property bool actionEnabled: true
    property string hint
    property bool showViewSwitch: false  // paired peers: may view this screen without asking
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
    signal updateClicked()

    implicitHeight: Math.max(64, implicitContentHeight + topPadding + bottomPadding)

    contentItem: RowLayout {
        spacing: 12

        Rectangle {
            width: 10
            height: 10
            radius: 5
            color: root.peer.connected ? Material.color(Material.Green)
                                       : (root.peer.trusted ? Material.color(Material.Grey) : "transparent")
            border.width: root.peer.trusted ? 0 : 1
            border.color: Material.color(Material.Grey)
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
                text: [root.peer.os,
                       root.peer.version ? "v" + root.peer.version : "",
                       root.peer.address ? root.peer.address + ":" + root.peer.port : "",
                       root.peer.trusted ? (root.peer.connected ? qsTr("connected") : qsTr("disconnected")) : root.hint]
                      .filter(s => s && s.length > 0).join("  ·  ")
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
            Switch {
                // Revoke only: the grant comes from the prompt ("Always allow"), which the
                // requester cannot answer remotely.
                visible: root.showViewSwitch && !!root.peer.allowView
                text: qsTr("May view this screen without asking")
                font.pixelSize: 12
                // Only the horizontal padding goes: without the vertical one the Material indicator
                // is taller than the control and covers the labels above.
                leftPadding: 0
                rightPadding: 0
                Layout.topMargin: 4
                checked: !!root.peer.allowView
                onToggled: {
                    root.viewAllowedToggled(checked)
                    checked = Qt.binding(() => !!root.peer.allowView)
                }
            }
            RowLayout {
                visible: root.showUpdate && !!root.peer.update
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
            ProgressBar {
                visible: root.updateState !== ""
                value: root.updateProgress
                indeterminate: root.updateState !== "downloading"
                Layout.fillWidth: true
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
