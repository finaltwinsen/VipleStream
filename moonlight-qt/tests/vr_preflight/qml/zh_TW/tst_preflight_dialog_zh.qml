import QtQuick 2.9
import QtQuick.Controls 2.5
import QtTest 1.2

import VrLauncher 1.0

import "../../../../app/gui"

// §VR-PREFLIGHT 對話框的 zh_TW 版：要帶 -translation <app/languages/qml_zh_TW.qm> 執行（見 ../../run_tests.ps1）。
// 驗的是真的 VrPreflightDialog.qml 經由真的 .qm 顯示出來的字，以及換成中文之後版面沒有壞。
// 放在自己的目錄：沒帶翻譯檔跑整個 qml 目錄時不會把這一個也帶進去。
Item {
    id: root
    width: 1280
    height: 800

    FocusScope {
        id: stackView
        anchors.fill: parent
        focus: true
    }

    VrPreflightDialog {
        id: dialog
    }

    TestCase {
        name: "VrPreflightDialogZhTW"
        when: windowShown

        function part(name) {
            var o = findChild(dialog, name)
            verify(o, "missing " + name)
            return o
        }

        function init() {
            VrLauncher.resetForTest()
            tryCompare(dialog, "visible", false, 2000)
        }

        function test_slow_text() {
            VrLauncher.setPreflight(false, true, 104, 131, 0)
            tryCompare(dialog, "opened", true, 2000)
            compare(dialog.title, "連線太慢，不適合 VR")
            compare(part("headline").text, "目前到主機的連線太慢，不適合 VR。現在開始的話畫面會破圖，而且明顯跟不上頭部的動作。")
            compare(part("measured").text, "量到的往返時間：104 ms。VR 需要 60 ms 以下。")
            compare(part("cause").text, "這通常是因為 VPN 改經中繼轉送，沒有直接連線。")
            compare(part("status").text, "每 2 秒重新檢查一次。連線夠快時會自動開始 VR。")
            compare(part("latest").text, "最新：131 ms")
            compare(part("streak").text, "已連續 0／3 次夠快")
            compare(part("startButton").text, "仍要開始")
            compare(part("cancelButton").text, "取消")

            VrLauncher.setPreflight(false, true, 104, -1, 0)
            compare(part("latest").text, "最新：主機沒有回應")
            VrLauncher.setPreflight(false, true, 104, 30, 2)
            compare(part("latest").text, "最新：30 ms")
            compare(part("streak").text, "已連續 2／3 次夠快")
            compare(part("streak").visible, true)
        }

        function test_checking_text() {
            VrLauncher.setPreflight(true, false, -1, -1, 0)
            tryCompare(dialog, "opened", true, 3000)
            compare(dialog.title, "正在檢查連線")
            compare(part("status").text, "正在量測到主機的往返時間…")
        }

        function test_layout() {
            VrLauncher.setPreflight(false, true, 104, 30, 2)            // 每一行都顯示的狀態
            tryCompare(dialog, "opened", true, 2000)
            verify(dialog.width > 200 && dialog.width <= 640, "width " + dialog.width)
            verify(dialog.height > 150 && dialog.height <= 520, "height " + dialog.height)
            var names = ["headline", "measured", "cause", "status", "latest", "streak"]
            for (var i = 0; i < names.length; i++) {
                var label = part(names[i])
                verify(label.visible, names[i])
                verify(label.width > 0 && label.width <= 520, names[i] + " width " + label.width)
                verify(!label.truncated, names[i] + " is cut off")
                verify(label.height >= label.contentHeight - 0.5, names[i] + " is clipped vertically")
            }
            var start = part("startButton")
            var cancel = part("cancelButton")
            verify(start.width >= start.implicitWidth - 0.5)             // 按鈕上的字放得下
            verify(cancel.width >= cancel.implicitWidth - 0.5)
            verify(start.x + start.width <= cancel.x, "buttons overlap")
            var popupItem = dialog.contentItem.parent
            var p = cancel.mapToItem(popupItem, cancel.width, cancel.height)
            verify(p.x <= popupItem.width && p.y <= popupItem.height, "cancel button sticks out")
            var q = start.mapToItem(popupItem, 0, 0)
            verify(q.x >= 0 && q.y >= 0, "start button sticks out")
        }
    }
}
