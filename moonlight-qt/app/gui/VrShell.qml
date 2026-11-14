import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Controls.Material 2.2

import ComputerManager 1.0
import VrLauncher 1.0
import VrNetBench 1.0

// §VR-SHELL（2026-10-20）：頭盔 VR 介面的外殼。不是視窗：backend/xrshell.cpp 用 QQuickRenderControl 把它畫在離屏的
// QQuickWindow 裡，讀回成圖片貼到 XR 面板上，控制器射線轉成滑鼠事件送回來。內容與平面視窗共用 AppContent.qml。
//
// 根物件叫 window，提供 AppContent 與各頁面會用到的 window.theme、pollingActive、clearOnBack；平面視窗才有意義的
// show()／hide() 等做成空函式（VR 裡介面一直都在面板上，不必藏）。
Item {
    id: window

    property bool pollingActive: false
    property bool clearOnBack: false
    // §PERF-1113 A21：這個外殼是頭盔 VR 介面（平面視窗 main.qml 沒有這個屬性）。AppContent 在 Component.onCompleted
    // 就要知道——那時 VrLauncher.shellActive 還是 false（要等 XR session 起來才設）
    readonly property bool vrShell: true
    // 介面一直顯示在 XR 面板上
    property bool active: true
    property Theme theme: Theme {}

    // 面板的邏輯尺寸（xrshell.cpp 依它建離屏畫面）
    width: 1280
    height: 800

    Material.theme: Material.Dark
    Material.background: theme.ink
    Material.foreground: theme.paper
    Material.primary: theme.ink2
    Material.accent: theme.lime

    function showMainWindow() {}
    function show() {}
    function hide() {}
    function showMinimized() {}
    function showMaximized() {}
    function showFullScreen() {}
    function raise() {}
    function requestActivate() {}

    Rectangle {
        anchors.fill: parent
        color: window.theme.ink
    }

    AppContent {
        id: appContent
        anchors.fill: parent
        focus: true
    }

    // §VR-NETBENCH（dev，--dev-netbench）：主機一上線就開始網路測試；主機還沒上線時每 5 秒再試，最多 2 分鐘
    Timer {
        property int tries: 0
        interval: 5000
        repeat: true
        running: VrNetBench.devAutoStart
        onTriggered: {
            tries++
            if (VrNetBench.running || VrNetBench.start(VrLauncher, ComputerManager) || tries >= 24) {
                console.log("[VIPLE-VR-NETBENCH] dev auto start: " + (VrNetBench.running ? "started" : "gave up: " + VrNetBench.status))
                stop()
            }
        }
    }

    // §PERF-1113：要不要輪詢主機，所有條件收在這一個函式（ComputerManager 是引用計數，start／stop 一定要成對，
    // 只靠 pollingActive 這一個旗標把關）：
    //   - visible：同一個行程裡的串流（StreamSegue 在連上後設 window.visible = false、結束時設回 true）。平面視窗靠
    //     onVisibleChanged 停輪詢，這裡的根是 Item，原本沒有對應的處理，整場串流每 3 秒一次 HTTPS 握手照跑
    //   - VrLauncher.running：串流子行程或影片在跑（§REVIEW-1111）
    //   - unfocusedLong：XR session 不在前景超過 unfocusedTimer 的時間（頭盔取下、被儀表板蓋住）。
    //     VrLauncher.shellFocused 目前恆為 true（XR 那一側還沒把狀態傳出來），所以這一條現在不會成立
    property bool unfocusedLong: false

    function updatePolling() {
        var want = window.visible && !VrLauncher.running && !window.unfocusedLong
        if (want && !window.pollingActive) {
            // 恢復時會立刻查一次，介面拿到的是最新狀態
            ComputerManager.startPolling()
            window.pollingActive = true
        }
        else if (!want && window.pollingActive) {
            ComputerManager.stopPollingAsync()
            window.pollingActive = false
        }
    }

    Component.onCompleted: updatePolling()
    onVisibleChanged: updatePolling()
    onUnfocusedLongChanged: updatePolling()

    Timer {
        id: unfocusedTimer
        // 開一下儀表板不要馬上停
        interval: 60000
        onTriggered: window.unfocusedLong = true
    }

    // §REVIEW-1111：串流子行程在跑的時候停掉主機輪詢。這段時間頭盔已經交給子行程、介面沒有人在看，
    // 原本卻仍每 3 秒對 host 做一次 HTTPS 查詢（每 30 秒再抓一次 app 清單），和串流用的是同一顆無線電。
    // 子行程結束就恢復（恢復時會立刻查一次，介面拿到的是最新狀態）。
    Connections {
        target: VrLauncher

        function onRunningChanged() {
            window.updatePolling()
        }

        function onShellFocusedChanged() {
            if (VrLauncher.shellFocused) {
                unfocusedTimer.stop()
                window.unfocusedLong = false
            }
            else {
                unfocusedTimer.restart()
            }
        }
    }
}
