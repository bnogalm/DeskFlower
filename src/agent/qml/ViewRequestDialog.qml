import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Shown on the device whose screen a paired master wants to view. No answer counts as Deny.
Dialog {
    id: root

    property string peerId
    property string peerName
    property int remaining: 0  // seconds left before the request times out

    signal answered(string decision)  // "once", "always" or "deny"

    function show(id, name, timeoutMs) {
        peerId = id
        peerName = name
        remaining = Math.max(1, Math.ceil(timeoutMs / 1000))
        open()
    }

    title: qsTr("Screen view request")
    modal: true
    closePolicy: Popup.NoAutoClose
    onOpened: denyButton.forceActiveFocus()
    onAccepted: answered("once")  // accept() and reject() close the dialog first
    onRejected: answered("deny")
    onApplied: {
        answered("always")
        close()
    }

    // The service times the request out too; this only keeps the dialog from lingering.
    Timer {
        interval: 1000
        repeat: true
        running: root.visible
        onTriggered: {
            root.remaining -= 1
            if (root.remaining <= 0)
                root.close()
        }
    }

    ColumnLayout {
        spacing: 16

        Label {
            text: qsTr("<b>%1</b> wants to see this screen.").arg(root.peerName)
            textFormat: Text.StyledText
            wrapMode: Text.WordWrap
            Layout.preferredWidth: 340
        }
        Label {
            text: qsTr("It will also be able to use this keyboard and mouse while it watches. "
                       + "If you did not expect this, press Deny.")
            opacity: 0.6
            wrapMode: Text.WordWrap
            font.pixelSize: 12
            Layout.preferredWidth: 340
        }
        Label {
            text: qsTr("Denied automatically in %n second(s).", "", root.remaining)
            opacity: 0.6
            font.pixelSize: 12
        }
    }

    footer: DialogButtonBox {
        Button {
            text: qsTr("Allow once")
            flat: true
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
        }
        Button {
            text: qsTr("Always allow this device")
            flat: true
            DialogButtonBox.buttonRole: DialogButtonBox.ApplyRole
        }
        Button {
            id: denyButton
            text: qsTr("Deny")
            flat: true
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
        }
    }
}
