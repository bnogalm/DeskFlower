pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Material
import QtQuick.Layouts

// Shown on the device that received the pairing request.
Dialog {
    id: root

    property string peerId
    property string peerName
    property string pin
    property string suggested

    signal cancelRequested()
    signal trustChosen(string trust)

    // trust: the level to offer: the one the requester chose for this device (its suggestion),
    // except that a device paired again never gets less than it had, a guest stays a guest and
    // Group is never offered on the requester's word alone; "device" when nothing was suggested.
    // The service applies it unless the user picks another. suggested: a higher level the
    // requester asked for ("group"), which applies only if picked here: pointed at, not chosen.
    function show(id: string, name: string, code: string, trust: string, wish: string): void {
        peerId = id
        peerName = name
        pin = code
        suggested = wish || ""
        chooser.value = trust || "device"
        open()
    }

    title: qsTr("Pairing request")
    modal: true
    closePolicy: Popup.NoAutoClose
    standardButtons: Dialog.Cancel
    onRejected: cancelRequested()

    ColumnLayout {
        spacing: 16

        Label {
            text: qsTr("Enter this code on <b>%1</b>:").arg(root.peerName)
            textFormat: Text.StyledText
            wrapMode: Text.WordWrap
            Layout.preferredWidth: 320
        }
        Label {
            text: root.pin.substring(0, 3) + " " + root.pin.substring(3)
            font.pixelSize: 44
            font.bold: true
            font.letterSpacing: 6
            font.family: "monospace"
            Layout.alignment: Qt.AlignHCenter
        }
        Label {
            text: qsTr("What it becomes on this device:")
            wrapMode: Text.WordWrap
            Layout.preferredWidth: 320
        }
        Label {
            // Joining the group lets the device add others to all of them: the user's own pick.
            visible: root.suggested === "group" && chooser.value !== "group"
            text: qsTr("<b>%1</b> asks to join <b>%2</b>. It can then add other devices to all of "
                       + "your devices: choose it only if %1 is one of yours.")
                      .arg(root.peerName).arg(TrustLevels.levels[2].text)
            textFormat: Text.StyledText
            wrapMode: Text.WordWrap
            color: palette.highlight
            Layout.preferredWidth: 320
        }
        TrustChooser {
            id: chooser
            Layout.fillWidth: true
            onPicked: trust => root.trustChosen(trust)
        }
        Label {
            text: qsTr("If you did not start this pairing, press Cancel.")
            opacity: 0.6
            wrapMode: Text.WordWrap
            font.pixelSize: 12
            Layout.preferredWidth: 320
        }
    }
}
