import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Shown on the device that received the pairing request.
Dialog {
    id: root

    property string peerId
    property string peerName
    property string pin

    signal cancelRequested()

    function show(id, name, code) {
        peerId = id
        peerName = name
        pin = code
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
            Layout.preferredWidth: 300
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
            text: qsTr("If you did not start this pairing, press Cancel.")
            opacity: 0.6
            wrapMode: Text.WordWrap
            font.pixelSize: 12
            Layout.preferredWidth: 300
        }
    }
}
