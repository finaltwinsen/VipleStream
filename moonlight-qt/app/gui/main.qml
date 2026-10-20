import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.3
import QtQuick.Window 2.2
import QtQuick.Controls.Material 2.2

import ComputerManager 1.0
import AutoUpdateChecker 1.0
import StreamingPreferences 1.0
import SystemProperties 1.0
import SdlGamepadKeyNavigation 1.0
import VrLauncher 1.0

ApplicationWindow {
    property bool pollingActive: false

    // Set by SettingsView to force the back operation to pop all
    // pages except the initial view. This is required when doing
    // a retranslate() because AppView breaks for some reason.
    property bool clearOnBack: false

    id: window
    width: 1280
    // VipleStream: initial height must accommodate the editorial
    // layout's first row without clipping. Upstream 600px was too
    // short because our PcView now has a §01 HOSTS hero banner
    // (~260px) stacked above the tile grid. Breakdown:
    //   toolbar        ~ 80px
    //   hero banner    ~260px
    //   tile row (Bold) ~440px  ← cellHeight in PcView.qml
    //   margins         ~40px
    //   ---------------------
    //   minimum         820px
    // Safe mode only needs ~710px; picking 820 covers the default
    // (Bold) variant at launch. User can still drag smaller — see
    // minimumHeight floor below, which prevents going below the
    // point where the first tile row stops being fully visible.
    height: 820
    minimumWidth: 960    // two Bold tiles + padding without horizontal clip
    minimumHeight: 720   // toolbar + banner + one Safe tile row + margins

    // VipleStream design tokens — palette, fonts, spacing.  Child views
    // access via `window.theme.lime` etc. (see gui/Theme.qml).
    property Theme theme: Theme {}

    // Editorial magazine theme: warm-tinted dark ink background,
    // electric lime accent, off-white "paper" foreground.  Switches
    // every Material-aware control inside the app.
    //
    // IMPORTANT: Material.primary is the paint for chrome surfaces
    // like ToolBar / TabBar backgrounds.  Setting it to lime turns
    // the header bar into a big lime block that swallows the white
    // title text + icons (foreground=paper reads as nearly-white),
    // which is exactly the "unreadable top bar" bug shipped in
    // v1.2.18..v1.2.32.  Keep primary on an ink-toned surface so
    // the header stays dark; lime is reserved for *accent* usage
    // (focus rings, switches, CTA buttons) via Material.accent and
    // for explicit per-element tints (§NN prefixes, live dots).
    Material.theme: Material.Dark
    Material.background: theme.ink
    Material.foreground: theme.paper
    Material.primary: theme.ink2
    Material.accent: theme.lime

    // Show the window according to the user's preferences
    // （§VR-LAUNCHER：VR 串流子行程結束後也用這裡把視窗叫回來）
    function showMainWindow() {
        if (SystemProperties.hasDesktopEnvironment) {
            if (StreamingPreferences.uiDisplayMode == StreamingPreferences.UI_MAXIMIZED) {
                window.showMaximized()
            }
            else if (StreamingPreferences.uiDisplayMode == StreamingPreferences.UI_FULLSCREEN) {
                window.showFullScreen()
            }
            else {
                window.show()
            }
        } else {
            window.showFullScreen()
        }
    }

    Component.onCompleted: {
        showMainWindow()
    }

    // §VR-SHELL：主畫面的內容（gui/AppContent.qml；頭盔 VR 介面的外殼 VrShell.qml 也用同一份）
    AppContent {
        id: appContent
        anchors.fill: parent
        focus: true
    }

    // This timer keeps us polling for 5 minutes of inactivity
    // to allow the user to work with VipleStream on a second display
    // while dealing with configuration issues. This will ensure
    // machines come online even if the input focus isn't on VipleStream.
    Timer {
        id: inactivityTimer
        interval: 5 * 60000
        onTriggered: {
            if (!active && pollingActive) {
                ComputerManager.stopPollingAsync()
                pollingActive = false
            }
        }
    }

    onVisibleChanged: {
        // When we become invisible while streaming is going on,
        // stop polling immediately.
        if (!visible) {
            inactivityTimer.stop()

            if (pollingActive) {
                ComputerManager.stopPollingAsync()
                pollingActive = false
            }
        }
        else if (active) {
            // When we become visible and active again, start polling
            inactivityTimer.stop()

            // Restart polling if it was stopped
            if (!pollingActive) {
                ComputerManager.startPolling()
                pollingActive = true
            }
        }

        // Poll for gamepad input only when the window is in focus
        SdlGamepadKeyNavigation.notifyWindowFocus(visible && active)
    }

    onActiveChanged: {
        if (active) {
            // Stop the inactivity timer
            inactivityTimer.stop()

            // Restart polling if it was stopped
            if (!pollingActive) {
                ComputerManager.startPolling()
                pollingActive = true
            }
        }
        else {
            // Start the inactivity timer to stop polling
            // if focus does not return within a few minutes.
            inactivityTimer.restart()
        }

        // Poll for gamepad input only when the window is in focus
        SdlGamepadKeyNavigation.notifyWindowFocus(visible && active)
    }

    // §VR-LAUNCHER：VR 串流在子行程進行時隱藏主視窗（留著的話 Frame 會把它當平面遊戲、疊上手把模式介面），
    // 子行程結束後叫回來（錯誤對話框在 AppContent）
    Connections {
        target: VrLauncher

        function onRunningChanged() {
            if (VrLauncher.running) {
                SdlGamepadKeyNavigation.disable()
                // 實驗（2026-10-19）：主視窗一藏，頭盔的 SteamVR 就把 Steam 主選單叫出來（"theater frame destroyed"），
                // 蓋在 VR 畫面上 2～4 秒。dev 的 hide＝… 比較幾種讓開的方式
                var mode = VrLauncher.devLaunchHide
                console.warn("[VIPLE-VR-LAUNCHER] child started: window mode=" + (mode === "" ? "now" : mode))
                if (mode === "minimize") {
                    window.showMinimized()
                }
                else if (mode !== "never" && mode !== "focused") {
                    window.hide()
                }
            }
            else {
                showMainWindow()
                // §K.4：有些 WM 在 hide→show 之後不會重新接管視窗、也不給焦點（同 StreamSegue）；沒拿到焦點時
                // 手把導覽與主機輪詢都不會恢復
                window.raise()
                window.requestActivate()
                SdlGamepadKeyNavigation.enable()
            }
        }

        function onChildXrState(state) {
            if (state === "FOCUSED" && VrLauncher.running && VrLauncher.devLaunchHide === "focused") {
                console.warn("[VIPLE-VR-LAUNCHER] child XR focused: hiding the window")
                window.hide()
            }
        }
    }
}
