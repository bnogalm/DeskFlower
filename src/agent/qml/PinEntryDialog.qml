pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Material
import QtQuick.Layouts

// Shown on the device that started the pairing.
Dialog {
    id: root

    property string peerId
    property string peerName
    property string trust: "device" // what the device becomes here (chosen before the request)

    signal pinEntered(string pin)
    signal cancelRequested()

    function show(id: string, name: string, level: string): void {
        peerId = id
        peerName = name
        trust = level || "device"
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

    Component.onCompleted: standardButton(Dialog.Ok).enabled = Qt.binding(function(): bool { return pinField.acceptableInput })

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
        Label {
            text: root.trust === "guest"
                  ? qsTr("It will be a guest here: it cannot drive this computer and never sees its screen.")
                  : root.trust === "group"
                    ? qsTr("It will join all my devices: trusted by every device in the group, and it trusts them.")
                    : qsTr("It will be trusted by this device only.")
            opacity: 0.6
            wrapMode: Text.WordWrap
            font.pixelSize: 12
            Layout.preferredWidth: 300
        }
    }
}
