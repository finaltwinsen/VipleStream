import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Controls.Material 2.2

import AppModel 1.0
import ComputerManager 1.0
import SdlGamepadKeyNavigation 1.0
import StreamingPreferences 1.0

CenteredGridView {
    property int computerIndex
    property AppModel appModel : createModel()
    property bool activated
    property bool showHiddenGames
    property bool showGames

    // Bold variant sizes the library tiles up to cover-story proportions
    // so each box-art reads like a zine poster; Safe keeps the packed
    // 8-column reading grid from §03 Safe GameGrid.
    readonly property bool bold: StreamingPreferences.designVariant == StreamingPreferences.DV_BOLD

    id: appGrid
    focus: true
    activeFocusOnTab: true
    // VipleStream H.4 v1.2.108: leave room at the top for the pinned
    // Steam profile bar (52px) when it's visible.  Without this the
    // first row of app tiles renders directly under the bar.
    topMargin: 20 + (pinnedSteamBar.visible ? 52 : 0)
    bottomMargin: 5
    cellWidth: bold ? 310 : 230
    cellHeight: bold ? 410 : 297

    // VipleStream H.4: while a Steam-account-switch is in flight (server
    // shutting down + relogging Steam, ~10–30 s), suppress all of the
    // bookkeeping that would normally nuke this view:
    //   - app-tile clicks / focus moves
    //   - the computerLost signal that pops AppView (the host tends to
    //     blip CS_OFFLINE for one polling tick because Sunshine's HTTP
    //     thread is busy with /steamswitch)
    // Bound by the busy overlay's `visible`.
    property bool steamSwitchInProgress: false

    function computerLost()
    {
        if (appGrid.steamSwitchInProgress) {
            // Host is *probably* coming back in a moment — Sunshine's
            // HTTP thread is busy spawning steam.exe -shutdown / -login,
            // which can starve one /serverinfo polling tick on a busy
            // box.  Don't tear down the AppView: the next poll will
            // recover.  If the switch genuinely fails, the busy overlay
            // shows the error and the user can back out manually.
            console.log("[H.4] computerLost ignored — switch in progress")
            return
        }
        stackView.pop()
    }

    Component.onCompleted: {
        // Don't show any highlighted item until interacting with them.
        // We do this here instead of onActivated to avoid losing the user's
        // selection when backing out of a different page of the app.
        currentIndex = -1
    }

    StackView.onActivated: {
        appModel.computerLost.connect(computerLost)
        activated = true

        // Highlight the first item if a gamepad is connected
        if (currentIndex === -1 && SdlGamepadKeyNavigation.getConnectedGamepads() > 0) {
            currentIndex = 0
        }

        if (!showGames && !showHiddenGames) {
            // Check if there's a direct launch app
            var directLaunchAppIndex = model.getDirectLaunchAppIndex();
            if (directLaunchAppIndex >= 0) {
                // Start the direct launch app if nothing else is running
                currentIndex = directLaunchAppIndex
                currentItem.launchOrResumeSelectedApp(false)

                // Set showGames so we will not loop when the stream ends
                showGames = true
            }
        }
    }

    StackView.onDeactivating: {
        appModel.computerLost.disconnect(computerLost)
        activated = false
    }

    function createModel()
    {
        var model = Qt.createQmlObject('import AppModel 1.0; AppModel {}', parent, '')
        model.initialize(ComputerManager, computerIndex, showHiddenGames)
        return model
    }

    model: appModel

    // Hero click: launch (or resume) the featured app via the same
    // delegate path a tile click would. The delegate's
    // launchOrResumeSelectedApp() encapsulates the full logic so we
    // just forward to it after moving currentIndex onto the
    // featured row.
    function activateFeatured() {
        var idx = appModel.featuredAppIndex();
        if (idx < 0) return;
        appGrid.currentIndex = idx;
        if (appGrid.currentItem && appGrid.currentItem.launchOrResumeSelectedApp) {
            appGrid.currentItem.launchOrResumeSelectedApp(true);
        } else if (appGrid.currentItem) {
            appGrid.currentItem.clicked();
        }
    }

    // VipleStream H.4 v1.2.108: bar is no longer in the GridView header
    // (which scrolls with content and disappears the moment GridView
    // auto-positions to a non-zero index — e.g. resuming the running
    // app, gamepad-set currentIndex=0 hop, etc.).  See `pinnedSteamBar`
    // declared as a direct CenteredGridView child near the bottom of
    // this file: GridView treats non-delegate non-header children as
    // viewport-anchored siblings rather than scroll-content children,
    // which is the closest thing GridView has to ListView's
    // `headerPositioning: ListView.OverlayHeader`.

    // VipleStream §03 Bold library masthead — matches §01 Bold on
    // PcView (see v1.2.37 commit message). Collapsed on Safe so
    // the existing packed grid is untouched.
    header: Component {
        Rectangle {
            id: libraryBanner
            visible: appGrid.bold && appGrid.count > 0
            width: appGrid.width
            height: visible ? 260 : 0
            color: "#0D0F0B"   // ink

            property string featuredName:
                visible ? appModel.nameAt(appModel.featuredAppIndex()) : ""

            // Responsive breakpoint — see PcView heroBanner.wide for
            // the rationale. Narrow windows collapse the tape and
            // use compact headline size so the banner looks OK in
            // portrait-ish windows and on phones.
            readonly property bool wide: width > 700

            MouseArea {
                anchors.fill: parent
                cursorShape: libraryBanner.featuredName !== "" ? Qt.PointingHandCursor : Qt.ArrowCursor
                onClicked: appGrid.activateFeatured()
                hoverEnabled: true
            }

            Image {
                anchors.fill: parent
                source: "qrc:/res/vs_diag_stripe.svg"
                fillMode: Image.Tile
                sourceSize: Qt.size(24, 24)
                opacity: 0.65
            }
            Rectangle {
                anchors.fill: parent
                gradient: Gradient {
                    GradientStop { position: 0.0; color: "#CC0D0F0B" }
                    GradientStop { position: 0.4; color: "#550D0F0B" }
                    GradientStop { position: 1.0; color: "#CC0D0F0B" }
                }
            }

            Column {
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.leftMargin: 32
                anchors.topMargin: 22
                spacing: 6

                Text {
                    text: "◤ " + qsTr("LIBRARY")
                    font.family: "Space Grotesk"
                    font.pointSize: libraryBanner.wide ? 34 : 24
                    font.bold: true
                    font.letterSpacing: -1.4
                    color: "#D4FF3A"
                }
                Text {
                    // objectName on AppView is the host PC's name —
                    // surface it in the subtitle like the §03 mock.
                    text: (appGrid.objectName || qsTr("HOST")).toUpperCase() +
                          "  ·  " + appGrid.count + " " + qsTr("TITLES")
                    font.family: "IBM Plex Mono"
                    font.pointSize: 10
                    font.letterSpacing: 1.8
                    color: "#8B8E7E"
                }
                // Mono stat strip — running / last-played state.
                Text {
                    property bool running: appModel.getRunningAppId() !== 0
                    text: (running ? qsTr("● SESSION ACTIVE") : qsTr("IDLE")) +
                          "   ·   " +
                          (libraryBanner.featuredName !== ""
                              ? qsTr("TAP HERO TO LAUNCH")
                              : qsTr("NO TITLES"))
                    font.family: "IBM Plex Mono"
                    font.pointSize: 9
                    font.letterSpacing: 1.6
                    color: running ? "#D4FF3A"
                                   : (libraryBanner.featuredName !== "" ? "#F2F5E1" : "#FF5A4E")
                }
            }

            Column {
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                anchors.leftMargin: 32
                anchors.bottomMargin: 20
                spacing: 4
                // Leave room for tape on wide; use full width on narrow.
                width: (libraryBanner.wide ? libraryBanner.width - 380
                                           : libraryBanner.width - 64)

                Text {
                    text: "FEATURED · 01 / " + ("0" + appGrid.count).slice(-2)
                    font.family: "IBM Plex Mono"
                    font.pointSize: 9
                    font.letterSpacing: 1.8
                    color: "#D4FF3A"
                }
                Text {
                    visible: libraryBanner.featuredName !== ""
                    text: libraryBanner.featuredName.toUpperCase()
                    font.family: "Space Grotesk"
                    font.pointSize: libraryBanner.wide ? 46 : 28
                    font.bold: true
                    font.letterSpacing: libraryBanner.wide ? -2.4 : -1.2
                    color: "#F2F5E1"
                    width: parent.width
                    elide: Text.ElideRight
                }
                Text {
                    visible: libraryBanner.featuredName === ""
                    text: qsTr("PICK A TITLE TO LAUNCH")
                    font.family: "IBM Plex Mono"
                    font.pointSize: 11
                    font.letterSpacing: 2.0
                    color: "#F2F5E1"
                }
            }

            // Rotated tape — wide-only so narrow / portrait windows
            // don't get a broken-looking clipped sticker.
            Rectangle {
                visible: libraryBanner.wide
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
                    text: "LAUNCH · TO · STREAM"
                    font.family: "IBM Plex Mono"
                    font.pointSize: 11
                    font.bold: true
                    font.letterSpacing: 3.0
                    color: "#0D0F0B"
                }
            }

            Rectangle {
                visible: libraryBanner.wide
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
        width: appGrid.bold ? 300 : 220
        height: appGrid.bold ? 400 : 287
        grid: appGrid

        property alias appContextMenu: appContextMenuLoader.item
        property alias appNameText: appNameTextLoader.item

        // Dim the app if it's hidden
        opacity: model.hidden ? 0.4 : 1.0

        // Bold-only outer stripe frame: fills the entire delegate
        // cell so the 10dp margin around the box art shows an ink
        // stripe pattern, giving every tile (boxart or not) the §03
        // Bold magazine frame. Real box art still covers the centre
        // unchanged.
        Rectangle {
            visible: appGrid.bold && !appIcon.isPlaceholder
            anchors.fill: parent
            color: "#0D0F0B"   // ink

            Image {
                anchors.fill: parent
                source: "qrc:/res/vs_diag_stripe_overlay.svg"
                fillMode: Image.Tile
                sourceSize: Qt.size(24, 24)
                opacity: 0.35
            }
        }

        // VipleStream: when the box art is the generic GFE / fallback
        // placeholder AND we're in Bold, replace the boring grey
        // placeholder with the editorial coloured diagonal-stripe card
        // from the mock. Each tile picks its own hue from its index
        // so the library feels like a magazine spread.
        Rectangle {
            id: placeholderStripe
            visible: appIcon.isPlaceholder && appGrid.bold
            anchors.horizontalCenter: parent.horizontalCenter
            y: 10
            width: appGrid.bold ? 280 : 200
            height: appGrid.bold ? 373 : 267
            color: Qt.hsla(((index * 53) % 360) / 360.0, 0.35, 0.22, 1.0)

            Image {
                anchors.fill: parent
                source: "qrc:/res/vs_diag_stripe_overlay.svg"
                fillMode: Image.Tile
                sourceSize: Qt.size(24, 24)
            }

            // Tiny lime "§NN" folio top-right of the card.
            Text {
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.margins: 8
                text: "§ " + (index < 9 ? "0" + (index + 1) : (index + 1))
                font.family: "IBM Plex Mono"
                font.pointSize: 9
                font.letterSpacing: 1.4
                color: "#D4FF3A"
            }
        }

        Image {
            property bool isPlaceholder: false

            // Hide the raw placeholder image when the striped card is
            // taking its place — the appNameTextLoader still draws
            // the title on top.
            visible: !placeholderStripe.visible

            id: appIcon
            anchors.horizontalCenter: parent.horizontalCenter
            y: 10
            source: model.boxart

            onSourceSizeChanged: {
                // Nearly all of Nvidia's official box art does not match the dimensions of placeholder
                // images, however the one known exception is Overcooked. Therefore, we only execute
                // the image size checks if this is not an app collector game. We know the officially
                // supported games all have box art, so this check is not required.
                if (!model.isAppCollectorGame &&
                    ((sourceSize.width === 130 && sourceSize.height === 180) || // GFE 2.0 placeholder image
                     (sourceSize.width === 628 && sourceSize.height === 888) || // GFE 3.0 placeholder image
                     (sourceSize.width === 200 && sourceSize.height === 266)))  // Our no_app_image.png
                {
                    isPlaceholder = true
                }
                else
                {
                    isPlaceholder = false
                }

                // Bold scales the box-art ~35% bigger to match the
                // roomier cell; Safe keeps the packed grid proportions.
                width  = appGrid.bold ? 280 : 200
                height = appGrid.bold ? 373 : 267
            }

            // Display a tooltip with the full name if it's truncated
            ToolTip.text: model.name
            ToolTip.delay: 1000
            ToolTip.timeout: 5000
            ToolTip.visible: (parent.hovered || parent.highlighted) && (!appNameText || appNameText.truncated)
        }

        Loader {
            active: model.running
            asynchronous: true
            anchors.fill: appIcon

            sourceComponent: Item {
                RoundButton {
                    // Don't steal focus from the toolbar buttons
                    focusPolicy: Qt.NoFocus

                    anchors.horizontalCenterOffset: appIcon.isPlaceholder ? -47 : 0
                    anchors.verticalCenterOffset: appIcon.isPlaceholder ? -75 : -60
                    anchors.centerIn: parent
                    implicitWidth: 85
                    implicitHeight: 85

                    icon.source: "qrc:/res/play_arrow_FILL1_wght700_GRAD200_opsz48.svg"
                    icon.width: 75
                    icon.height: 75

                    onClicked: {
                        launchOrResumeSelectedApp(true)
                    }

                    ToolTip.text: qsTr("Resume Game")
                    ToolTip.delay: 1000
                    ToolTip.timeout: 3000
                    ToolTip.visible: hovered

                    Material.background: "#D0808080"
                }

                RoundButton {
                    // Don't steal focus from the toolbar buttons
                    focusPolicy: Qt.NoFocus

                    anchors.horizontalCenterOffset: appIcon.isPlaceholder ? 47 : 0
                    anchors.verticalCenterOffset: appIcon.isPlaceholder ? -75 : 60
                    anchors.centerIn: parent
                    implicitWidth: 85
                    implicitHeight: 85

                    icon.source: "qrc:/res/stop_FILL1_wght700_GRAD200_opsz48.svg"
                    icon.width: 75
                    icon.height: 75

                    onClicked: {
                        doQuitGame()
                    }

                    ToolTip.text: qsTr("Quit Game")
                    ToolTip.delay: 1000
                    ToolTip.timeout: 3000
                    ToolTip.visible: hovered

                    Material.background: "#D0808080"
                }
            }
        }

        Loader {
            id: appNameTextLoader
            active: appIcon.isPlaceholder

            // This loader is not asynchronous to avoid noticeable differences
            // in the time in which the text loads for each game.

            width: appIcon.width
            height: model.running ? 175 : appIcon.height

            anchors.left: appIcon.left
            anchors.right: appIcon.right
            anchors.bottom: appIcon.bottom

            sourceComponent: Column {
                id: appNameText
                // Column positions children; expose `truncated` so the outer
                // Image's ToolTip trigger still works (binds to appNameText.truncated).
                property bool truncated: gameNameLabel.truncated

                spacing: 4
                leftPadding: 16
                rightPadding: 16
                topPadding: appNameTextLoader.height * 0.32  // push towards centre

                Label {
                    // Editorial meta above the title: "§ NN · LIBRARY"
                    text: "§ " + (index < 9 ? "0" + (index + 1) : (index + 1)) + " · " + qsTr("LIBRARY")
                    font.family: "IBM Plex Mono"
                    font.pointSize: 9
                    font.letterSpacing: 1.2
                    color: "#8B8E7E"    // vs mute
                    width: appNameTextLoader.width - 32
                    horizontalAlignment: Text.AlignHCenter
                }

                Label {
                    id: gameNameLabel
                    text: model.name
                    font.pointSize: 18
                    font.bold: true
                    font.letterSpacing: -0.3
                    color: "#F2F5E1"    // vs paper
                    width: appNameTextLoader.width - 32
                    verticalAlignment: Text.AlignVCenter
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    elide: Text.ElideRight
                }
            }
        }

        // Editorial "● PLAYING" badge — lime block in the top-right corner,
        // visible whenever the game is currently running.
        Rectangle {
            id: playingBadge
            visible: model.running
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: 10
            z: 5
            width: playingLabel.implicitWidth + 14
            height: playingLabel.implicitHeight + 6
            color: "#D4FF3A"    // vs lime
            radius: 0

            Label {
                id: playingLabel
                anchors.centerIn: parent
                text: "● " + qsTr("PLAYING")
                font.family: "IBM Plex Mono"
                font.pointSize: 8
                font.bold: true
                font.letterSpacing: 1.4
                color: "#1A2300"    // vs lime-ink
            }
        }

        function launchOrResumeSelectedApp(quitExistingApp)
        {
            var runningId = appModel.getRunningAppId()
            if (runningId !== 0 && runningId !== model.appid) {
                if (quitExistingApp) {
                    quitAppDialog.appName = appModel.getRunningAppName()
                    quitAppDialog.segueToStream = true
                    quitAppDialog.nextAppName = model.name
                    quitAppDialog.nextAppIndex = index
                    quitAppDialog.open()
                }

                return
            }

            var component = Qt.createComponent("StreamSegue.qml")
            var segue = component.createObject(stackView, {
                                                   "appName": model.name,
                                                   "session": appModel.createSessionForApp(index),
                                                   "isResume": runningId === model.appid
                                               })
            stackView.push(segue)
        }

        onClicked: {
            // Only allow clicking on the box art for non-running games.
            // For running games, buttons will appear to resume or quit which
            // will handle starting the game and clicks on the box art will
            // be ignored.
            if (!model.running) {
                launchOrResumeSelectedApp(true)
            }
        }

        onPressAndHold: {
            // popup() ensures the menu appears under the mouse cursor
            if (appContextMenu.popup) {
                appContextMenu.popup()
            }
            else {
                // Qt 5.9 doesn't have popup()
                appContextMenu.open()
            }
        }

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.RightButton;
            onClicked: {
                parent.pressAndHold()
            }
        }

        Keys.onReturnPressed: {
            // Open the app context menu if activated via the gamepad or keyboard
            // for running games. If the game isn't running, the above onClicked
            // method will handle the launch.
            if (model.running) {
                // This will be keyboard/gamepad driven so use
                // open() instead of popup()
                appContextMenu.open()
            }
        }

        Keys.onEnterPressed: {
            // Open the app context menu if activated via the gamepad or keyboard
            // for running games. If the game isn't running, the above onClicked
            // method will handle the launch.
            if (model.running) {
                // This will be keyboard/gamepad driven so use
                // open() instead of popup()
                appContextMenu.open()
            }
        }

        Keys.onMenuPressed: {
            // This will be keyboard/gamepad driven so use open() instead of popup()
            appContextMenu.open()
        }

        function doQuitGame() {
            quitAppDialog.appName = appModel.getRunningAppName()
            quitAppDialog.segueToStream = false
            quitAppDialog.open()
        }

        Loader {
            id: appContextMenuLoader
            asynchronous: true
            sourceComponent: NavigableMenu {
                id: appContextMenu
                initiator: appContextMenuLoader.parent
                NavigableMenuItem {
                    text: model.running ? qsTr("Resume Game") : qsTr("Launch Game")
                    onTriggered: launchOrResumeSelectedApp(true)
                }
                NavigableMenuItem {
                    text: qsTr("Quit Game")
                    onTriggered: doQuitGame()
                    visible: model.running
                }
                NavigableMenuItem {
                    checkable: true
                    checked: model.directLaunch
                    text: qsTr("Direct Launch")
                    onTriggered: appModel.setAppDirectLaunch(model.index, !model.directLaunch)
                    enabled: !model.hidden

                    ToolTip.text: qsTr("Launch this app immediately when the host is selected, bypassing the app selection grid.")
                    ToolTip.delay: 1000
                    ToolTip.timeout: 3000
                    ToolTip.visible: hovered
                }
                NavigableMenuItem {
                    checkable: true
                    checked: model.hidden
                    text: qsTr("Hide Game")
                    onTriggered: appModel.setAppHidden(model.index, !model.hidden)
                    enabled: model.hidden || (!model.running && !model.directLaunch)

                    ToolTip.text: qsTr("Hide this game from the app grid. To access hidden games, right-click on the host and choose %1.").arg(qsTr("View All Apps"))
                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                }
            }
        }
    }

    // Editorial empty-state: monospace meta + paper display headline.
    Column {
        anchors.centerIn: parent
        spacing: 8
        visible: appGrid.count === 0

        Label {
            text: "§ 03 · " + qsTr("LIBRARY") + " / " + qsTr("EMPTY")
            font.family: "IBM Plex Mono"
            font.pointSize: 10
            font.letterSpacing: 1.4
            color: "#D4FF3A"    // vs lime
        }

        Label {
            text: qsTr("This computer doesn't seem to have any applications or some applications are hidden")
            font.pointSize: 16
            color: "#F2F5E1"    // vs paper
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            width: 420
        }
    }

    NavigableMessageDialog {
        id: quitAppDialog
        property string appName : ""
        property bool segueToStream : false
        property string nextAppName: ""
        property int nextAppIndex: 0
        text:qsTr("Are you sure you want to quit %1? Any unsaved progress will be lost.").arg(appName)
        standardButtons: Dialog.Yes | Dialog.No

        function quitApp() {
            var component = Qt.createComponent("QuitSegue.qml")
            var params = {"appName": appName, "quitRunningAppFn": function() { appModel.quitRunningApp() }}
            if (segueToStream) {
                // Store the session and app name if we're going to stream after
                // successfully quitting the old app.
                params.nextAppName = nextAppName
                params.nextSession = appModel.createSessionForApp(nextAppIndex)
            }
            else {
                params.nextAppName = null
                params.nextSession = null
            }

            stackView.push(component.createObject(stackView, params))
        }

        onAccepted: quitApp()
    }

    ScrollBar.vertical: ScrollBar {}

    // ─────────────────────────────────────────────────────────────────────
    // VipleStream H.4 v1.2.108: pinned Steam profile bar + switch overlay.
    //
    // These two Rectangles are direct children of the GridView root.
    // Qt's Flickable parents declared root-level non-delegate children
    // to the Flickable proper (NOT contentItem), so they sit on the
    // viewport and don't scroll with the app grid.  This is the closest
    // thing GridView has to ListView's `headerPositioning: OverlayHeader`.
    // ─────────────────────────────────────────────────────────────────────

    Rectangle {
        id: pinnedSteamBar
        z: 100  // above the grid + scroll bar
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: visible ? 52 : 0
        visible: false
        color: "#1A2300"          // lime_ink
        border.color: "#D4FF3A"   // lime
        border.width: 2
        enabled: !appGrid.steamSwitchInProgress

        Row {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: 32
            anchors.rightMargin: 32
            spacing: 12

            Rectangle {
                width: 10; height: 10; radius: 1
                color: "#D4FF3A"
                anchors.verticalCenter: parent.verticalCenter
            }
            Text {
                text: "STEAM PROFILE"
                font.family: "IBM Plex Mono"
                font.pointSize: 11
                font.bold: true
                font.letterSpacing: 1.8
                color: "#D4FF3A"
                anchors.verticalCenter: parent.verticalCenter
            }

            ComboBox {
                id: steamProfileCombo
                width: 240
                anchors.verticalCenter: parent.verticalCenter
                model: ListModel { id: steamProfileModel }
                textRole: "persona"
                valueRole: "account"
                property bool suppressActivated: false

                onActivated: {
                    if (suppressActivated) return
                    var account = currentValue
                    if (!account) return
                    console.log("[H.4] switch start: account=" + account)
                    appGrid.steamSwitchInProgress = true
                    var ok = appModel.requestSteamSwitch(account)
                    appGrid.steamSwitchInProgress = false
                    console.log("[H.4] switch result: ok=" + ok
                                + " error=" + appModel.lastSteamSwitchError())
                    if (!ok) {
                        console.warn("[H.4] switch failed:",
                                     appModel.lastSteamSwitchError())
                    }
                    // Re-populate either way: success → reflects new
                    // current; failure → roll back highlight to current.
                    pinnedSteamBar.populate()
                }
            }
        }

        function populate() {
            var peerOK = appModel.peerSupportsSteamProfiles()
            if (!peerOK) {
                pinnedSteamBar.visible = false
                return
            }
            var ok = appModel.refreshSteamProfiles()
            var n  = appModel.steamProfileCount()
            console.log("[H.4] populate: peerOK=" + peerOK
                        + " refreshOK=" + ok
                        + " profileCount=" + n
                        + " lastError=" + appModel.lastSteamSwitchError())
            steamProfileModel.clear()
            for (var i = 0; i < n; i++) {
                steamProfileModel.append({
                    "persona":  appModel.steamProfilePersona(i),
                    "account":  appModel.steamProfileAccount(i),
                    "switchable": appModel.steamProfileSwitchable(i)
                })
            }
            var cur = appModel.currentSteamProfileIndex()
            steamProfileCombo.suppressActivated = true
            steamProfileCombo.currentIndex = (cur >= 0) ? cur : 0
            steamProfileCombo.suppressActivated = false
            pinnedSteamBar.visible = (n > 0)
        }

        Component.onCompleted: populate()

        Connections {
            target: appModel
            function onPeerIsVipleChanged() { pinnedSteamBar.populate() }
            // v1.2.119: receive live progress from server-side async
            // task. Drives switchBusyOverlay's progressLabel — gives
            // the user real "Shutting down… / Logging in…" feedback
            // instead of staring at a spinner for 9-90 s.
            function onSteamSwitchProgress(state, message) {
                switchBusyOverlay.progressState   = state
                switchBusyOverlay.progressMessage = message
            }
        }
    }

    // Busy overlay — covers the entire viewport while a switch is in
    // flight.  Eats clicks (so app tiles can't be launched mid-switch),
    // shows a spinner + status text, and dismisses when
    // steamSwitchInProgress flips back to false.
    Rectangle {
        id: switchBusyOverlay
        z: 200  // above pinnedSteamBar + grid
        anchors.fill: parent
        visible: appGrid.steamSwitchInProgress
        color: "#CC0D0F0B"  // 80 % black on top of the page bg

        // v1.2.119: live progress from server async task.  Initialised
        // empty so the overlay reverts to the static helper text on
        // first open; populated by onSteamSwitchProgress after the
        // first /steamswitch/status reply.
        property string progressState:   ""
        property string progressMessage: ""

        // Reset when the overlay closes so a stale state from the last
        // switch doesn't flash on the next open.
        onVisibleChanged: {
            if (!visible) {
                progressState   = ""
                progressMessage = ""
            }
        }

        // Eat clicks
        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            onClicked: {/* swallow */}
        }

        Column {
            anchors.centerIn: parent
            spacing: 14

            BusyIndicator {
                running: switchBusyOverlay.visible
                anchors.horizontalCenter: parent.horizontalCenter
                width: 56; height: 56
            }
            Text {
                text: qsTr("Switching Steam account…")
                font.family: "IBM Plex Mono"
                font.pointSize: 14
                font.bold: true
                font.letterSpacing: 1.6
                color: "#D4FF3A"
                anchors.horizontalCenter: parent.horizontalCenter
            }
            Text {
                // Live message from server when available, otherwise
                // the static helper text.
                text: switchBusyOverlay.progressMessage.length > 0
                      ? switchBusyOverlay.progressMessage
                      : qsTr("Steam will close and re-login on the host.\nThis can take 60–90 seconds on a typical box.\nIf it stalls longer, check the host — Steam Guard 2FA may be prompting.")
                font.family: "Inter"
                font.pointSize: 11
                color: "#8B8E7E"
                horizontalAlignment: Text.AlignHCenter
                anchors.horizontalCenter: parent.horizontalCenter
            }
            Text {
                visible: switchBusyOverlay.progressState.length > 0
                text: "[" + switchBusyOverlay.progressState + "]"
                font.family: "IBM Plex Mono"
                font.pointSize: 9
                font.letterSpacing: 1.2
                color: "#5C6052"
                anchors.horizontalCenter: parent.horizontalCenter
            }
        }
    }
}
