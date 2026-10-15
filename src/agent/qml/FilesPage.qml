import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Dialogs
import QtQuick.Layouts
import Qt.labs.folderlistmodel

// Shared folder: where phone uploads go (copied and dragged files wait elsewhere). Phones on the
// local network reach it over HTTPS after scanning the QR code.
Item {
    id: page

    required property Backend backend
    property int urlIndex: 0

    readonly property string phoneUrl: backend.phoneUrls.length > 0
                                       ? backend.phoneUrls[Math.min(urlIndex, backend.phoneUrls.length - 1)] : ""
    readonly property var imageSuffixes: ["jpg", "jpeg", "png", "gif", "webp", "bmp"]

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 12

        // --- Shared folder ---------------------------------------------------------------------
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Label {
                text: qsTr("Shared folder")
                font.bold: true
            }
            Label {
                Layout.fillWidth: true
                text: page.backend.sharedFolder
                elide: Text.ElideMiddle
                opacity: 0.8
            }
            Button {
                text: qsTr("Change…")
                enabled: page.backend.serviceConnected
                onClicked: folderDialog.open()
            }
            Button {
                text: qsTr("Open")
                highlighted: true
                enabled: page.backend.sharedFolder.length > 0
                onClicked: page.backend.openSharedFolder()
            }
        }

        // --- Phone access ----------------------------------------------------------------------
        Frame {
            Layout.fillWidth: true

            RowLayout {
                anchors.fill: parent
                spacing: 16

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignTop
                    spacing: 6

                    Switch {
                        id: phoneSwitch
                        text: qsTr("Phone access on the local network")
                        checked: page.backend.phoneAccess
                        enabled: page.backend.serviceConnected
                        onToggled: {
                            page.backend.setPhoneAccess(checked)
                            checked = Qt.binding(() => page.backend.phoneAccess)
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        opacity: 0.7
                        text: page.backend.phoneRunning
                              ? qsTr("Scan the code with the phone's camera to upload photos and files to the shared folder, download them, or send text to this computer's clipboard. The phone will warn about the certificate the first time: it is this computer's own.")
                              : qsTr("Turn it on to show a QR code for your phone.")
                    }
                    Label {
                        visible: page.backend.phoneError.length > 0
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: Material.color(Material.Red)
                        text: page.backend.phoneError
                    }
                    ComboBox {
                        visible: page.backend.phoneRunning && page.backend.phoneUrls.length > 1
                        Layout.fillWidth: true
                        model: page.backend.phoneUrls.map(u => u.replace(/\/\?t=.*$/, ""))
                        currentIndex: page.urlIndex
                        onActivated: index => page.urlIndex = index
                    }
                    TextField {
                        visible: page.phoneUrl.length > 0
                        Layout.fillWidth: true
                        readOnly: true
                        selectByMouse: true
                        font.pixelSize: 12
                        text: page.phoneUrl
                    }
                    RowLayout {
                        visible: page.backend.phoneRunning
                        spacing: 8
                        Button {
                            text: qsTr("New code")
                            onClicked: page.backend.newPhoneLink()
                        }
                        Label {
                            text: page.backend.phoneSessions === 1 ? qsTr("1 phone connected")
                                                                  : qsTr("%1 phones connected").arg(page.backend.phoneSessions)
                            opacity: 0.8
                        }
                        Button {
                            visible: page.backend.phoneSessions > 0
                            flat: true
                            text: qsTr("Disconnect phones")
                            onClicked: page.backend.revokePhones()
                        }
                    }
                }

                Rectangle {
                    visible: page.backend.phoneRunning && page.phoneUrl.length > 0
                    Layout.preferredWidth: 200
                    Layout.preferredHeight: 200
                    color: "white"
                    radius: 8

                    Image {
                        anchors.fill: parent
                        anchors.margins: 4
                        source: page.phoneUrl.length > 0 ? "image://qr/" + encodeURIComponent(page.phoneUrl) : ""
                        sourceSize: Qt.size(400, 400)
                        fillMode: Image.PreserveAspectFit
                        smooth: false
                        cache: false
                    }
                }
            }
        }

        // --- Folder contents -------------------------------------------------------------------
        Label {
            text: qsTr("Files")
            font.bold: true
        }

        GridView {
            id: grid
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            cellWidth: 150
            cellHeight: 150
            ScrollBar.vertical: ScrollBar {}

            model: FolderListModel {
                folder: page.backend.sharedFolderUrl
                showDirsFirst: true
                showDotAndDotDot: false
                showHidden: false
                sortField: FolderListModel.Time
            }

            delegate: ItemDelegate {
                id: cell

                required property string fileName
                required property url fileUrl
                required property bool fileIsDir
                required property string fileSuffix

                width: grid.cellWidth - 8
                height: grid.cellHeight - 8
                padding: 6
                onDoubleClicked: Qt.openUrlExternally(cell.fileUrl)
                ToolTip.visible: hovered
                ToolTip.delay: 600
                ToolTip.text: cell.fileName

                contentItem: ColumnLayout {
                    spacing: 4

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        radius: 6
                        color: Material.color(Material.BlueGrey, Material.Shade800)
                        clip: true

                        Image {
                            id: preview
                            anchors.fill: parent
                            visible: !cell.fileIsDir && page.imageSuffixes.includes(cell.fileSuffix.toLowerCase())
                            source: visible ? cell.fileUrl : ""
                            sourceSize: Qt.size(240, 180)
                            fillMode: Image.PreserveAspectCrop
                            asynchronous: true
                            autoTransform: true
                        }
                        Label {
                            anchors.centerIn: parent
                            visible: !preview.visible || preview.status === Image.Error
                            text: cell.fileIsDir ? qsTr("Folder") : (cell.fileSuffix.length > 0 ? cell.fileSuffix.toUpperCase() : qsTr("File"))
                            color: "white"
                            opacity: 0.8
                            font.bold: true
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        text: cell.fileName
                        elide: Text.ElideMiddle
                        horizontalAlignment: Text.AlignHCenter
                        font.pixelSize: 12
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                visible: grid.count === 0
                opacity: 0.6
                text: qsTr("The shared folder is empty.")
            }
        }
    }

    FolderDialog {
        id: folderDialog
        title: qsTr("Choose the shared folder")
        currentFolder: page.backend.sharedFolderUrl
        onAccepted: page.backend.setSharedFolder(selectedFolder)
    }

    // A folder the service refused: say why and offer to choose another.
    Dialog {
        id: rejectedDialog
        property string path
        property string reason
        anchors.centerIn: parent
        width: Math.min(parent.width - 48, 460)
        modal: true
        title: qsTr("This folder cannot be shared")

        ColumnLayout {
            anchors.fill: parent
            spacing: 8
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: qsTr("%1 cannot be the shared folder: %2.").arg(rejectedDialog.path).arg(rejectedDialog.reason)
            }
            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                opacity: 0.7
                text: qsTr("Phones and paired computers can put files there, so it must be an ordinary folder "
                           + "of yours, such as one in Documents or Desktop. The shared folder stays %1.")
                      .arg(page.backend.sharedFolder)
            }
        }
        footer: DialogButtonBox {
            Button {
                text: qsTr("Choose another folder…")
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            }
            Button {
                text: qsTr("Cancel")
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            }
        }
        onAccepted: folderDialog.open()
    }

    Connections {
        target: page.backend
        function onSharedFolderRejected(path, reason) {
            rejectedDialog.path = path
            rejectedDialog.reason = reason
            rejectedDialog.open()
        }
    }
}
