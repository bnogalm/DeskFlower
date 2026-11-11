pragma ComponentBehavior: Bound

pragma Singleton
import QtQuick

// The three trust levels as the service names them, strictest first, with the texts shown for
// them everywhere (PeerRow's selector and tooltips, TrustChooser's radios): one place, so they
// never drift. `values`, `texts` and `descriptions` are the typed lists (same order); `levels`
// joins them as objects for the selector's model.
QtObject {
    readonly property list<string> values: ["guest", "device", "group"]
    readonly property list<string> texts: [
        qsTr("Guest"),
        qsTr("This device only"),
        qsTr("All my devices")
    ]
    readonly property list<string> descriptions: [
        qsTr("Cannot drive this computer, never sees its screen; gets the clipboard only when you paste on it with the keyboard"),
        qsTr("One of your devices, trusted by this device only: every pair of devices needs its own PIN"),
        qsTr("One of your devices, trusted by every device of your group and trusting them: one PIN pairs it with the whole group. Members share keyboard, mouse, clipboard and files")
    ]
    readonly property list<var> levels: [
        { value: values[0], text: texts[0], description: descriptions[0] },
        { value: values[1], text: texts[1], description: descriptions[1] },
        { value: values[2], text: texts[2], description: descriptions[2] }
    ]

    // Index of a level; an unknown one counts as "device" (what the service assumes).
    function index(level: string): int {
        for (let i = 0; i < values.length; ++i)
            if (values[i] === level)
                return i
        return 1
    }
    function description(level: string): string {
        const i = index(level)  // first: the list is not read across the call
        return descriptions[i]
    }
}
