pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Material
import QtQuick.Layouts

// Three radio buttons for a trust level, with a line on what each one means (texts from
// TrustLevels). `value` is the level the service uses ("guest", "device" or "group"); set it to
// preselect one. The radios
// are driven from `value` rather than bound to it, since an exclusive group sets `checked`
// itself and would break bindings.
ColumnLayout {
    id: root

    property string value: "device"
    signal picked(string value)

    spacing: 0

    onValueChanged: {
        guest.checked = value === "guest"
        device.checked = value === "device"
        group.checked = value === "group"
    }
    Component.onCompleted: valueChanged()

    ButtonGroup {
        id: levels
        onClicked: button => {
            root.value = button === guest ? "guest" : button === group ? "group" : "device"
            root.picked(root.value)
        }
    }

    RadioButton {
        id: guest
        ButtonGroup.group: levels
        text: TrustLevels.texts[0]
    }
    Label {
        text: TrustLevels.descriptions[0]
        font.pixelSize: 12
        opacity: 0.7
        wrapMode: Text.WordWrap
        Layout.leftMargin: 40
        Layout.fillWidth: true
    }
    RadioButton {
        id: device
        ButtonGroup.group: levels
        text: TrustLevels.texts[1]
    }
    Label {
        text: TrustLevels.descriptions[1]
        font.pixelSize: 12
        opacity: 0.7
        wrapMode: Text.WordWrap
        Layout.leftMargin: 40
        Layout.fillWidth: true
    }
    RadioButton {
        id: group
        ButtonGroup.group: levels
        text: TrustLevels.texts[2]
    }
    Label {
        text: TrustLevels.descriptions[2]
        font.pixelSize: 12
        opacity: 0.7
        wrapMode: Text.WordWrap
        Layout.leftMargin: 40
        Layout.fillWidth: true
    }
}
