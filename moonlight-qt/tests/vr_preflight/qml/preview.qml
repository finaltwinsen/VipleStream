import QtQuick 2.9
import QtQuick.Controls 2.5
import QtQuick.Controls.Material 2.2
import QtQuick.Layouts 1.2

import VrLauncher 1.0

import "../../../app/gui"

// 在開發機上用眼睛看 §VR-PREFLIGHT 的對話框（不是自動測試；檔名不是 tst_ 開頭，qmltestrunner 不會跑它）。
// 頭盔以外的建置上 VR 模式開不起來，真的 GUI 走不到這個對話框；這裡把真的 VrPreflightDialog.qml 放進一個
// 小視窗，VrLauncher 用替身，往返時間照下面選的情境演（每 2 秒一次、連續 3 次夠快就「開始」，跟產品的規則一樣，
// 但這裡是 QML 重寫的示意，產品的規則在 app/backend/vrpreflight.h）。
//
// 執行（會開一個視窗）：
//   $env:QT_QUICK_CONTROLS_STYLE = "Material"; $env:QT_QUICK_CONTROLS_MATERIAL_THEME = "Dark"
//   <Qt>\bin\qml.exe -I moonlight-qt\tests\vr_preflight\qml\imports moonlight-qt\tests\vr_preflight\qml\preview.qml
// 滑鼠＝雷射；鍵盤的左右、Enter、Esc＝手把的左右、A、B。
ApplicationWindow {
    id: window
    visible: true
    width: 1280
    height: 820
    title: "VR preflight dialog preview"

    Material.theme: Material.Dark
    Material.background: "#0D0F0B"
    Material.foreground: "#F2F5E1"
    Material.primary: "#161A12"
    Material.accent: "#D4FF3A"

    // 往返時間的腳本（-1＝量不到），最後一項一直重複
    property var samples: [150]
    property int cursor: 0
    property int streak: 0
    property string lastOutcome: "nothing started yet"

    function nextSample() {
        var value = samples[Math.min(cursor, samples.length - 1)]
        cursor++
        return value
    }

    function start(scenario, checkingMs) {
        if (VrLauncher.preflightChecking || VrLauncher.preflightWaiting) {
            return
        }
        samples = scenario
        cursor = 0
        streak = 0
        lastOutcome = "checking…"
        VrLauncher.setPreflight(true, false, -1, -1, 0)
        checkingTimer.interval = checkingMs
        checkingTimer.restart()
    }

    Timer {
        id: checkingTimer
        onTriggered: {
            if (!VrLauncher.preflightChecking) {
                return
            }
            // 開頭三次的最小值
            var best = -1
            for (var i = 0; i < 3; i++) {
                var v = window.nextSample()
                if (v >= 0 && (best < 0 || v < best)) {
                    best = v
                }
            }
            if (best < 0) {
                window.lastOutcome = "unmeasured: started as before"
                VrLauncher.setPreflight(false, false, -1, -1, 0)
            }
            else if (best <= VrLauncher.preflightThresholdMs) {
                window.lastOutcome = "fast enough (" + best + " ms): started at once"
                VrLauncher.setPreflight(false, false, -1, best, 0)
            }
            else {
                VrLauncher.setPreflight(false, true, best, best, 0)
            }
        }
    }

    Timer {
        interval: VrLauncher.preflightIntervalSec * 1000
        repeat: true
        running: VrLauncher.preflightWaiting
        onTriggered: {
            var v = window.nextSample()
            window.streak = (v >= 0 && v <= VrLauncher.preflightThresholdMs) ? window.streak + 1 : 0
            if (window.streak >= VrLauncher.preflightGoodNeeded) {
                window.lastOutcome = "auto-started after " + VrLauncher.preflightGoodNeeded + " good probes"
                VrLauncher.setPreflight(false, false, VrLauncher.preflightMeasuredMs, v, window.streak)
            }
            else {
                VrLauncher.setPreflight(false, true, VrLauncher.preflightMeasuredMs, v, window.streak)
            }
        }
    }

    Connections {
        target: VrLauncher
        function onStartAnywayCallsChanged() { window.lastOutcome = "the user chose Start anyway" }
        function onCancelCallsChanged() { window.lastOutcome = "the user cancelled" }
    }

    // main.qml 的 stackView（NavigableDialog 關閉時把焦點交給它）
    FocusScope {
        id: stackView
        anchors.fill: parent
        focus: true

        ColumnLayout {
            anchors.centerIn: parent
            spacing: 14

            Label {
                text: "Pick what the connection does, then watch the dialog. Last outcome: " + window.lastOutcome
                Layout.maximumWidth: 900
                wrapMode: Text.Wrap
            }

            Button {
                focus: true
                text: "Relay, stays slow (150 ms) - try both buttons"
                onClicked: window.start([150], 450)
            }
            Button {
                text: "Relay, direct after 10 s (150 x8, then 30) - starts by itself"
                onClicked: window.start([150, 150, 150, 150, 150, 150, 150, 150, 30], 450)
            }
            Button {
                text: "Flapping (150 x4, 30 x2, 150, then 30)"
                onClicked: window.start([150, 150, 150, 150, 30, 30, 150, 30], 450)
            }
            Button {
                text: "Slow, then the host stops answering"
                onClicked: window.start([150, 150, 150, -1], 450)
            }
            Button {
                text: "No answer at all (3 s of checking, then it starts as before)"
                onClicked: window.start([-1], 3000)
            }
            Button {
                text: "Good connection (12 ms) - no dialog"
                onClicked: window.start([12], 12)
            }
        }
    }

    VrPreflightDialog {
        id: vrPreflightDialog
    }
}
