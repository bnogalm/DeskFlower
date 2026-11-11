pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Material
import QtQuick.Layouts

// One transfer of files from another device that the user sees (a paste or a drag): what it is,
// its progress, and a way to stop it. detailed (DeskFlower's progress window): also the rate and
// the time left, and once it ended how it went, with what to do with the files (open their folder
// on Windows, paste again elsewhere).
ColumnLayout {
    id: row

    required property Backend backend
    required property transferInfo modelData
    property bool detailed: false

    readonly property bool running: modelData.state === "running"

    spacing: 2

    // 1536 -> "1.5 KB": one decimal below 10 units.
    function amount(value: real): real {
        return value < 10 ? Math.round(value * 10) / 10 : Math.round(value)
    }
    function size(bytes: real): string {
        if (bytes < 1024)
            return qsTr("%1 B").arg(Math.round(bytes))
        if (bytes < 1024 * 1024)
            return qsTr("%1 KB").arg(row.amount(bytes / 1024))
        if (bytes < 1024 * 1024 * 1024)
            return qsTr("%1 MB").arg(row.amount(bytes / (1024 * 1024)))
        return qsTr("%1 GB").arg(row.amount(bytes / (1024 * 1024 * 1024)))
    }

    function timeLeft(seconds: real): string {
        if (seconds < 0)
            return ""
        if (seconds < 60)
            return qsTr("%n second(s) left", "", Math.max(1, Math.ceil(seconds)))
        if (seconds < 3600)
            return qsTr("about %n minute(s) left", "", Math.ceil(seconds / 60))
        return qsTr("about %n hour(s) left", "", Math.round(seconds / 3600))
    }

    function title(): string {
        const t = row.modelData
        const from = t.peerName
        if (t.state === "done")
            return t.fileCount > 0 ? qsTr("%n file(s) received from %1", "", t.fileCount).arg(from)
                                   : qsTr("Received from %1").arg(from)
        if (t.state === "cancelled")
            return qsTr("Cancelled: files from %1").arg(from)
        if (t.state === "failed")
            return t.error !== "" ? qsTr("Could not receive the files from %1: %2").arg(from).arg(t.error)
                                  : qsTr("Could not receive the files from %1").arg(from)
        if (t.total <= 0)
            return qsTr("Connecting to %1…").arg(from)
        if (t.fileCount > 0)
            return qsTr("Receiving from %1: %2 of %3 files").arg(from).arg(t.files).arg(t.fileCount)
        return qsTr("Receiving from %1").arg(from)
    }

    function details(): string {
        const t = row.modelData
        if (!row.running) {
            // The files are in their folder; elsewhere the paste that asked for them gave up waiting.
            if (t.state === "done" && Qt.platform.os !== "windows")
                return qsTr("Paste again to use them.")
            return ""
        }
        const done = t.done
        const total = t.total
        const rate = t.rateBytesPerSec
        const secondsLeft = t.secondsLeft
        if (total <= 0)
            return ""
        let text = qsTr("%1 of %2, %3 left").arg(row.size(done)).arg(row.size(total))
                       .arg(row.size(Math.max(0, total - done)))
        if (rate > 0)
            text += " · " + qsTr("%1/s").arg(row.size(rate))
        const left = row.timeLeft(secondsLeft)
        if (left !== "")
            text += " · " + left
        return text
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 8

        Label {
            Layout.fillWidth: true
            elide: Text.ElideRight
            maximumLineCount: 2
            wrapMode: row.running || !row.detailed ? Text.NoWrap : Text.Wrap
            text: row.title()
        }
        // Compact (the main window's strip): the progress on the title's line.
        ProgressBar {
            visible: row.running && !row.detailed
            Layout.preferredWidth: 120
            indeterminate: row.modelData.total <= 0
            value: row.modelData.total > 0 ? row.modelData.done / row.modelData.total : 0
        }
        // What is left of the total, then the rate: "13.8 GB left of 15 GB · 85 MB/s".
        Label {
            visible: row.running && !row.detailed && text !== ""
            font.pixelSize: 12
            opacity: 0.7
            text: {
                const t = row.modelData
                const parts = []
                if (t.total > 0)
                    parts.push(qsTr("%1 left of %2").arg(row.size(Math.max(0, t.total - t.done))).arg(row.size(t.total)))
                if (t.rateBytesPerSec > 0)
                    parts.push(qsTr("%1/s").arg(row.size(t.rateBytesPerSec)))
                return parts.join(" · ")
            }
        }
        Button {
            visible: row.running && row.modelData.cancellable
            text: qsTr("Cancel")
            flat: true
            topInset: 0
            bottomInset: 0
            implicitHeight: 32
            onClicked: row.backend.cancelTransfer(row.modelData.id)
        }
        Button {
            visible: row.detailed && row.modelData.state === "done" && Qt.platform.os === "windows"
                     && row.modelData.target !== ""
            text: qsTr("Open folder")
            flat: true
            topInset: 0
            bottomInset: 0
            implicitHeight: 32
            onClicked: row.backend.openTransferFolder(row.modelData.id)
        }
    }
    ProgressBar {
        visible: row.running && row.detailed
        Layout.fillWidth: true
        indeterminate: row.modelData.total <= 0
        value: row.modelData.total > 0 ? row.modelData.done / row.modelData.total : 0
    }
    Label {
        visible: row.detailed && text !== ""
        Layout.fillWidth: true
        elide: Text.ElideRight
        font.pixelSize: 12
        opacity: 0.7
        text: row.details()
    }
}
