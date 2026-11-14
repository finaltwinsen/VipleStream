import QtQuick 2.0
import QtQuick.Controls 2.2
import QtQuick.Window 2.2

import SdlGamepadKeyNavigation 1.0
import Session 1.0
import SystemProperties 1.0
import VrLauncher 1.0   // §VR-EXIT-COMBO 的提示用；漏了會在 onLoaded 丟例外，session 根本不會開始

Item {
    property Session session
    property string appName
    property string stageText : isResume ? qsTr("Resuming %1...").arg(appName) :
                                           qsTr("Starting %1...").arg(appName)
    property bool isResume : false
    property bool quitAfter : false

    function stageStarting(stage)
    {
        // Update the spinner text
        stageText = qsTr("Starting %1...").arg(stage)
    }

    function stageFailed(stage, errorCode, failingPorts)
    {
        // Display the error dialog after Session::exec() returns
        streamSegueErrorDialog.text = qsTr("Starting %1 failed: Error %2").arg(stage).arg(errorCode)

        if (failingPorts) {
            streamSegueErrorDialog.text += "\n\n" + qsTr("Check your firewall and port forwarding rules for port(s): %1").arg(failingPorts)
        }
    }

    function connectionStarted()
    {
        // Hide the UI contents so the user doesn't
        // see them briefly when we pop off the StackView
        stageSpinner.visible = false
        stageLabel.visible = false
        hintText.visible = false

        // Hide the window now that streaming has begun
        window.visible = false
    }

    function displayLaunchError(text)
    {
        // Display the error dialog after Session::exec() returns
        streamSegueErrorDialog.text = text
        console.error(text)
    }

    function quitStarting()
    {
        // Avoid the push transition animation
        var component = Qt.createComponent("QuitSegue.qml")
        stackView.replace(stackView.currentItem, component.createObject(stackView, {"appName": appName}), StackView.Immediate)

        // Show the Qt window again to show quit segue
        window.visible = true
        // VipleStream §K.4 GUI fix:On X11/Linux some WMs (verified on Ubuntu
        // 26.04 GNOME Shell + Hyper-V virtio-gpu) don't restore the window
        // to the taskbar / Alt-Tab list after a hide → show cycle —— it
        // remains "withdrawn" from the WM's view even though Qt thinks
        // visible=true.  raise() + requestActivate() force the WM to
        // re-recognize it and place it on top with focus.
        window.raise()
        window.requestActivate()
    }

    function sessionFinished(portTestResult)
    {
        if (portTestResult !== 0 && portTestResult !== -1 && streamSegueErrorDialog.text) {
            streamSegueErrorDialog.text += "\n\n" + qsTr("This PC's Internet connection is blocking VipleStream. Streaming over the Internet may not work while connected to this network.")
        }

        // Re-enable GUI gamepad usage now
        // §PERF-1113 A31：下面兩條「馬上結束行程」的路（命令列串流正常結束、C++ 驅動的命令列串流出錯）不必再把
        // 手把子系統開起來——開了也是立刻被關掉。條件和下面的 if／else if 一致
        var exitingNow = quitAfter && (!streamSegueErrorDialog.text ||
                                       (typeof cliStreamSession !== "undefined" && cliStreamSession))
        if (!exitingNow) {
            SdlGamepadKeyNavigation.enable()
        }

        // Pop the StreamSegue off the stack if this is a GUI-based app launch
        if (!quitAfter) {
            stackView.pop()
        }

        if (quitAfter && !streamSegueErrorDialog.text) {
            // If this was a CLI launch without errors, exit now
            Qt.quit()
        }
        else if (quitAfter && typeof cliStreamSession !== "undefined" && cliStreamSession) {
            // VipleStream M4a：C++ 驅動的 CLI stream（main.cpp StreamRequested）——錯誤已由 main.cpp 印到
            // stderr；不開等人按確定的對話框（無頭 offscreen／XR 下沒人能按），以非零結束碼退出
            console.error(streamSegueErrorDialog.text)
            Qt.exit(1)
        }
        else {
            // Show the Qt window again after streaming
            window.visible = true
            // VipleStream §K.4 GUI fix: see quitStarting() comment —
            // some Linux WMs (X11) drop window from taskbar / Alt-Tab
            // after hide → show cycle.  Force WM re-attach + focus.
            window.raise()
            window.requestActivate()

            // Display any launch errors. We do this after
            // the Qt UI is visible again to prevent losing
            // focus on the dialog which would impact gamepad
            // users.
            if (streamSegueErrorDialog.text) {
                streamSegueErrorDialog.quitAfter = quitAfter
                streamSegueErrorDialog.open()
            }
        }
    }

    function sessionReadyForDeletion()
    {
        // Garbage collect the Session object since it's pretty heavyweight
        // and keeps other libraries (like SDL_TTF) around until it is deleted.
        session = null
        gc()
    }

    StackView.onDeactivating: {
        // Show the toolbar again when popped off the stack
        toolBar.visible = true

        // Re-enable GUI gamepad usage now
        SdlGamepadKeyNavigation.enable()
    }

    StackView.onActivated: {
        // Hide the toolbar before we start loading
        toolBar.visible = false

        // Hook up our signals
        session.stageStarting.connect(stageStarting)
        session.stageFailed.connect(stageFailed)
        session.connectionStarted.connect(connectionStarted)
        session.displayLaunchError.connect(displayLaunchError)
        session.quitStarting.connect(quitStarting)
        session.sessionFinished.connect(sessionFinished)
        session.readyForDeletion.connect(sessionReadyForDeletion)

        // Ensure the SystemProperties async thread is finished,
        // since it may currently be using the SDL video subsystem
        SystemProperties.waitForAsyncLoad()

        // Kick off the stream
        spinnerTimer.start()
        streamLoader.active = true
    }

    Timer {
        id: spinnerTimer

        // Display the spinner appearance a bit to allow us to reach
        // the code in Session.exec() that pumps the event loop.
        // If we display it immediately, it will briefly hang in the
        // middle of the animation on Windows, which looks very
        // obviously broken.
        interval: 100
        onTriggered: stageSpinner.visible = true
    }

    Timer {
        id: startSessionTimer
        onTriggered: {
            // Garbage collect QML stuff before we start streaming,
            // since we'll probably be streaming for a while and we
            // won't be able to GC during the stream.
            gc()

            // Run the streaming session to completion
            session.start()
        }
    }

    Loader {
        id: streamLoader
        active: false
        asynchronous: true

        onLoaded: {
            // Set the hint text. We do this here rather than
            // in the hintText control itself to synchronize
            // with Session.exec() which requires no concurrent
            // gamepad usage.
            // §TIPS（2026-11-12）：原本只有「怎麼結束串流」一句。改成每次轉場從一份清單隨機挑一條，
            // 清單依這台裝置有什麼（頭盔控制器／手把／鍵盤）組出來；結束串流那一條出現的機會加倍。
            var quitTip = qsTr("Press %1 to disconnect your session").arg(SdlGamepadKeyNavigation.getConnectedGamepads() > 0 ?
                                                  qsTr("Start+Select+L1+R1") : qsTr("Ctrl+Alt+Shift+Q"))
            var tips = [quitTip, quitTip]
            if (VrLauncher.available) {
                // §VR-EXIT-COMBO：頭盔上（XR 桌面與 VR 串流）用 VR 控制器結束
                tips = [qsTr("VR controller: hold A+X+trigger+grip to end the stream"),
                        qsTr("VR controller: hold A+X+trigger+grip to end the stream"),
                        qsTr("Flat-screen stream: press the left menu button to open or close the virtual keyboard"),
                        qsTr("Flat-screen stream: hold the left menu button to bring the screen back in front of you"),
                        qsTr("Flat-screen stream: trigger = left click, grip = right click, thumbstick up/down = scroll"),
                        qsTr("VR stream: hold menu+trigger for one second to open the SteamVR dashboard on the PC"),
                        qsTr("VR stream: hold menu+grip on one hand for one second to open or close the virtual keyboard"),
                        qsTr("Hold both menu buttons for 2 seconds to switch Xbox controller mode (flat-screen and VR streams)"),
                        qsTr("Hold both thumbsticks down for 1.5 seconds to switch steering wheel mode (in Xbox controller mode)"),
                        qsTr("VR stream: games without VR controller support get an Xbox controller automatically")]
            }
            else {
                tips.push(qsTr("Press %1 to show or hide the performance overlay").arg(qsTr("Ctrl+Alt+Shift+S")))
                tips.push(qsTr("Press %1 to switch between windowed and full-screen").arg(qsTr("Ctrl+Alt+Shift+X")))
                tips.push(qsTr("Press %1 to release or capture the mouse and keyboard").arg(qsTr("Ctrl+Alt+Shift+Z")))
                tips.push(qsTr("Press %1 to switch the mouse between game mode and remote desktop mode").arg(qsTr("Ctrl+Alt+Shift+M")))
                tips.push(qsTr("Press %1 to type the clipboard text on the host").arg(qsTr("Ctrl+Alt+Shift+V")))
                if (SdlGamepadKeyNavigation.getConnectedGamepads() > 0) {
                    tips.push(qsTr("Gamepad: hold Start to use the gamepad as a mouse"))
                }
            }
            hintText.text = qsTr("Tip:") + " " + tips[Math.floor(Math.random() * tips.length)]

            // Stop GUI gamepad usage now
            SdlGamepadKeyNavigation.disable()

            // Initialize the session and probe for host/client capabilities
            if (!session.initialize(window)) {
                // §REVIEW-1104：initialize() 失敗卻沒寫錯誤文字時，quitAfter 會當成正常結束（結束碼 0），CLI／VR 子行程也不報錯
                if (!streamSegueErrorDialog.text) {
                    displayLaunchError(qsTr("Failed to start the stream."));
                }
                sessionFinished(0);
                sessionReadyForDeletion();
                return;
            }

            // Don't wait unless we have toasts to display
            startSessionTimer.interval = 0

            // §PERF-1113 A24：Qt 用 offscreen（頭盔的 VR／XR 桌面子行程）時沒有任何人看得到提示：警告照樣寫進 log，
            // 不建提示、也不為它多等 3.5 秒才開始串流。不能用 window.visible 判斷（offscreen 下也是 true）
            var headless = Qt.platform.pluginName === "offscreen"

            // Display the toasts together in a vertical centered arrangement
            var yOffset = 0
            for (var i = 0; i < session.launchWarnings.length; i++) {
                var text = session.launchWarnings[i]
                console.warn(text)
                if (headless) {
                    continue
                }

                // Show the tooltip for 3 seconds
                var toast = Qt.createQmlObject('import QtQuick.Controls 2.2; ToolTip {}', parent, '')
                toast.timeout = 3000
                toast.text = text
                toast.y += yOffset
                toast.visible = true

                // Offset the next toast below the previous one
                yOffset = toast.y + toast.padding + toast.height

                // Allow an extra 500 ms for the tooltip's fade-out animation to finish
                startSessionTimer.interval = toast.timeout + 500;
            }

            // Start the timer to wait for toasts (or start the session immediately)
            startSessionTimer.start()
        }

        sourceComponent: Item {}
    }

    Row {
        anchors.centerIn: parent
        spacing: 5

        BusyIndicator {
            id: stageSpinner
            running: visible
            visible: false
        }

        Label {
            id: stageLabel
            height: stageSpinner.height
            text: stageText
            font.pointSize: 20
            verticalAlignment: Text.AlignVCenter

            wrapMode: Text.Wrap
        }
    }

    Label {
        id: hintText
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 50
        anchors.horizontalCenter: parent.horizontalCenter
        font.pointSize: 18
        verticalAlignment: Text.AlignVCenter
        horizontalAlignment: Text.AlignHCenter

        wrapMode: Text.Wrap
    }
}
