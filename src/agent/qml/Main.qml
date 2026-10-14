import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ApplicationWindow {
    id: window

    required property Backend backend
    property int initialPage: 0       // 3: devices, with the advanced settings window open
    property bool startHidden: false  // --minimized: only the tray icon at first
    property bool hideOnClose: false  // with a system tray, closing hides the window

    width: 820
    height: 720
    minimumWidth: 480
    minimumHeight: 480
    visible: !startHidden
    onClosing: close => {
        if (hideOnClose) {
            close.accepted = false
            window.hide()
        }
    }
    title: "DeskFlower"

    Material.theme: Material.System
    Material.accent: Material.Teal
    Material.primary: Material.BlueGrey

    header: ToolBar {
        Material.background: Material.color(Material.BlueGrey, Material.Shade800)
        Material.foreground: "white"

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 16
            anchors.rightMargin: 8
            spacing: 12

            ColumnLayout {
                spacing: 0
                Layout.fillWidth: true

                Label {
                    text: window.backend.selfName || "…"
                    font.pixelSize: 18
                    font.bold: true
                    elide: Text.ElideRight
                    Layout.fillWidth: true

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: renameDialog.open()
                    }
                }
                Label {
                    text: window.backend.fingerprint
                    font.family: "monospace"
                    font.pixelSize: 11
                    opacity: 0.7
                }
            }

            Switch {
                text: qsTr("Discovery")
                checked: window.backend.exploring
                enabled: window.backend.serviceConnected
                onToggled: {
                    window.backend.setExploring(checked)
                    checked = Qt.binding(() => window.backend.exploring)
                }
            }

            ToolButton {
                text: "⋮"
                font.pixelSize: 20
                onClicked: appMenu.popup(this, 0, height)

                Menu {
                    id: appMenu
                    MenuItem {
                        text: qsTr("Start DeskFlower when I sign in")
                        checkable: true
                        checked: window.backend.autostart
                        enabled: window.backend.serviceConnected && window.backend.autostartSupported
                        onTriggered: {
                            window.backend.setAutostart(checked)
                            checked = Qt.binding(() => window.backend.autostart)
                        }
                    }
                    MenuItem {
                        text: qsTr("Advanced settings…")
                        enabled: window.backend.serviceConnected
                        onTriggered: {
                            advancedSettings.show()
                            advancedSettings.raise()
                            advancedSettings.requestActivate()
                        }
                    }
                    MenuSeparator {}
                    MenuItem {
                        text: qsTr("About DeskFlower")
                        onTriggered: aboutDialog.open()
                    }
                }
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Pane {
            visible: !window.backend.serviceConnected
            Layout.fillWidth: true
            Material.background: Material.color(Material.Orange, Material.Shade100)
            RowLayout {
                anchors.fill: parent
                spacing: 12
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: "#5d3a00"
                    text: window.backend.serviceStartError
                          ? qsTr("The DeskFlower service is not running: %1.").arg(window.backend.serviceStartError)
                          : qsTr("The DeskFlower service is not running.")
                }
                Button {
                    text: qsTr("Start")
                    onClicked: window.backend.startService()
                }
            }
        }

        Pane {
            visible: window.backend.serviceConnected && window.backend.screenViewerName !== ""
            Layout.fillWidth: true
            Material.background: Material.color(Material.Teal, Material.Shade100)
            RowLayout {
                anchors.fill: parent
                spacing: 12
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: "#00332c"
                    textFormat: Text.StyledText
                    text: qsTr("Your screen is being shown on <b>%1</b>.").arg(window.backend.screenViewerName)
                }
                Button {
                    text: qsTr("Stop")
                    onClicked: window.backend.stopScreenView()
                }
            }
        }

        TabBar {
            id: tabs
            Layout.fillWidth: true
            currentIndex: window.initialPage < 3 ? window.initialPage : 0
            TabButton { text: qsTr("Devices") }
            TabButton { text: qsTr("Screens") }
            TabButton { text: qsTr("Files") }
        }

        StackLayout {
            currentIndex: tabs.currentIndex
            Layout.fillWidth: true
            Layout.fillHeight: true

            DevicesPage {
                backend: window.backend
            }
            ScreensPage {
                backend: window.backend
                active: tabs.currentIndex === 1
            }
            FilesPage {
                backend: window.backend
            }
        }
    }

    // On every tab: version, the support notice and the bouquet (this desktop + the available ones).
    footer: Pane {
        padding: 0
        leftPadding: 16
        rightPadding: 8

        RowLayout {
            anchors.fill: parent
            spacing: 12

            Label {
                opacity: 0.6
                font.pixelSize: 11
                text: window.backend.serviceConnected
                      ? qsTr("DeskFlower %1  ·  UDP %2").arg(window.backend.selfVersion).arg(window.backend.port) : ""
            }
            Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                font.pixelSize: 14
                textFormat: Text.StyledText
                linkColor: Material.accent
                text: qsTr("Free for personal use. Using it for work, or want to support it? "
                           + "<a href=\"https://ko-fi.com/bnogal\">Ko-fi ♥</a>")
                onLinkActivated: link => Qt.openUrlExternally(link)

                HoverHandler {
                    enabled: parent.hoveredLink !== ""
                    cursorShape: Qt.PointingHandCursor
                }
            }
            Bouquet {
                Layout.alignment: Qt.AlignRight | Qt.AlignBottom
                deviceIds: [window.backend.selfId].concat(window.backend.trustedPeers
                                                         .filter(p => p.connected).map(p => p.id))
                                                 .filter(id => !!id)
                running: window.visible && window.visibility !== Window.Minimized
            }
        }
    }

    PinDisplayDialog {
        id: pinDisplay
        anchors.centerIn: parent
        onCancelRequested: window.backend.cancelPair(peerId)
    }

    PinEntryDialog {
        id: pinEntry
        anchors.centerIn: parent
        onPinEntered: pin => window.backend.submitPin(peerId, pin)
        onCancelRequested: window.backend.cancelPair(peerId)
    }

    ViewRequestDialog {
        id: viewRequest
        anchors.centerIn: parent
        onAnswered: decision => window.backend.answerView(peerId, decision)
    }

    AdvancedSettingsWindow {
        id: advancedSettings
        backend: window.backend
        Component.onCompleted: if (window.initialPage === 3) show()
    }

    AboutDialog {
        id: aboutDialog
        anchors.centerIn: parent
        backend: window.backend
    }

    Dialog {
        id: renameDialog
        anchors.centerIn: parent
        title: qsTr("Name of this device")
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAboutToShow: nameField.text = window.backend.selfName
        onAccepted: window.backend.rename(nameField.text)
        TextField {
            id: nameField
            implicitWidth: 280 // the dialog sizes its background from the content's implicit size
            maximumLength: 64
        }
    }

    ToolTip {
        id: toast
        x: (window.width - width) / 2
        y: window.height - height - 64
        timeout: 4000
    }

    Connections {
        target: window.backend
        function onPinShown(peerId, peerName, pin) {
            pinEntry.close()
            pinDisplay.show(peerId, peerName, pin)
        }
        function onPinRequested(peerId, peerName) {
            pinDisplay.close()
            pinEntry.show(peerId, peerName)
        }
        function onViewPromptRequested(peerId, peerName, timeoutMs) {
            viewRequest.show(peerId, peerName, timeoutMs)
        }
        function onViewPromptClosed(peerId) {
            if (viewRequest.peerId === peerId)
                viewRequest.close()
        }
        function onServiceConnectedChanged() {
            // An answer could not reach the service; it replays the prompt on reconnection.
            if (!window.backend.serviceConnected)
                viewRequest.close()
        }
        function onPhoneUploaded(fileName) {
            toast.text = qsTr("Received from the phone: %1").arg(fileName)
            toast.open()
        }
        function onNotice(message, problem) {
            toast.text = message
            toast.open()
        }
        function onServiceError(message) {
            toast.text = message
            toast.open()
        }
        function onUpdateFinished(ok, message) {
            toast.text = message
            toast.open()
        }
        function onPairingFinished(peerId, ok, error) {
            pinDisplay.close()
            pinEntry.close()
            toast.text = ok ? qsTr("Paired successfully") : qsTr("Pairing failed: %1").arg(error)
            toast.open()
        }
    }
}
