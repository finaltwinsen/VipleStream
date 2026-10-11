pragma Singleton
import QtQuick 2.9

// QML 測試用的 VrLauncher 替身（真的是 C++：app/backend/vrlauncher.h，由 main.cpp 註冊成單例）。
// 屬性、函式、訊號的名稱跟真的一樣；有沒有對上由 test_check.cpp 拿真的類別的 meta-object 檢查。
// 行為只模擬介面看得到的部分：狀態由測試用 setPreflight() 設定，兩個按鈕的呼叫記下來，
// 並且跟真的一樣在呼叫當下就回到「沒有在檢查」、發 preflightChanged。
QtObject {
    property bool available: true
    property string blockedReason: ""
    property bool vrMode: true
    property bool running: false

    property bool preflightChecking: false
    property bool preflightWaiting: false
    property int preflightMeasuredMs: -1
    property int preflightRttMs: -1
    property int preflightGoodStreak: 0
    property int preflightThresholdMs: 60
    property int preflightGoodNeeded: 3
    property int preflightIntervalSec: 2

    signal preflightChanged()
    signal finished(int exitCode, string error)

    // ---- 給測試看的紀錄 ----
    property var launches: []
    property int startAnywayCalls: 0
    property int cancelCalls: 0
    // true＝launch() 模擬「太慢」：進入等待，測試再決定接下來發生什麼
    property bool slowOnLaunch: false

    function setVrMode(on) {
        if (vrMode === on) {
            return false
        }
        vrMode = on
        return true
    }

    function launch(hostAddress, probeHost, probePort, appName, takeover) {
        if (!vrMode || running || preflightChecking || preflightWaiting || hostAddress === "" || appName === "") {
            return false
        }
        var list = launches
        list.push({"hostAddress": hostAddress, "probeHost": probeHost, "probePort": probePort,
                   "appName": appName, "takeover": takeover})
        launches = list
        if (slowOnLaunch) {
            setPreflight(false, true, 118, 118, 0)
        }
        return true
    }

    function preflightStartAnyway() {
        if (!preflightChecking && !preflightWaiting) {
            return
        }
        startAnywayCalls++
        setPreflight(false, false, preflightMeasuredMs, preflightRttMs, preflightGoodStreak)
    }

    function preflightCancel() {
        if (!preflightChecking && !preflightWaiting) {
            return
        }
        cancelCalls++
        setPreflight(false, false, preflightMeasuredMs, preflightRttMs, preflightGoodStreak)
    }

    // 測試用：設定狀態，跟真的一樣全部設好之後才發一次 preflightChanged
    function setPreflight(checking, waiting, measuredMs, rttMs, goodStreak) {
        preflightChecking = checking
        preflightWaiting = waiting
        preflightMeasuredMs = measuredMs
        preflightRttMs = rttMs
        preflightGoodStreak = goodStreak
        preflightChanged()
    }

    function resetForTest() {
        launches = []
        startAnywayCalls = 0
        cancelCalls = 0
        slowOnLaunch = false
        vrMode = true
        running = false
        setPreflight(false, false, -1, -1, 0)
    }
}
