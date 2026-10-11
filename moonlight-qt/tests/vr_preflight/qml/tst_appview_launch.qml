import QtQuick 2.9
import QtQuick.Controls 2.5
import QtTest 1.2

import ComputerManager 1.0
import VrLauncher 1.0

// 真的 AppView.qml（連同它用到的 QuitSegue.qml、NavigableMessageDialog.qml…）與真的 VrPreflightDialog.qml
import "../../../app/gui"

// §VR-PREFLIGHT：AppView.qml 開 VR 串流的三條路（直接開、接管、先結束舊 app 再開）都經過 VrLauncher.launch()，
// 帶的是 AppModel.vrLaunchTarget() 的位址與埠；對話框開著的時候開不了第二場。
// C++ 的型別都是替身（imports/）；執行方式見 ../run_tests.ps1。
Item {
    id: root
    width: 1280
    height: 800

    // main.qml 的工具列（QuitSegue 會把它藏起來再放回來）
    Item {
        id: toolBar
    }

    StackView {
        id: stackView
        anchors.fill: parent
        focus: true
    }

    Component {
        id: appViewComponent
        AppView {
        }
    }

    // main.qml 裡的那一個
    VrPreflightDialog {
        id: preflightDialog
    }

    TestCase {
        name: "AppViewVrLaunch"
        when: windowShown

        property var view: null

        function init() {
            VrLauncher.resetForTest()
            tryCompare(preflightDialog, "visible", false, 2000)
            view = stackView.push(appViewComponent, {"computerIndex": 0, "objectName": "Host"}, StackView.Immediate)
            verify(view)
            tryCompare(view, "activated", true, 2000)
            compare(view.count, 2)
            verify(view.appModel)
        }

        function cleanup() {
            // 留在畫面上的對話框先關掉（測試失敗時才會有）
            var button = dialogButton(/./)
            if (button) {
                keyClick(Qt.Key_Escape)
            }
            // QuitSegue 的進退場動畫跑完再清（動畫中途清掉的話，退到一半的那一頁會留在畫面上擋住下一個測試）
            tryCompare(stackView, "busy", false, 3000)
            stackView.clear(StackView.Immediate)
            view = null
            wait(50)
        }

        function findVisual(item, accept) {
            if (!item) {
                return null
            }
            if (accept(item)) {
                return item
            }
            for (var i = 0; i < item.children.length; i++) {
                var found = findVisual(item.children[i], accept)
                if (found) {
                    return found
                }
            }
            return null
        }

        // 目前開著的對話框上、文字符合 pattern 的按鈕（像使用者一樣用看的找，不靠 AppView 裡的 id）
        function dialogButton(pattern) {
            return findVisual(root.Overlay.overlay, function(item) {
                return item.visible && item.text !== undefined && item.clicked !== undefined
                        && item.flat !== undefined && pattern.test(item.text)
            })
        }

        function clickDialogButton(pattern) {
            var button = null
            tryVerify(function() { button = dialogButton(pattern); return button !== null }, 3000, "no dialog button " + pattern)
            wait(300)                                                   // 等進場動畫跑完
            mouseClick(button)
        }

        function tile(index) {
            var item = view.itemAtIndex(index)
            verify(item, "no tile " + index)
            return item
        }

        function test_1_direct_launch_with_the_pointer() {
            mouseClick(tile(0))
            compare(VrLauncher.launches.length, 1)
            var launch = VrLauncher.launches[0]
            compare(launch.hostAddress, "host.example:47989")           // 子行程找主機用的
            compare(launch.probeHost, "host.example")                   // 量往返時間用的：同一個位址
            compare(launch.probePort, 47989)                            // 同一個埠
            compare(launch.appName, "Beat Saber")
            compare(launch.takeover, false)
            compare(view.appModel.targetReads, 1)                       // 三個欄位一次讀出
        }

        function test_2_direct_launch_with_the_gamepad() {
            view.currentIndex = 1
            view.forceActiveFocus()
            keyClick(Qt.Key_Return)                                     // 手把的 A
            compare(VrLauncher.launches.length, 1)
            compare(VrLauncher.launches[0].appName, "Half-Life: Alyx")
            compare(VrLauncher.launches[0].probeHost, "host.example")
            compare(VrLauncher.launches[0].probePort, 47989)
            compare(VrLauncher.launches[0].takeover, false)
        }

        function test_3_takeover() {
            view.appModel.runningAppId = 12
            view.appModel.ownerUuid = "another-device"
            view.appModel.ownerName = "Living room"
            mouseClick(tile(0))
            compare(VrLauncher.launches.length, 0)                      // 先問要不要接管
            clickDialogButton(/Yes/)
            compare(VrLauncher.launches.length, 1)
            var launch = VrLauncher.launches[0]
            compare(launch.takeover, true)
            compare(launch.appName, "Beat Saber")
            compare(launch.hostAddress, "host.example:47989")
            compare(launch.probeHost, "host.example")
            compare(launch.probePort, 47989)
        }

        function test_4_takeover_declined() {
            view.appModel.runningAppId = 12
            view.appModel.ownerUuid = "another-device"
            mouseClick(tile(0))
            clickDialogButton(/No/)
            wait(400)
            compare(VrLauncher.launches.length, 0)
        }

        function test_5_quit_the_running_app_first() {
            view.appModel.runningAppId = 12                             // 主機上跑的是另一個 app，而且是這台裝置開的
            view.appModel.runningAppName = "Half-Life: Alyx"
            mouseClick(tile(0))
            compare(VrLauncher.launches.length, 0)
            clickDialogButton(/Yes/)                                    // 「確定要結束 …？」
            tryCompare(view.appModel, "quitCalls", 1, 3000)             // QuitSegue 上場、開始結束舊 app
            compare(VrLauncher.launches.length, 0)                      // 結束完成之前還不開
            compare(view.appModel.targetReads, 1)                       // 位址在按下「是」的時候就讀好了

            // 結束期間主機換了位址：用的是按下去當時讀到的那一組（跟改動前的行為一樣）
            view.appModel.target = {"address": "elsewhere.example:1", "host": "elsewhere.example", "port": 1}
            ComputerManager.quitAppCompleted(undefined)
            compare(VrLauncher.launches.length, 1)
            var launch = VrLauncher.launches[0]
            compare(launch.appName, "Beat Saber")
            compare(launch.takeover, false)
            compare(launch.hostAddress, "host.example:47989")
            compare(launch.probeHost, "host.example")
            compare(launch.probePort, 47989)
            tryCompare(stackView, "depth", 1, 3000)                     // QuitSegue 收掉，回到 app 清單
        }

        // 排在最後跑（測試照名稱的順序執行）：QuitSegue 失敗時會開一個屬於它自己的錯誤對話框、同時把自己收掉，
        // 留下的畫面狀態會影響排在後面的測試
        function test_z_quit_fails_nothing_is_launched() {
            view.appModel.runningAppId = 12
            view.appModel.runningAppName = "Half-Life: Alyx"
            mouseClick(tile(0))
            clickDialogButton(/Yes/)
            tryCompare(view.appModel, "quitCalls", 1, 3000)
            ComputerManager.quitAppCompleted("could not quit")
            wait(300)
            compare(VrLauncher.launches.length, 0)
        }

        function test_7_host_cannot_do_vr() {
            view.appModel.vrReason = "This host doesn't offer VR streaming."
            mouseClick(tile(0))
            compare(VrLauncher.launches.length, 0)
            clickDialogButton(/OK/)
            wait(300)
            compare(VrLauncher.launches.length, 0)
        }

        function test_8_no_second_launch_under_the_preflight_dialog() {
            VrLauncher.slowOnLaunch = true                              // 這一次量到的往返太慢
            mouseClick(tile(0))
            compare(VrLauncher.launches.length, 1)
            compare(VrLauncher.preflightWaiting, true)
            compare(preflightDialog.visible, true)                      // main.qml 的對話框自己開了

            // 對話框還在進場，就再點同一個、再點另一個 app，再按 A
            mouseClick(tile(0))
            mouseClick(tile(1))
            keyClick(Qt.Key_Return)
            compare(VrLauncher.launches.length, 1)
            compare(VrLauncher.cancelCalls, 0)
            compare(VrLauncher.startAnywayCalls, 0)

            tryCompare(preflightDialog, "armed", true, 2000)
            mouseClick(tile(1))                                         // 開好之後點後面的 app 一樣沒有作用
            compare(VrLauncher.launches.length, 1)
            compare(preflightDialog.opened, true)

            keyClick(Qt.Key_Escape)                                     // 手把的 B：取消
            compare(VrLauncher.cancelCalls, 1)
            tryCompare(preflightDialog, "visible", false, 2000)

            VrLauncher.slowOnLaunch = false
            mouseClick(tile(1))                                         // 取消之後可以再開
            compare(VrLauncher.launches.length, 2)
            compare(VrLauncher.launches[1].appName, "Half-Life: Alyx")
        }

        function test_9_page_goes_away_while_waiting() {
            VrLauncher.slowOnLaunch = true
            mouseClick(tile(0))
            tryCompare(preflightDialog, "opened", true, 2000)
            tryCompare(preflightDialog, "armed", true, 2000)

            // 等待中主機暫時離線：AppView 被收掉。對話框不屬於它，照樣開著、照樣能操作
            stackView.clear(StackView.Immediate)
            view = null
            wait(100)
            compare(preflightDialog.opened, true)
            compare(preflightDialog.focusPlace(), 1)
            keyClick(Qt.Key_Left)
            keyClick(Qt.Key_Return)                                     // 「仍要開始」
            compare(VrLauncher.startAnywayCalls, 1)
            tryCompare(preflightDialog, "visible", false, 2000)
        }
    }
}
