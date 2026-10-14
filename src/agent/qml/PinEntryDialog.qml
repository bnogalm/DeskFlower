import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Shown on the device that started the pairing.
Dialog {
    id: root

    property string peerId
    property string peerName

    signal pinEntered(string pin)
    signal cancelRequested()

    function show(id, name) {
        peerId = id
        peerName = name
        pinField.clear()
        open()
        pinField.forceActiveFocus()
    }

    title: qsTr("Pair with %1").arg(peerName)
    modal: true
    closePolicy: Popup.NoAutoClose
    standardButtons: Dialog.Ok | Dialog.Cancel
    onAccepted: pinEntered(pinField.text)
    onRejected: cancelRequested()

    Component.onCompleted: standardButton(Dialog.Ok).enabled = Qt.binding(() => pinField.acceptableInput)

    ColumnLayout {
        spacing: 12

        Label {
            text: qsTr("Type the 6-digit code shown on <b>%1</b>.").arg(root.peerName)
            textFormat: Text.StyledText
            wrapMode: Text.WordWrap
            Layout.preferredWidth: 300
        }
        TextField {
            id: pinField
            placeholderText: qsTr("Code")
            font.pixelSize: 28
            font.family: "monospace"
            horizontalAlignment: TextInput.AlignHCenter
            inputMethodHints: Qt.ImhDigitsOnly
            validator: RegularExpressionValidator { regularExpression: /^[0-9]{6}$/ }
            Layout.fillWidth: true
            onAccepted: if (acceptableInput) root.accept()
        }
    }
}
