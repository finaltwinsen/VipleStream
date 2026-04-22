import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.3

import ComputerModel 1.0

import ComputerManager 1.0
import StreamingPreferences 1.0
import SystemProperties 1.0
import SdlGamepadKeyNavigation 1.0

CenteredGridView {
    property ComputerModel computerModel : createModel()

    // Bold variant pushes the grid into cover-story territory with
    // ~35% bigger host cards; Safe keeps the original tight grid.
    readonly property bool bold: StreamingPreferences.designVariant == StreamingPreferences.DV_BOLD

    id: pcGrid
    focus: true
    activeFocusOnTab: true
    topMargin: 20
    bottomMargin: 5
    cellWidth: bold ? 420 : 310
    cellHeight: bold ? 440 : 330
    objectName: qsTr("Computers")

    Component.onCompleted: {
        // Don't show any highlighted item until interacting with them.
        // We do this here instead of onActivated to avoid losing the user's
        // selection when backing out of a different page of the app.
        currentIndex = -1
    }

    // Note: Any initialization done here that is critical for streaming must
    // also be done in CliStartStreamSegue.qml, since this code does not run
    // for command-line initiated streams.
    StackView.onActivated: {
        // Setup signals on CM
        ComputerManager.computerAddCompleted.connect(addComplete)

        // Highlight the first item if a gamepad is connected
        if (currentIndex === -1 && SdlGamepadKeyNavigation.getConnectedGamepads() > 0) {
            currentIndex = 0
        }
    }

    StackView.onDeactivating: {
        ComputerManager.computerAddCompleted.disconnect(addComplete)
    }

    function pairingComplete(error)
    {
        // Close the PIN dialog
        pairDialog.close()

        // Display a failed dialog if we got an error
        if (error !== undefined) {
            errorDialog.text = error
            errorDialog.helpText = ""
            errorDialog.open()
        }
    }

    function addComplete(success, detectedPortBlocking)
    {
        if (!success) {
            errorDialog.text = qsTr("Unable to connect to the specified PC.")

            if (detectedPortBlocking) {
                errorDialog.text += "\n\n" + qsTr("This PC's Internet connection is blocking Moonlight. Streaming over the Internet may not work while connected to this network.")
            }
            else {
                errorDialog.helpText = qsTr("Click the Help button for possible solutions.")
            }

            errorDialog.open()
        }
    }

    function createModel()
    {
        var model = Qt.createQmlObject('import ComputerModel 1.0; ComputerModel {}', parent, '')
        model.initialize(ComputerManager)
        model.pairingCompleted.connect(pairingComplete)
        model.connectionTestCompleted.connect(testConnectionDialog.connectionTestComplete)
        return model
    }

    Row {
        anchors.centerIn: parent
        spacing: 5
        visible: pcGrid.count === 0

        BusyIndicator {
            id: searchSpinner
            visible: StreamingPreferences.enableMdns
            running: visible
        }

        Label {
            height: searchSpinner.height
            elide: Label.ElideRight
            text: StreamingPreferences.enableMdns ? qsTr("Searching for compatible hosts on your local network...")
                                                  : qsTr("Automatic PC discovery is disabled. Add your PC manually.")
            font.pointSize: 20
            verticalAlignment: Text.AlignVCenter
            wrapMode: Text.Wrap
        }
    }

    model: computerModel

    // Helper: open the featured host via the same pipeline a tile
    // click would. Called by the Bold hero banner.
    function activateFeatured() {
        var idx = computerModel.featuredComputerIndex();
        if (idx < 0) return;
        // Re-dispatch to the delegate's onClicked logic by finding
        // its index and forcing focus + click. Simpler: inline the
        // branching here — the delegate logic is small.
        // Unfortunately we don't have direct access to `model` at
        // this level; emulate by pulling the session directly.
        // For now, just push AppView if the featured host is online
        // + paired (the common case); anything else falls through
        // to the grid for the user to click manually.
        // (Emulating onPressAndHold / context menu not supported
        // here — that still requires a tile click.)
        pcGrid.currentIndex = idx;
        if (pcGrid.currentItem) {
            pcGrid.currentItem.clicked();
        }
    }

    // VipleStream §01 Bold cover banner — a magazine masthead that
    // sits above the host grid when the design variant is DV_BOLD,
    // giving the screen the "HOSTS / LOCAL NETWORK" editorial look
    // from the Claude-Design mock.  Collapses to zero height on Safe.
    //
    // GridView.header is a Component rendered flush at the top,
    // full-width, above the first grid row. Scrolls with content.
    header: Component {
        Rectangle {
            id: heroBanner
            visible: pcGrid.bold && pcGrid.count > 0
            width: pcGrid.width
            // Tucks flush against the grid when Bold, invisible + 0-height
            // on Safe so the existing grid layout is completely undisturbed.
            height: visible ? 260 : 0
            color: "#0D0F0B"   // ink

            property string featuredName:
                visible ? computerModel.nameAt(computerModel.featuredComputerIndex()) : ""

            MouseArea {
                anchors.fill: parent
                cursorShape: heroBanner.featuredName !== "" ? Qt.PointingHandCursor : Qt.ArrowCursor
                onClicked: pcGrid.activateFeatured()
                hoverEnabled: true
            }

            // Diagonal stripe pattern tiled across the banner.
            Image {
                anchors.fill: parent
                source: "qrc:/res/vs_diag_stripe.svg"
                fillMode: Image.Tile
                sourceSize: Qt.size(24, 24)
                opacity: 0.65
            }
            // Gradient wash so content reads against the stripes.
            Rectangle {
                anchors.fill: parent
                gradient: Gradient {
                    GradientStop { position: 0.0; color: "#CC0D0F0B" }
                    GradientStop { position: 0.4; color: "#550D0F0B" }
                    GradientStop { position: 1.0; color: "#CC0D0F0B" }
                }
            }

            // Top-left meta + ◤ HOSTS masthead
            Column {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.leftMargin: 32
                anchors.topMargin: 22
                spacing: 6

                Text {
                    text: "◤ " + qsTr("HOSTS")
                    font.family: "Space Grotesk"
                    font.pointSize: 34
                    font.bold: true
                    font.letterSpacing: -1.4
                    color: "#D4FF3A"   // lime
                }
                Text {
                    text: qsTr("LOCAL NETWORK") + " · " +
                          Qt.formatDateTime(new Date(), "ddd HH:mm").toUpperCase()
                    font.family: "IBM Plex Mono"
                    font.pointSize: 10
                    font.letterSpacing: 1.8
                    color: "#8B8E7E"    // mute
                }
            }

            // Bottom-left: featured host name as cover-story display
            // type (§01 Bold mock). Clickable via the outer MouseArea.
            Column {
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                anchors.leftMargin: 32
                anchors.bottomMargin: 20
                spacing: 4
                width: heroBanner.width - 380

                Text {
                    text: "FEATURED · 01 / " + ("0" + pcGrid.count).slice(-2)
                    font.family: "IBM Plex Mono"
                    font.pointSize: 9
                    font.letterSpacing: 1.8
                    color: "#D4FF3A"
                }
                Text {
                    visible: heroBanner.featuredName !== ""
                    text: heroBanner.featuredName
                    font.family: "Space Grotesk"
                    font.pointSize: 46
                    font.bold: true
                    font.letterSpacing: -2.4
                    color: "#F2F5E1"
                    width: parent.width
                    elide: Text.ElideRight
                }
                Text {
                    visible: heroBanner.featuredName === ""
                    text: qsTr("PICK A HOST TO CONNECT")
                    font.family: "IBM Plex Mono"
                    font.pointSize: 11
                    font.letterSpacing: 2.0
                    color: "#F2F5E1"
                }
            }

            // Rotated tape sticker, top-right — editorial accent.
            Rectangle {
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.rightMargin: -24
                anchors.topMargin: 64
                width: 260
                height: 36
                color: "#F2F5E1"
                rotation: -3
                transformOrigin: Item.Center

                Text {
                    anchors.centerIn: parent
                    text: "STREAM · PATCH · 2026"
                    font.family: "IBM Plex Mono"
                    font.pointSize: 11
                    font.bold: true
                    font.letterSpacing: 3.0
                    color: "#0D0F0B"
                }
            }

            // Small lime live-pip top-right above the tape.
            Rectangle {
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.rightMargin: 28
                anchors.topMargin: 28
                width: 12; height: 12; radius: 2
                color: "#D4FF3A"
                SequentialAnimation on opacity {
                    loops: Animation.Infinite
                    NumberAnimation { from: 1.0; to: 0.35; duration: 900 }
                    NumberAnimation { from: 0.35; to: 1.0; duration: 900 }
                }
            }
        }
    }

    delegate: NavigableItemDelegate {
        width: pcGrid.bold ? 410 : 300
        height: pcGrid.bold ? 430 : 320
        grid: pcGrid

        property alias pcContextMenu : pcContextMenuLoader.item

        // VipleStream editorial host card. Preserves the same state
        // bindings as before (online / paired / statusUnknown) but
        // renders them in the §01 Safe PcList mock style: §NN index
        // badge, status dot + monospace label in the corner, thin
        // hairline between the icon "box art" region and the name.

        // Top meta strip: §NN index on the left, status dot + label on the right
        Item {
            id: pcMetaBar
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.margins: 10
            height: 14
            z: 2

            Text {
                id: pcIndexLabel
                text: "§ " + (index < 9 ? "0" + (index + 1) : (index + 1))
                font.family: "IBM Plex Mono"
                font.pointSize: 9
                font.letterSpacing: 1.4
                color: "#8B8E7E"    // vs mute
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
            }

            Row {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: 6

                // Status dot — lime/danger/mute depending on state
                Rectangle {
                    id: pcStatusDot
                    width: 8; height: 8; radius: 1
                    visible: !model.statusUnknown
                    color: !model.online          ? "#8B8E7E"     // mute / offline
                          : !model.paired          ? "#FF5A4E"     // danger / unpaired
                                                   : "#D4FF3A"     // lime / ready
                    anchors.verticalCenter: parent.verticalCenter
                }

                Text {
                    id: pcStatusLabel
                    visible: !model.statusUnknown
                    text: !model.online  ? qsTr("OFFLINE")
                        : !model.paired   ? qsTr("UNPAIRED")
                                          : qsTr("READY")
                    font.family: "IBM Plex Mono"
                    font.pointSize: 9
                    font.letterSpacing: 1.4
                    color: !model.online  ? "#8B8E7E"
                         : !model.paired   ? "#FF5A4E"
                                           : "#D4FF3A"
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
        }

        // "Box art" area — the desktop icon sits inside an ink2 panel.
        // Bold variant gives it more vertical real estate so the
        // host card reads as a magazine cover.
        Rectangle {
            id: pcBoxArt
            anchors.top: pcMetaBar.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            anchors.topMargin: 8
            height: pcGrid.bold ? 300 : 200
            color: "#14170F"                 // ink2
            border.color: "#2D3127"          // line2
            border.width: 1
            radius: 0

            Image {
                id: pcIcon
                anchors.centerIn: parent
                source: "qrc:/res/desktop_windows-48px.svg"
                sourceSize { width: pcGrid.bold ? 200 : 140; height: pcGrid.bold ? 200 : 140 }
                opacity: model.online ? 1.0 : 0.55
            }

            Image {
                id: stateIcon
                anchors.centerIn: parent
                visible: !model.statusUnknown && (!model.online || !model.paired)
                source: !model.online ? "qrc:/res/warning_FILL1_wght300_GRAD200_opsz24.svg" : "qrc:/res/baseline-lock-24px.svg"
                sourceSize {
                    width: !model.online ? 60 : 56
                    height: !model.online ? 60 : 56
                }
            }

            BusyIndicator {
                id: statusUnknownSpinner
                anchors.centerIn: parent
                width: 60; height: 60
                visible: model.statusUnknown
                running: visible
            }
        }

        // Hairline divider
        Rectangle {
            id: pcDivider
            anchors.top: pcBoxArt.bottom
            anchors.topMargin: 10
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            height: 1
            color: "#1F2219"    // vs line
        }

        Label {
            id: pcNameText
            text: model.name

            anchors.top: pcDivider.bottom
            anchors.topMargin: 6
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            anchors.bottom: parent.bottom
            // Bold: cover-story display type. Safe: editorial heading.
            font.pointSize: pcGrid.bold ? 26 : 18
            font.family: "Space Grotesk"
            font.bold: true
            font.letterSpacing: pcGrid.bold ? -0.8 : -0.4
            color: "#F2F5E1"    // vs paper
            horizontalAlignment: Text.AlignLeft
            verticalAlignment: Text.AlignTop
            wrapMode: Text.Wrap
            elide: Text.ElideRight
        }

        Loader {
            id: pcContextMenuLoader
            asynchronous: true
            sourceComponent: NavigableMenu {
                id: pcContextMenu
                initiator: pcContextMenuLoader.parent
                MenuItem {
                    text: qsTr("PC Status: %1").arg(model.online ? qsTr("Online") : qsTr("Offline"))
                    font.bold: true
                    enabled: false
                }
                NavigableMenuItem {
                    text: qsTr("View All Apps")
                    onTriggered: {
                        var component = Qt.createComponent("AppView.qml")
                        var appView = component.createObject(stackView, {"computerIndex": index, "objectName": model.name, "showHiddenGames": true})
                        stackView.push(appView)
                    }
                    visible: model.online && model.paired
                }
                NavigableMenuItem {
                    text: qsTr("Wake PC")
                    onTriggered: computerModel.wakeComputer(index)
                    visible: !model.online && model.wakeable
                }
                NavigableMenuItem {
                    text: StreamingPreferences.autoWakeOnLan
                          ? qsTr("Auto Wake-on-LAN: ON")
                          : qsTr("Auto Wake-on-LAN: OFF")
                    onTriggered: {
                        StreamingPreferences.autoWakeOnLan = !StreamingPreferences.autoWakeOnLan
                        StreamingPreferences.save()
                    }
                    visible: model.wakeable
                }
                NavigableMenuItem {
                    text: qsTr("Test Network")
                    onTriggered: {
                        computerModel.testConnectionForComputer(index)
                        testConnectionDialog.open()
                    }
                }

                NavigableMenuItem {
                    text: qsTr("Rename PC")
                    onTriggered: {
                        renamePcDialog.pcIndex = index
                        renamePcDialog.originalName = model.name
                        renamePcDialog.open()
                    }
                }
                NavigableMenuItem {
                    text: qsTr("Delete PC")
                    onTriggered: {
                        deletePcDialog.pcIndex = index
                        deletePcDialog.pcName = model.name
                        deletePcDialog.open()
                    }
                }
                NavigableMenuItem {
                    text: qsTr("View Details")
                    onTriggered: {
                        showPcDetailsDialog.pcDetails = model.details
                        showPcDetailsDialog.open()
                    }
                }
            }
        }

        onClicked: {
            if (model.online) {
                if (!model.serverSupported) {
                    errorDialog.text = qsTr("The version of GeForce Experience on %1 is not supported by this build of Moonlight. You must update Moonlight to stream from %1.").arg(model.name)
                    errorDialog.helpText = ""
                    errorDialog.open()
                }
                else if (model.paired) {
                    // go to game view
                    var component = Qt.createComponent("AppView.qml")
                    var appView = component.createObject(stackView, {"computerIndex": index, "objectName": model.name})
                    stackView.push(appView)
                }
                else {
                    var pin = computerModel.generatePinString()

                    // Kick off pairing in the background
                    computerModel.pairComputer(index, pin)

                    // Display the pairing dialog
                    pairDialog.pin = pin
                    pairDialog.open()
                }
            } else if (!model.online) {
                // Using open() here because it may be activated by keyboard
                pcContextMenu.open()
            }
        }

        onPressAndHold: {
            // popup() ensures the menu appears under the mouse cursor
            if (pcContextMenu.popup) {
                pcContextMenu.popup()
            }
            else {
                // Qt 5.9 doesn't have popup()
                pcContextMenu.open()
            }
        }

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.RightButton;
            onClicked: {
                parent.pressAndHold()
            }
        }

        Keys.onMenuPressed: {
            // We must use open() here so the menu is positioned on
            // the ItemDelegate and not where the mouse cursor is
            pcContextMenu.open()
        }

        Keys.onDeletePressed: {
            deletePcDialog.pcIndex = index
            deletePcDialog.pcName = model.name
            deletePcDialog.open()
        }
    }

    ErrorMessageDialog {
        id: errorDialog

        // Using Setup-Guide here instead of Troubleshooting because it's likely that users
        // will arrive here by forgetting to enable GameStream or not forwarding ports.
        helpUrl: "https://github.com/moonlight-stream/moonlight-docs/wiki/Setup-Guide"
    }

    // VipleStream editorial pairing dialog — big PIN digits in
    // sharp-edged boxes, monospace meta, "waiting" pulse. Replaces
    // the text-only NavigableMessageDialog layout.  External API
    // preserved: set `pairDialog.pin` then `pairDialog.open()`,
    // and PcView.pairingComplete() still calls close().
    NavigableDialog {
        id: pairDialog
        closePolicy: Popup.CloseOnEscape

        property string pin : "0000"
        standardButtons: Dialog.Cancel

        onRejected: {
            // FIXME: We should interrupt pairing here
        }

        // Transparent surround; the content Column provides its own ink2 panel.
        background: Rectangle {
            color: "#14170F"           // vs ink2
            border.color: "#2D3127"    // vs line2
            border.width: 1
            radius: 0
        }

        ColumnLayout {
            spacing: 16

            // Editorial meta header
            RowLayout {
                spacing: 8
                Rectangle { width: 8; height: 8; radius: 1; color: "#D4FF3A" }
                Label {
                    text: "§ 02 · " + qsTr("PAIRING") + "  ·  " + qsTr("STEP 02 / 03")
                    font.family: "IBM Plex Mono"
                    font.pointSize: 9
                    font.letterSpacing: 1.6
                    color: "#D4FF3A"
                }
            }

            Label {
                text: qsTr("Enter PIN on host")
                font.pointSize: 24
                font.bold: true
                font.letterSpacing: -0.8
                color: "#F2F5E1"
            }

            // PIN digit boxes — each digit in its own sharp-edged cell
            Row {
                Layout.alignment: Qt.AlignHCenter
                spacing: 10

                Repeater {
                    // Repeat one cell per character in pin (always 4 for real pairing,
                    // but guard against empty / placeholder strings).
                    model: pairDialog.pin.length > 0 ? pairDialog.pin.length : 4

                    delegate: Rectangle {
                        width: 62
                        height: 90
                        color: index === 1 ? "#1B2000" : "transparent"   // subtle tint on one cell
                        border.color: "#2D3127"
                        border.width: 1
                        radius: 0

                        Label {
                            anchors.centerIn: parent
                            text: pairDialog.pin.length > index ? pairDialog.pin.charAt(index) : "—"
                            font.pointSize: 36
                            font.bold: true
                            font.letterSpacing: -1.5
                            color: "#F2F5E1"
                        }
                    }
                }
            }

            // Host helper + sunshine fallback copy
            Label {
                Layout.maximumWidth: 420
                text: qsTr("If your host PC is running Sunshine, navigate to the Sunshine web UI to enter the PIN.")
                font.pointSize: 10
                color: "#8B8E7E"           // vs mute
                wrapMode: Text.Wrap
            }

            // Waiting pulse
            RowLayout {
                spacing: 6
                Rectangle {
                    width: 6; height: 6; radius: 1
                    color: "#D4FF3A"
                    SequentialAnimation on opacity {
                        loops: Animation.Infinite
                        NumberAnimation { from: 1.0; to: 0.35; duration: 800 }
                        NumberAnimation { from: 0.35; to: 1.0; duration: 800 }
                    }
                }
                Label {
                    text: qsTr("WAITING FOR HOST")
                    font.family: "IBM Plex Mono"
                    font.pointSize: 9
                    font.letterSpacing: 1.4
                    color: "#8B8E7E"
                }
            }
        }
    }

    NavigableMessageDialog {
        id: deletePcDialog
        // don't allow edits to the rest of the window while open
        property int pcIndex : -1
        property string pcName : ""
        text: qsTr("Are you sure you want to remove '%1'?").arg(pcName)
        standardButtons: Dialog.Yes | Dialog.No

        onAccepted: {
            computerModel.deleteComputer(pcIndex)
        }
    }

    NavigableMessageDialog {
        id: testConnectionDialog
        closePolicy: Popup.CloseOnEscape
        standardButtons: Dialog.Ok

        onAboutToShow: {
            testConnectionDialog.text = qsTr("Moonlight is testing your network connection to determine if any required ports are blocked.") + "\n\n" + qsTr("This may take a few seconds…")
            showSpinner = true
        }

        function connectionTestComplete(result, blockedPorts)
        {
            if (result === -1) {
                text = qsTr("The network test could not be performed because none of Moonlight's connection testing servers were reachable from this PC. Check your Internet connection or try again later.")
                imageSrc = "qrc:/res/baseline-warning-24px.svg"
            }
            else if (result === 0) {
                text = qsTr("This network does not appear to be blocking Moonlight. If you still have trouble connecting, check your PC's firewall settings.") + "\n\n" + qsTr("If you are trying to stream over the Internet, install the Moonlight Internet Hosting Tool on your gaming PC and run the included Internet Streaming Tester to check your gaming PC's Internet connection.")
                imageSrc = "qrc:/res/baseline-check_circle_outline-24px.svg"
            }
            else {
                text = qsTr("Your PC's current network connection seems to be blocking Moonlight. Streaming over the Internet may not work while connected to this network.") + "\n\n" + qsTr("The following network ports were blocked:") + "\n"
                text += blockedPorts
                imageSrc = "qrc:/res/baseline-error_outline-24px.svg"
            }

            // Stop showing the spinner and show the image instead
            showSpinner = false
        }
    }

    NavigableDialog {
        id: renamePcDialog
        property string label: qsTr("Enter the new name for this PC:")
        property string originalName
        property int pcIndex : -1;

        standardButtons: Dialog.Ok | Dialog.Cancel

        onOpened: {
            // Force keyboard focus on the textbox so keyboard navigation works
            editText.forceActiveFocus()
        }

        onClosed: {
            editText.clear()
        }

        onAccepted: {
            if (editText.text) {
                computerModel.renameComputer(pcIndex, editText.text)
            }
        }

        ColumnLayout {
            Label {
                text: renamePcDialog.label
                font.bold: true
            }

            TextField {
                id: editText
                placeholderText: renamePcDialog.originalName
                Layout.fillWidth: true
                focus: true

                Keys.onReturnPressed: {
                    renamePcDialog.accept()
                }

                Keys.onEnterPressed: {
                    renamePcDialog.accept()
                }
            }
        }
    }

    NavigableMessageDialog {
        id: showPcDetailsDialog
        property string pcDetails : "";
        text: showPcDetailsDialog.pcDetails
        imageSrc: "qrc:/res/baseline-help_outline-24px.svg"
        standardButtons: Dialog.Ok
    }

    ScrollBar.vertical: ScrollBar {}
}
