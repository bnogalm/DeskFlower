pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Material
import QtQuick.Layouts

ApplicationWindow {
    id: window

    required property Backend backend
    property int initialPage: 0       // 3: devices, with the advanced settings window open
    property bool startHidden: false  // --minimized: only the tray icon at first
    property bool hideOnClose: false  // with a system tray, closing hides the window
    // The progress window (main.cpp places it on the screen).
    readonly property alias progressWindow: progressWindow

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
        Material.background: "#37474f"  // Material BlueGrey, Shade800
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
                // toggle() puts the switch back without removing its binding (a click does not
                // either): it changes when the service reports the new state.
                onToggled: {
                    window.backend.setExploring(checked)
                    toggle()
                }
            }

            ToolButton {
                text: "⋮"
                font.pixelSize: 20
                onClicked: appMenu.popup(this, 0, height)

                Menu {
                    id: appMenu
                    // As wide as the longest entry (the default width cuts them off).
                    width: {
                        let widest = 0
                        for (let i = 0; i < count; ++i) {
                            const item = itemAt(i)
                            if (item)
                                widest = Math.max(widest, item.implicitWidth)
                        }
                        return Math.max(widest, 200)
                    }
                    MenuItem {
                        text: qsTr("Start DeskFlower when I sign in")
                        checkable: true
                        checked: window.backend.autostart
                        enabled: window.backend.serviceConnected && window.backend.autostartSupported
                        onTriggered: {
                            window.backend.setAutostart(checked)
                            toggle() // back until the service reports it, binding kept (as above)
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
                        text: qsTr("Download the latest version…")
                        onTriggered: Qt.openUrlExternally(window.backend.downloadsUrl)
                    }
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
            Material.background: "#ffe0b2"  // Material Orange, Shade100
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

        // The session helper cannot share this computer's keyboard, mouse or screen (no display,
        // the desktop refused remote control, a missing macOS permission...): say why instead of
        // failing silently, with the system setting that fixes it where there is one.
        Pane {
            visible: window.backend.serviceConnected && window.backend.inputProblem !== ""
            Layout.fillWidth: true
            Material.background: "#ffe0b2"  // Material Orange, Shade100
            RowLayout {
                anchors.fill: parent
                spacing: 12
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: "#5d3a00"
                    // The macOS helper's texts say what to allow; the others name what failed.
                    text: Qt.platform.os === "osx"
                          ? window.backend.inputProblem
                          : qsTr("Keyboard and mouse sharing does not work on this computer: %1")
                            .arg(window.backend.inputProblem)
                }
                Button {
                    visible: "" + window.backend.inputProblemLink !== "" // the link as text
                    text: qsTr("Open System Settings")
                    onClicked: Qt.openUrlExternally(window.backend.inputProblemLink)
                }
            }
        }

        // A connected paired device of another protocol: file transfers with it are refused until
        // one of the two is updated. An older device can take this device's package with its own
        // Update button (it sees the offer in our hello); a newer one may offer its package here.
        Repeater {
            model: window.backend.protocolAlerts
            delegate: Pane {
                id: protocolBanner
                visible: window.backend.serviceConnected
                required property peerInfo modelData
                readonly property bool older: modelData.protocolMismatch === "older"
                readonly property bool canUpdateHere: !older && modelData.update !== ""
                                                      && window.backend.updatesConfigured
                                                      && window.backend.updatesSupported
                Layout.fillWidth: true
                Material.background: "#ffe0b2"  // Material Orange, Shade100
                RowLayout {
                    anchors.fill: parent
                    spacing: 12
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: "#5d3a00"
                        textFormat: Text.StyledText
                        text: {
                            const name = protocolBanner.modelData.name || protocolBanner.modelData.shortId
                            if (!protocolBanner.older)
                                return qsTr("<b>%1</b> uses a newer protocol (version %2). The protocol has changed: update this device.")
                                    .arg(name).arg(protocolBanner.modelData.protocol)
                            const base = qsTr("<b>%1</b> uses an older protocol (version %2). The protocol has changed: update it.")
                                .arg(name).arg(protocolBanner.modelData.protocol)
                            return protocolBanner.modelData.canServeUpdate
                                ? base + " " + qsTr("On %1, click Update on the Devices tab: this device can send it the update.").arg(name)
                                : base
                        }
                    }
                    Button {
                        visible: protocolBanner.canUpdateHere
                        enabled: window.backend.updateState === ""
                        text: qsTr("Update")
                        onClicked: window.backend.updateFrom(protocolBanner.modelData.id)
                    }
                    Button {
                        visible: !protocolBanner.canUpdateHere && !protocolBanner.modelData.canServeUpdate
                        flat: true
                        text: qsTr("Downloads")
                        onClicked: Qt.openUrlExternally(window.backend.downloadsUrl)
                    }
                }
            }
        }

        // Keyboard diagnostics record the keys that go between devices: always shown while on.
        Pane {
            visible: window.backend.keyDiagnosticsAvailable && window.backend.serviceConnected
                     && window.backend.keyDiagActive
            Layout.fillWidth: true
            Material.background: "#ffecb3"  // Material Amber, Shade100
            RowLayout {
                anchors.fill: parent
                spacing: 12
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: "#4d3800"
                    text: window.backend.keyDiagBy !== ""
                          ? qsTr("Keyboard diagnostics asked by %1: the keys that go between devices are being recorded.")
                            .arg(window.backend.keyDiagBy)
                          : qsTr("Keyboard diagnostics: the keys that go between devices are being recorded.")
                }
                Button {
                    text: qsTr("Show")
                    flat: true
                    onClicked: {
                        advancedSettings.show()
                        advancedSettings.raise()
                        advancedSettings.requestActivate()
                    }
                }
                Button {
                    text: qsTr("Stop")
                    onClicked: window.backend.setKeyDiagnostics(false)
                }
            }
        }

        Pane {
            visible: window.backend.serviceConnected && window.backend.screenViewerName !== ""
            Layout.fillWidth: true
            Material.background: "#b2dfdb"  // Material Teal, Shade100
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

        // Files being received from another device (a paste or a drag), with a way to stop them.
        // Light theme like the other banners, whatever the system's: dark text on the light strip.
        // Compact: one line per transfer (title, progress, Cancel).
        Pane {
            visible: transferRows.count > 0 // one row per transfer (the list's length does not compile)
            Layout.fillWidth: true
            topPadding: 2
            bottomPadding: 2
            leftPadding: 16
            rightPadding: 8
            Material.theme: Material.Light
            Material.background: "#eceff1"  // Material BlueGrey, Shade50
            ColumnLayout {
                anchors.fill: parent
                spacing: 0
                Repeater {
                    id: transferRows
                    model: window.backend.transfers
                    delegate: TransferRow {
                        backend: window.backend
                        Layout.fillWidth: true
                    }
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
                id: supportNotice
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                font.pixelSize: 14
                textFormat: Text.StyledText
                linkColor: Material.accentColor
                text: qsTr("Free for personal use. Using it for work, or want to support it? "
                           + "<a href=\"https://ko-fi.com/bnogal\">Ko-fi ♥</a>")
                onLinkActivated: link => Qt.openUrlExternally(link)

                HoverHandler {
                    enabled: supportNotice.hoveredLink !== ""
                    cursorShape: Qt.PointingHandCursor
                }
            }
            Bouquet {
                Layout.alignment: Qt.AlignRight | Qt.AlignBottom
                deviceIds: window.backend.connectedIds
                // Also still while files arrive (see Backend::transferBusy).
                running: window.visible && window.visibility !== Window.Minimized && !window.backend.transferBusy
            }
        }
    }

    PinDisplayDialog {
        id: pinDisplay
        anchors.centerIn: parent
        onCancelRequested: window.backend.cancelPair(peerId)
        onTrustChosen: trust => window.backend.setPairTrust(peerId, trust)
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

    // DeskFlower's own progress window for pasted files (and Receive), shown on its own.
    TransferWindow {
        id: progressWindow
        backend: window.backend
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
        function onPinShown(peerId: string, peerName: string, pin: string, trust: string, suggested: string): void {
            pinEntry.close()
            pinDisplay.show(peerId, peerName, pin, trust, suggested)
        }
        function onPinRequested(peerId: string, peerName: string, trust: string): void {
            pinDisplay.close()
            pinEntry.show(peerId, peerName, trust)
        }
        function onViewPromptRequested(peerId: string, peerName: string, timeoutMs: int): void {
            viewRequest.show(peerId, peerName, timeoutMs)
        }
        function onViewPromptClosed(peerId: string): void {
            if (viewRequest.peerId === peerId)
                viewRequest.close()
        }
        function onServiceConnectedChanged(): void {
            // An answer could not reach the service; it replays the prompt on reconnection.
            if (!window.backend.serviceConnected)
                viewRequest.close()
        }
        function onPhoneUploaded(fileName: string): void {
            toast.text = qsTr("Received from the phone: %1").arg(fileName)
            toast.open()
        }
        function onNotice(message: string, problem: bool): void {
            toast.text = message
            toast.open()
        }
        function onServiceError(message: string): void {
            toast.text = message
            toast.open()
        }
        function onUpdateFinished(ok: bool, message: string): void {
            toast.text = message
            toast.open()
        }
        function onPairingFinished(peerId: string, ok: bool, error: string): void {
            pinDisplay.close()
            pinEntry.close()
            toast.text = ok ? qsTr("Paired successfully") : qsTr("Pairing failed: %1").arg(error)
            toast.open()
        }
    }
}
