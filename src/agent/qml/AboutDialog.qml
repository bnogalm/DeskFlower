import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Version, license terms and the third-party notices (texts built into the agent).
Dialog {
    id: root

    required property Backend backend

    title: qsTr("About DeskFlower")
    modal: true
    standardButtons: Dialog.Close
    width: Math.min(parent ? parent.width - 48 : 760, 760)
    height: Math.min(parent ? parent.height - 48 : 640, 640)

    readonly property var documents: backend.licenseDocuments()

    function documentLabel(name) {
        if (name === "LICENSE.md")
            return qsTr("DeskFlower license (PolyForm Noncommercial 1.0.0)")
        if (name === "COMMERCIAL-LICENSE.md")
            return qsTr("Commercial license")
        if (name === "THIRD-PARTY-NOTICES.md")
            return qsTr("Third-party software")
        if (name === "licenses/qt-components.md")
            return qsTr("Components inside Qt")
        return qsTr("License text: %1").arg(name.replace(/^licenses\//, "").replace(/\.txt$/, ""))
    }

    onAboutToShow: documentBox.currentIndex = 0

    ColumnLayout {
        anchors.fill: parent
        spacing: 12

        Label {
            text: qsTr("DeskFlower %1").arg(Qt.application.version)
            font.pixelSize: 20
            font.bold: true
        }
        Label {
            text: "Copyright 2026 Benjamín Nogal Muñoz"
            opacity: 0.7
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            textFormat: Text.StyledText
            text: qsTr("DeskFlower is free for personal and other noncommercial use, under the <b>PolyForm "
                       + "Noncommercial License 1.0.0</b>. Using it for work (companies of any size, freelancers, "
                       + "any work done for profit) needs a commercial license: <b>5 € per user, once, for "
                       + "life</b>, VAT included, paid on Ko-fi. Write your billing country in the Ko-fi message; "
                       + "for a full invoice, email the receipt to vegawallet@gmail.com.")
        }
        RowLayout {
            spacing: 8
            Button {
                text: qsTr("Buy a license")
                highlighted: true
                onClicked: Qt.openUrlExternally("https://ko-fi.com/bnogal")
            }
            Button {
                text: qsTr("Support with a coffee")
                flat: true
                onClicked: Qt.openUrlExternally("https://ko-fi.com/bnogal")
            }
            Label {
                text: "vegawallet@gmail.com"
                opacity: 0.7
                Layout.leftMargin: 8
            }
        }
        Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            opacity: 0.7
            text: qsTr("Built with Qt (LGPL v3, linked dynamically and replaceable), OpenSSL, libsodium and "
                       + "the QR Code generator library by Project Nayuki.")
        }

        ComboBox {
            id: documentBox
            Layout.fillWidth: true
            model: root.documents.map(d => root.documentLabel(d))
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true

            TextArea {
                readOnly: true
                selectByMouse: true
                wrapMode: TextEdit.Wrap
                textFormat: root.documents[documentBox.currentIndex] !== undefined
                            && root.documents[documentBox.currentIndex].endsWith(".md")
                            ? TextEdit.MarkdownText : TextEdit.PlainText
                text: root.documents[documentBox.currentIndex] !== undefined
                      ? root.backend.licenseDocument(root.documents[documentBox.currentIndex]) : ""
                font.pixelSize: 12
            }
        }
    }
}
