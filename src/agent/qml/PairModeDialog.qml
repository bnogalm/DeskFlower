pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Material
import QtQuick.Layouts

// Asked before a pairing request: what the device becomes here once paired.
Dialog {
    id: root

    property string peerId
    property string peerName

    signal chosen(string peerId, string trust)

    // current: the peer's trust from the peers model, which may lack it (var, so a missing one stays undefined).
    function show(id: string, name: string, current: var): void {
        peerId = id
        peerName = name
        chooser.value = current || "device"
        open()
    }

    title: qsTr("Pair with %1").arg(peerName)
    modal: true
    standardButtons: Dialog.Ok | Dialog.Cancel
    onAccepted: chosen(peerId, chooser.value)

    ColumnLayout {
        spacing: 12

        Label {
            text: qsTr("What <b>%1</b> becomes on this device:").arg(root.peerName)
            textFormat: Text.StyledText
            wrapMode: Text.WordWrap
            Layout.preferredWidth: 320
        }
        TrustChooser {
            id: chooser
            Layout.fillWidth: true
        }
    }
}
