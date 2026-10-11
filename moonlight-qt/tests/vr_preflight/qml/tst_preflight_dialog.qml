import QtQuick 2.9
import QtQuick.Controls 2.5
import QtQuick.Window 2.2
import QtTest 1.2

import VrLauncher 1.0

// 真的檔案（不是照抄一份）：app/gui/VrPreflightDialog.qml，連同它的基底 NavigableDialog.qml
import "../../../app/gui"

// §VR-PREFLIGHT 對話框的流程測試。VrLauncher 是替身（imports/VrLauncher）；執行方式見 ../run_tests.sh。
// 這裡驗的是對話框本身：什麼時候開、顯示什麼、兩個按鈕（滑鼠＝雷射、鍵盤＝手把送出的鍵）、
// 開著的時候後面那一頁按不到、焦點被搶走會拿回來。
Item {
    id: root
    width: 1280
    height: 800

    // 測試視窗本身（附加屬性要在 Item 自己的函式裡讀）
    function ownWindow() {
        return Window.window
    }

    // 另一個頂層視窗，代表頭盔上蓋到 VipleStream 前面的別的介面：把焦點給它＝測試視窗失去焦點。
    // 用到的測試才顯示（initTestCase 建立）
    property var otherWindow: null

    // main.qml 的 stackView：NavigableDialog 關閉時會對它 forceActiveFocus()
    FocusScope {
        id: stackView
        anchors.fill: parent
        focus: true

        // main.qml 的 stackView 收到 Escape／Menu／Hangup 會退回上一頁或開設定頁：對話框開著時這些鍵不能漏到這裡
        property int keysLeaked: 0
        Keys.onEscapePressed: keysLeaked++
        Keys.onBackPressed: keysLeaked++
        Keys.onMenuPressed: keysLeaked++
        Keys.onHangupPressed: keysLeaked++

        // 後面那一頁上的東西（代表 app 格子）：對話框開著的時候不應該按得到
        Button {
            id: behind
            x: 500
            y: 520
            width: 280
            height: 80
            focus: true
            text: "behind"
            property int activations: 0
            onClicked: activations++
            Keys.onReturnPressed: activations++
            Keys.onSpacePressed: activations++
        }
    }

    VrPreflightDialog {
        id: dialog
    }

    // 疊在上面的另一個對話框（例：啟動時非同步跳出來的設定警告），用的也是真的 NavigableMessageDialog
    NavigableMessageDialog {
        id: otherDialog
        standardButtons: Dialog.Ok
        text: "another dialog"
    }

    TestCase {
        id: testCase
        name: "VrPreflightDialog"
        when: windowShown

        function part(name) {
            var o = findChild(dialog, name)
            verify(o, "missing " + name)
            return o
        }

        function waitClosed() {
            tryCompare(dialog, "visible", false, 2000)
        }

        // 太慢：對話框開好、過了剛出現時不接受按鈕的那一小段
        function openSlow(measured, latest, streak) {
            VrLauncher.setPreflight(false, true, measured, latest, streak === undefined ? 0 : streak)
            tryCompare(dialog, "opened", true, 2000)
            tryCompare(dialog, "armed", true, 2000)
        }

        function initTestCase() {
            root.otherWindow = Qt.createQmlObject("import QtQuick 2.9; import QtQuick.Window 2.2; Window { width: 160; height: 90 }", root)
            // 不可以是測試視窗的 transient 子視窗：那樣它拿到焦點時，測試視窗在 Qt 看來仍然是作用中的
            root.otherWindow.transientParent = null
        }

        // 測試視窗失去焦點（別的視窗到前景）
        function deactivateWindow() {
            root.otherWindow.show()
            root.otherWindow.requestActivate()
            tryCompare(root.ownWindow(), "active", false, 3000)
        }

        // 測試視窗回到前景
        function activateWindow() {
            root.ownWindow().requestActivate()
            tryCompare(root.ownWindow(), "active", true, 3000)
            root.otherWindow.hide()
        }

        function init() {
            if (!root.ownWindow().active) {
                activateWindow()                                        // 上一個測試失敗時可能留在背景
            }
            VrLauncher.resetForTest()
            if (otherDialog.visible) {
                otherDialog.close()
                tryCompare(otherDialog, "visible", false, 2000)
            }
            waitClosed()
            behind.activations = 0
            stackView.keysLeaked = 0
            stackView.forceActiveFocus()
            tryCompare(behind, "activeFocus", true, 2000)
        }

        function test_01_idle_is_closed() {
            compare(dialog.visible, false)
            compare(dialog.shown, false)
            wait(dialog.showCheckingAfterMs + 200)
            compare(dialog.visible, false)
        }

        function test_02_slow_opens_and_says_what_was_measured() {
            VrLauncher.setPreflight(false, true, 104, 131, 0)
            compare(dialog.visible, true)              // 狀態一變就開，不等
            tryCompare(dialog, "opened", true, 2000)
            compare(dialog.slow, true)
            compare(dialog.modal, true)
            compare(dialog.title, qsTr("Connection too slow for VR"))
            verify(dialog.title.length > 0)

            var measured = part("measured")
            verify(measured.visible)
            verify(measured.text.indexOf("104") >= 0, measured.text)    // 判定太慢時量到的值
            verify(measured.text.indexOf("60") >= 0, measured.text)     // 門檻
            verify(part("headline").visible)
            verify(part("headline").text.length > 20)
            verify(part("cause").visible)
            verify(part("cause").text.indexOf("VPN") >= 0)
            var status = part("status")
            verify(status.text.indexOf("2") >= 0, status.text)          // 每 2 秒
            var latest = part("latest")
            verify(latest.visible)
            verify(latest.text.indexOf("131") >= 0, latest.text)
            var streak = part("streak")
            verify(streak.visible)                                      // 0／3 也顯示：看得到自動開始的條件
            verify(streak.text.indexOf("0") >= 0 && streak.text.indexOf("3") >= 0, streak.text)
            verify(part("startButton").visible)
            verify(part("cancelButton").visible)
            verify(part("startButton").text.length > 0)
            verify(part("cancelButton").text.length > 0)
        }

        function test_03_latest_value_follows_the_probes() {
            openSlow(104, 131)
            var latest = part("latest")
            var streak = part("streak")
            var measured = part("measured")

            VrLauncher.setPreflight(false, true, 104, 98, 0)
            verify(latest.text.indexOf("98") >= 0, latest.text)
            verify(measured.text.indexOf("104") >= 0)                   // 這個不跟著變

            VrLauncher.setPreflight(false, true, 104, -1, 0)            // 這一次量不到
            verify(latest.text.indexOf("-1") < 0, latest.text)
            verify(latest.text.length > 0)
            var noAnswer = latest.text

            var heightBefore = dialog.height
            VrLauncher.setPreflight(false, true, 104, 31, 1)
            verify(latest.text !== noAnswer)
            verify(latest.text.indexOf("31") >= 0)
            verify(streak.text.indexOf("1") >= 0 && streak.text.indexOf("3") >= 0, streak.text)

            VrLauncher.setPreflight(false, true, 104, 29, 2)
            verify(streak.text.indexOf("2") >= 0, streak.text)

            VrLauncher.setPreflight(false, true, 104, 150, 0)           // 又變慢：連續次數歸零
            verify(streak.text.indexOf("0") >= 0, streak.text)
            compare(dialog.height, heightBefore)                        // 等待中對話框不會忽大忽小（按鈕不會在雷射底下移動）
            compare(dialog.opened, true)                                // 每次更新都不會重開或關掉
            compare(part("cancelButton").activeFocus, true)
        }

        function test_04_auto_start_closes_it() {
            openSlow(104, 30, 2)
            VrLauncher.setPreflight(false, false, 104, 28, 3)           // C++ 決定開始：狀態回到沒有在檢查
            compare(dialog.shown, false)
            compare(dialog.slow, true)                                  // 淡出的時候不換字
            compare(dialog.title, qsTr("Connection too slow for VR"))
            waitClosed()
            compare(VrLauncher.startAnywayCalls, 0)
            compare(VrLauncher.cancelCalls, 0)
            tryCompare(behind, "activeFocus", true, 2000)               // 關掉之後焦點回到後面那一頁
        }

        function test_05_start_anyway_with_the_pointer() {
            openSlow(118, 118)
            mouseClick(part("startButton"))
            compare(VrLauncher.startAnywayCalls, 1)
            compare(VrLauncher.cancelCalls, 0)
            waitClosed()
            dialog.startAnyway()                                        // 關掉之後才到的那一下不會多算
            compare(VrLauncher.startAnywayCalls, 1)
        }

        function test_05b_focus_ring_shows_again_when_it_reopens() {
            // 上一次關閉時有焦點的那顆按鈕留著焦點：再打開時它收不到新的 focus 事件，焦點框要另外補，
            // 手把使用者才看得到現在在哪一顆
            openSlow(118, 118)
            mouseClick(part("cancelButton"))                            // 用雷射點掉（焦點本來就在「取消」上）
            waitClosed()
            openSlow(118, 118)
            compare(part("cancelButton").activeFocus, true)
            compare(part("cancelButton").visualFocus, true)
            keyClick(Qt.Key_Return)                                     // 用手把按掉
            waitClosed()
            openSlow(118, 118)
            compare(part("cancelButton").activeFocus, true)
            compare(part("cancelButton").visualFocus, true)
            keyClick(Qt.Key_Left)
            keyClick(Qt.Key_Return)                                     // 這一次停在「仍要開始」
            waitClosed()
            openSlow(118, 118)
            compare(part("cancelButton").activeFocus, true)             // 不管上次停在哪一顆，都回到預設的
            compare(part("cancelButton").visualFocus, true)
            compare(part("startButton").activeFocus, false)
        }

        function test_06_cancel_with_the_pointer() {
            openSlow(118, 118)
            mouseClick(part("cancelButton"))
            compare(VrLauncher.cancelCalls, 1)
            compare(VrLauncher.startAnywayCalls, 0)
            waitClosed()
        }

        function test_07_gamepad_keys() {
            openSlow(118, 118)
            var start = part("startButton")
            var cancel = part("cancelButton")
            compare(cancel.activeFocus, true)                           // 預設在「取消」
            compare(cancel.visualFocus, true)                           // 看得到焦點在哪（手把沒有游標）

            keyClick(Qt.Key_Left)
            compare(start.activeFocus, true)
            compare(start.visualFocus, true)
            keyClick(Qt.Key_Left)                                       // 已經在最左邊
            compare(start.activeFocus, true)
            keyClick(Qt.Key_Right)
            compare(cancel.activeFocus, true)
            keyClick(Qt.Key_Right)
            compare(cancel.activeFocus, true)
            keyClick(Qt.Key_Up)                                         // 手把的上下：沒有作用，也不會跑到後面
            keyClick(Qt.Key_Down)
            keyClick(Qt.Key_Menu)                                       // 手把的 X
            keyClick(Qt.Key_Hangup)                                     // 手把的 Y／Start（main.qml 用它開設定頁）
            compare(cancel.activeFocus, true)
            compare(behind.activations, 0)
            compare(stackView.keysLeaked, 0)
            compare(dialog.opened, true)

            keyClick(Qt.Key_Left)
            keyClick(Qt.Key_Return)                                     // 手把的 A
            compare(VrLauncher.startAnywayCalls, 1)
            compare(VrLauncher.cancelCalls, 0)
            waitClosed()
            compare(behind.activations, 0)
        }

        function test_08_return_on_the_default_button_cancels() {
            openSlow(118, 118)
            keyClick(Qt.Key_Return)
            compare(VrLauncher.cancelCalls, 1)
            compare(VrLauncher.startAnywayCalls, 0)
            waitClosed()
            compare(behind.activations, 0)
        }

        function test_09_space_and_tab_work_too() {
            // 設定頁的導覽模式下，手把的 A 送 Space、上下送 Tab／Shift+Tab
            openSlow(118, 118)
            var start = part("startButton")
            var cancel = part("cancelButton")
            keyClick(Qt.Key_Tab)
            verify(start.activeFocus || cancel.activeFocus)
            keyClick(Qt.Key_Backtab)
            verify(start.activeFocus || cancel.activeFocus)             // Tab 不會離開對話框
            start.forceActiveFocus(Qt.TabFocus)
            keyClick(Qt.Key_Space)
            compare(VrLauncher.startAnywayCalls, 1)
            waitClosed()
            compare(behind.activations, 0)
        }

        function test_10_escape_cancels() {
            openSlow(118, 118)
            keyClick(Qt.Key_Escape)                                     // 手把的 B
            compare(VrLauncher.cancelCalls, 1)
            compare(VrLauncher.startAnywayCalls, 0)
            compare(stackView.keysLeaked, 0)                            // 只取消這一場，不會同時退回上一頁
            waitClosed()
        }

        function test_11_modal_nothing_behind_can_be_triggered() {
            openSlow(118, 118)
            mouseClick(behind)                                          // 點後面的 app
            compare(behind.activations, 0)
            mouseClick(root, 20, 20)                                    // 點對話框外面的空白
            mouseDoubleClickSequence(root, 1200, 700)
            compare(dialog.opened, true)                                // 不會關
            compare(VrLauncher.cancelCalls, 0)
            compare(VrLauncher.startAnywayCalls, 0)
            compare(behind.activations, 0)
            // 再點一次 app 也開不了第二場：launch() 在檢查有結果之前不接
            compare(VrLauncher.launch("host:1", "host", 1, "App", false), false)
            compare(VrLauncher.launches.length, 0)
        }

        function test_12_not_armed_right_after_it_appears() {
            VrLauncher.setPreflight(false, true, 118, 118, 0)
            compare(dialog.visible, true)
            compare(dialog.armed, false)
            // 不耐煩的第二下點擊正好落在按鈕上：不算
            mouseClick(part("startButton"))
            mouseClick(part("cancelButton"))
            keyClick(Qt.Key_Return)
            compare(VrLauncher.startAnywayCalls, 0)
            compare(VrLauncher.cancelCalls, 0)
            compare(behind.activations, 0)                              // 進場動畫期間的按鍵也不會落到後面那一頁
            compare(dialog.visible, true)

            tryCompare(dialog, "armed", true, 2000)
            mouseClick(part("startButton"))
            compare(VrLauncher.startAnywayCalls, 1)
            waitClosed()
        }

        function test_12b_ignored_click_does_not_change_the_default_button() {
            // 還不接受按鈕的那一小段裡雷射點到「仍要開始」：那一下不算，預設的按鈕也不可以因此換成它——
            // 接著手把按 A 要是「取消」，不是開一場會破圖的串流
            VrLauncher.setPreflight(false, true, 118, 118, 0)
            compare(dialog.armed, false)
            var start = part("startButton")
            var cancel = part("cancelButton")
            mouseClick(start)
            compare(VrLauncher.startAnywayCalls, 0)
            compare(cancel.activeFocus, true)
            tryCompare(dialog, "armed", true, 2000)
            tryCompare(dialog, "opened", true, 2000)
            compare(start.activeFocus, false)
            compare(cancel.activeFocus, true)
            compare(cancel.visualFocus, true)
            keyClick(Qt.Key_Return)                                     // 手把的 A
            compare(VrLauncher.startAnywayCalls, 0)
            compare(VrLauncher.cancelCalls, 1)
            waitClosed()
        }

        function test_12c_the_pointer_does_not_move_the_focus_ring() {
            var start = part("startButton")
            var cancel = part("cancelButton")

            // 雷射按住「仍要開始」、移開才放掉（沒有點成）：焦點框留在原本那一顆上，不會不見
            openSlow(118, 118)
            mousePress(start)
            mouseMove(root, 20, 20)
            mouseRelease(root, 20, 20)
            compare(VrLauncher.startAnywayCalls, 0)
            compare(dialog.opened, true)
            compare(start.activeFocus, false)
            compare(cancel.activeFocus, true)
            compare(cancel.visualFocus, true)
            keyClick(Qt.Key_Return)
            compare(VrLauncher.startAnywayCalls, 0)
            compare(VrLauncher.cancelCalls, 1)
            waitClosed()

            // 同樣的動作發生在還不接受按鈕的那一小段裡
            VrLauncher.setPreflight(false, true, 118, 118, 0)
            compare(dialog.armed, false)
            mousePress(start)
            mouseMove(root, 20, 20)
            mouseRelease(root, 20, 20)
            tryCompare(dialog, "armed", true, 2000)
            tryCompare(dialog, "opened", true, 2000)
            compare(cancel.activeFocus, true)
            compare(cancel.visualFocus, true)
            keyClick(Qt.Key_Return)
            compare(VrLauncher.startAnywayCalls, 0)
            compare(VrLauncher.cancelCalls, 2)
            waitClosed()

            // 雷射照樣點得到按鈕：手把先把焦點移到「仍要開始」，再用雷射點「取消」
            openSlow(118, 118)
            keyClick(Qt.Key_Left)
            compare(start.visualFocus, true)
            mouseClick(cancel)
            compare(VrLauncher.cancelCalls, 3)
            compare(VrLauncher.startAnywayCalls, 0)
            waitClosed()
        }

        function test_13_escape_works_even_before_it_is_armed() {
            VrLauncher.setPreflight(false, true, 118, 118, 0)
            keyClick(Qt.Key_Escape)
            compare(VrLauncher.cancelCalls, 1)
            waitClosed()
        }

        function test_14_focus_taken_by_the_page_behind_comes_back() {
            openSlow(118, 118)
            var start = part("startButton")
            var cancel = part("cancelButton")
            compare(dialog.focusPlace(), 1)

            // 別的對話框關閉（NavigableDialog.onClosed）或頁面切換（stackView.onCurrentItemChanged）會這樣做
            stackView.forceActiveFocus()
            tryCompare(cancel, "activeFocus", true, 2000)
            compare(behind.activeFocus, false)
            keyClick(Qt.Key_Return)
            compare(behind.activations, 0)
            compare(VrLauncher.cancelCalls, 1)
            waitClosed()

            // 原本在「仍要開始」上：還給同一顆
            openSlow(118, 118)
            keyClick(Qt.Key_Left)
            compare(start.activeFocus, true)
            behind.forceActiveFocus()
            tryCompare(start, "activeFocus", true, 2000)
            compare(dialog.focusPlace(), 1)
        }

        function test_15_another_dialog_on_top_keeps_its_focus() {
            openSlow(118, 118)
            var cancel = part("cancelButton")
            otherDialog.open()
            tryCompare(otherDialog, "opened", true, 2000)
            wait(100)                                                   // 留時間給可能發生的搶焦點
            compare(dialog.focusPlace(), 2)                             // 焦點在上面那個對話框裡，不搶
            compare(cancel.activeFocus, false)
            compare(dialog.opened, true)

            keyClick(Qt.Key_Return)                                     // 按掉上面那一個
            tryCompare(otherDialog, "visible", false, 2000)
            // 它關閉時把焦點交給 stackView，這裡要拿回來，否則手把按的是後面那一頁
            tryCompare(cancel, "activeFocus", true, 2000)
            compare(VrLauncher.cancelCalls, 0)
            keyClick(Qt.Key_Return)
            compare(VrLauncher.cancelCalls, 1)
            compare(behind.activations, 0)
            waitClosed()
        }

        function test_16_checking_is_not_shown_when_the_answer_is_quick() {
            VrLauncher.setPreflight(true, false, -1, -1, 0)
            compare(dialog.visible, false)
            wait(150)
            compare(dialog.visible, false)
            VrLauncher.setPreflight(false, false, -1, 12, 0)            // 夠快：直接開始
            wait(dialog.showCheckingAfterMs + 300)
            compare(dialog.visible, false)                              // 從頭到尾沒有閃一下
            compare(dialog.shown, false)
        }

        function test_17_checking_is_shown_when_it_takes_long() {
            VrLauncher.setPreflight(true, false, -1, -1, 0)
            wait(dialog.showCheckingAfterMs - 250)
            compare(dialog.visible, false)
            VrLauncher.setPreflight(true, false, -1, -1, 0)             // 中間的狀態更新不會把計時重來
            tryCompare(dialog, "visible", true, 600)
            compare(dialog.slow, false)
            compare(dialog.title, qsTr("Checking the connection"))
            compare(part("headline").visible, false)
            compare(part("measured").visible, false)
            compare(part("latest").visible, false)
            verify(part("status").text.length > 0)
            tryCompare(dialog, "armed", true, 2000)
            compare(part("cancelButton").activeFocus, true)

            // 量完了、太慢：同一個對話框換成太慢的內容，焦點不動
            VrLauncher.setPreflight(false, true, 118, 131, 0)
            compare(dialog.slow, true)
            compare(dialog.opened, true)
            compare(dialog.armed, true)
            verify(part("measured").visible)
            verify(part("measured").text.indexOf("118") >= 0)
            compare(part("cancelButton").activeFocus, true)
            keyClick(Qt.Key_Escape)
            compare(VrLauncher.cancelCalls, 1)
            waitClosed()
        }

        function test_18_checking_then_unmeasured_closes() {
            VrLauncher.setPreflight(true, false, -1, -1, 0)
            tryCompare(dialog, "visible", true, 2000)
            tryCompare(dialog, "armed", true, 2000)
            mouseClick(part("startButton"))                             // 量的時候也可以不等了
            compare(VrLauncher.startAnywayCalls, 1)
            waitClosed()

            VrLauncher.setPreflight(true, false, -1, -1, 0)
            tryCompare(dialog, "visible", true, 2000)
            VrLauncher.setPreflight(false, false, -1, -1, 0)            // 三次都量不到：照常開始
            waitClosed()
            compare(VrLauncher.cancelCalls, 0)
        }

        function test_19_can_be_used_again_and_again() {
            for (var i = 1; i <= 3; i++) {
                openSlow(100 + i, 100 + i)
                verify(part("measured").text.indexOf(String(100 + i)) >= 0)
                compare(part("cancelButton").activeFocus, true)         // 每次都從預設的按鈕開始
                keyClick(Qt.Key_Left)
                keyClick(Qt.Key_Return)
                compare(VrLauncher.startAnywayCalls, i)
                waitClosed()
            }
        }

        function test_21_layout() {
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
            // 對話框在視窗正中間、整個看得到
            var origin = popupItem.mapToItem(root, 0, 0)
            verify(origin.x >= 0 && origin.y >= 0)
            verify(origin.x + popupItem.width <= root.width && origin.y + popupItem.height <= root.height)
        }

        function test_20_reopening_while_it_is_still_fading_out() {
            openSlow(118, 118)
            VrLauncher.setPreflight(false, false, 118, 118, 0)
            compare(dialog.shown, false)
            // 還在淡出就又判定太慢（使用者取消後馬上再點一次）
            VrLauncher.setPreflight(false, true, 125, 125, 0)
            compare(dialog.shown, true)
            tryCompare(dialog, "opened", true, 2000)
            tryCompare(dialog, "armed", true, 2000)
            verify(part("measured").text.indexOf("125") >= 0)
            compare(part("cancelButton").activeFocus, true)
            mouseClick(part("cancelButton"))
            compare(VrLauncher.cancelCalls, 1)
            waitClosed()
        }

        // 視窗失去焦點再回來（等連線變快的時候叫出別的介面再回來；這個對話框可能開很久）：Qt 把焦點還給原本
        // 那顆按鈕，但不畫焦點框。兩顆按鈕的結果相反，手把使用者要看得到 A 會按在哪一顆上
        function test_22_focus_ring_comes_back_with_the_window() {
            openSlow(118, 118)
            var start = part("startButton")
            var cancel = part("cancelButton")
            compare(cancel.visualFocus, true)

            deactivateWindow()
            compare(dialog.focusPlace(), 0)                             // 視窗不在前景：沒有焦點
            compare(cancel.visualFocus, false)
            activateWindow()
            tryCompare(cancel, "visualFocus", true, 2000)
            compare(cancel.activeFocus, true)
            compare(dialog.opened, true)                                // 只是把焦點框補回來，沒有按到任何東西
            compare(VrLauncher.cancelCalls, 0)
            compare(VrLauncher.startAnywayCalls, 0)

            // 原本在「仍要開始」上：回來還是那一顆
            keyClick(Qt.Key_Left)
            compare(start.visualFocus, true)
            deactivateWindow()
            activateWindow()
            tryCompare(start, "visualFocus", true, 2000)
            compare(start.activeFocus, true)
            compare(cancel.activeFocus, false)

            keyClick(Qt.Key_Right)
            keyClick(Qt.Key_Return)
            compare(VrLauncher.cancelCalls, 1)
            compare(VrLauncher.startAnywayCalls, 0)
            compare(behind.activations, 0)
            waitClosed()
        }

        function test_23_appears_while_the_window_is_in_the_background() {
            deactivateWindow()
            VrLauncher.setPreflight(false, true, 118, 118, 0)
            tryCompare(dialog, "opened", true, 2000)
            tryCompare(dialog, "armed", true, 2000)
            var cancel = part("cancelButton")
            compare(cancel.visualFocus, false)                          // 視窗不在前景時誰都沒有焦點
            activateWindow()
            tryCompare(cancel, "visualFocus", true, 2000)               // 回來：焦點在預設的按鈕上，而且看得到
            compare(cancel.activeFocus, true)
            compare(behind.activeFocus, false)
            keyClick(Qt.Key_Return)
            compare(VrLauncher.cancelCalls, 1)
            compare(VrLauncher.startAnywayCalls, 0)
            compare(behind.activations, 0)
            waitClosed()
        }

        function test_24_window_comes_back_while_another_dialog_is_on_top() {
            openSlow(118, 118)
            var cancel = part("cancelButton")
            otherDialog.open()
            tryCompare(otherDialog, "opened", true, 2000)
            compare(dialog.focusPlace(), 2)
            deactivateWindow()
            activateWindow()
            wait(100)                                                   // 留時間給可能發生的搶焦點
            compare(dialog.focusPlace(), 2)                             // 焦點回到上面那個對話框裡，這裡不搶
            compare(cancel.activeFocus, false)
            compare(otherDialog.opened, true)

            keyClick(Qt.Key_Return)                                     // 按掉上面那一個
            tryCompare(otherDialog, "visible", false, 2000)
            tryCompare(cancel, "visualFocus", true, 2000)               // 焦點回到這個對話框，看得到
            compare(VrLauncher.cancelCalls, 0)
            keyClick(Qt.Key_Return)
            compare(VrLauncher.cancelCalls, 1)
            compare(behind.activations, 0)
            waitClosed()
        }
    }
}
