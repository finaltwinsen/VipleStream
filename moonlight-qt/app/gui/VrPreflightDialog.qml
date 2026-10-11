import QtQuick 2.9
import QtQuick.Controls 2.5
import QtQuick.Layouts 1.2
import QtQuick.Window 2.2

import VrLauncher 1.0

// VipleStream 2.0 §VR-PREFLIGHT：開 VR 串流之前量到主機的往返時間，太慢時顯示這個對話框。
//
// 量測、計時、什麼時候自己開始都在 C++（VrLauncher／vrpreflight.h）；這裡只顯示 VrLauncher 的狀態，
// 並把兩個按鈕轉達回去。放在 main.qml（不會被銷毀的地方）：等待期間 AppView 可能因為主機暫時離線而被收掉。
//
//   VrLauncher.preflightWaiting   太慢 → 立刻顯示：量到幾毫秒、為什麼不能玩、最新的值、「仍要開始」「取消」
//   VrLauncher.preflightChecking  還在量 → 通常幾十毫秒就有結果，不顯示；超過 showCheckingAfterMs 還沒有結果
//                                 （例如每一次都逾時）才顯示「正在檢查連線」，使用者不會對著沒反應的畫面等
//   兩個都不是                     關閉（檢查通過、自動開始、使用者按了按鈕）
//
// 操作：雷射＝滑鼠，點按鈕；手把的左右換按鈕、A（Return，設定頁的導覽模式下是 Space）按下、B（Escape）取消。
// 點對話框外面不會關：雷射很容易點到外面。modal，所以後面那一頁點不到；VrLauncher.launch() 在檢查有結果之前
// 也不接第二次。
NavigableDialog {
    id: dialog

    // 顯示哪一種內容：true＝太慢，false＝還在量。關閉的過程中維持原樣，淡出時不換字
    property bool slow: false

    // sync() 認為這個對話框現在該開著
    property bool shown: false

    // 剛出現的前 armAfterMs 毫秒不接受按鈕。這個對話框是在使用者按下 app 之後隔一下才自己跳出來的，
    // 不耐煩再點一次的那一下可能正好落在按鈕上
    property bool armed: false

    readonly property int showCheckingAfterMs: 600
    readonly property int armAfterMs: 400

    // 焦點被別的東西拿走時要還給哪一顆按鈕
    property Button lastFocusButton: cancelButton

    closePolicy: Popup.CloseOnEscape
    focus: true
    title: slow ? qsTr("Connection too slow for VR") : qsTr("Checking the connection")

    function sync() {
        if (VrLauncher.preflightWaiting) {
            checkingDelay.stop()
            slow = true
            show()
        }
        else if (VrLauncher.preflightChecking) {
            if (!shown && !checkingDelay.running) {
                checkingDelay.start()
            }
        }
        else {
            checkingDelay.stop()
            shown = false
            close()
        }
    }

    function show() {
        if (shown) {
            return
        }
        shown = true
        armed = false
        armDelay.restart()
        open()
        // open() 之後對話框已經掛進視窗：進場動畫還沒跑完就把焦點放好，這段時間的按鍵不會落到後面那一頁
        focusDefault()
    }

    // 把焦點放到 button 上，並且看得到焦點框（手把沒有游標，只能靠它知道現在在哪一顆）。
    // 這一顆上次關閉時就有焦點的話，forceActiveFocus() 什麼事件都不會發，焦點框也就不會出現
    // （對話框第二次打開時就是這樣）：先放掉再給。
    function giveFocus(button) {
        button.focus = false
        button.forceActiveFocus(Qt.TabFocus)
    }

    function focusDefault() {
        // 預設在「取消」：不小心多按一下 A 不會把一場會破圖的串流開起來
        lastFocusButton = cancelButton
        giveFocus(cancelButton)
    }

    function startAnyway() {
        if (!armed) {
            return
        }
        VrLauncher.preflightStartAnyway()
        sync()
    }

    function cancelFromButton() {
        if (!armed) {
            return
        }
        reject()
    }

    // item 所在的 popup 最外層的 item；不在任何 popup 裡回 null
    function popupRootOf(item) {
        var overlay = dialog.Overlay.overlay
        if (!overlay) {
            return null
        }
        for (; item; item = item.parent) {
            if (item.parent === overlay) {
                return item
            }
        }
        return null
    }

    // 目前的焦點在哪裡：0＝沒有（視窗不在前景）、1＝這個對話框裡、2＝別的 popup 裡、3＝後面那一頁
    function focusPlace() {
        var focusItem = content.windowFocusItem()
        if (!focusItem) {
            return 0
        }
        var root = popupRootOf(focusItem)
        if (root === null) {
            return 3
        }
        return root === popupRootOf(content) ? 1 : 2
    }

    // 對話框開著的時候焦點跑到後面那一頁（別的對話框關閉時 NavigableDialog 會把焦點交給 stackView；
    // 主機暫時離線、AppView 被收掉時 stackView 也會把焦點給新的那一頁），手把按的就是被遮住的東西：拿回來。
    // 焦點在別的 popup 裡不搶（那個 popup 疊在這個上面，要讓使用者先處理它）。
    function focusMoved() {
        if (shown && focusPlace() === 3) {
            // 在 activeFocusItemChanged 裡面直接 forceActiveFocus 會被外層還沒做完的那一次蓋掉，所以延後
            Qt.callLater(reclaimFocus)
        }
    }

    function reclaimFocus() {
        if (shown && visible && focusPlace() === 3) {
            giveFocus(lastFocusButton || cancelButton)
        }
    }

    // 視窗失去焦點再回來（例：等連線變快的時候叫出系統的介面再回來——這個對話框可能開很久），或對話框出現時
    // 視窗剛好不在前景：Qt 把焦點還給原本那一顆，但原因是「視窗變成作用中」，按鈕不畫焦點框。A 仍然作用在
    // 那一顆上，畫面上卻看不出是哪一顆，而兩顆的結果相反。視窗回到前景時補上。
    function windowActiveChanged() {
        if (content.windowIsActive()) {
            // 這時候 Qt 已經把焦點還完了；延後一拍，不在它的通知裡面再搬一次焦點
            Qt.callLater(restoreFocusRing)
        }
    }

    function restoreFocusRing() {
        var button = lastFocusButton || cancelButton
        // 焦點在別的 popup 裡不動它；在後面那一頁的由 reclaimFocus() 處理
        if (shown && visible && focusPlace() === 1 && !button.visualFocus) {
            giveFocus(button)
        }
    }

    Component.onCompleted: sync()

    onOpened: {
        // 進場動畫期間使用者可能已經換到另一顆：留在那一顆上，只確認焦點真的在對話框裡、焦點框有出現
        var button = lastFocusButton || cancelButton
        if (focusPlace() !== 1 || !button.visualFocus) {
            giveFocus(button)
        }
    }

    onClosed: {
        shown = false
        armDelay.stop()
    }

    // Escape、手把的 B、「取消」都到這裡
    onRejected: {
        VrLauncher.preflightCancel()
        sync()
    }

    Connections {
        target: VrLauncher
        function onPreflightChanged() {
            dialog.sync()
        }
    }

    Timer {
        id: checkingDelay
        interval: dialog.showCheckingAfterMs
        onTriggered: {
            if (VrLauncher.preflightChecking) {
                dialog.slow = false
                dialog.show()
            }
            else {
                dialog.sync()
            }
        }
    }

    Timer {
        id: armDelay
        interval: dialog.armAfterMs
        onTriggered: dialog.armed = true
    }

    ColumnLayout {
        id: content
        spacing: 12

        // 附加屬性要在 Item 自己的函式裡讀（dialog 是 Popup，不是 Item；寫在 Connections 或 Timer 裡讀到的是 null）
        function windowFocusItem() {
            return Window.activeFocusItem
        }

        function windowIsActive() {
            return Window.active
        }

        Window.onActiveFocusItemChanged: dialog.focusMoved()
        Window.onActiveChanged: dialog.windowActiveChanged()

        // 文字最寬到這裡就換行（對話框的寬度跟著內容）。上限要設在每一個 Label 上：設在外層的 RowLayout 上
        // 只會限制排版時給的寬度，對話框還是會照沒換行的那一行撐開
        readonly property int textWidth: 520
        readonly property int spinnerSize: 36

        Label {
            id: headline
            objectName: "headline"
            visible: dialog.slow
            Layout.fillWidth: true
            Layout.maximumWidth: content.textWidth
            wrapMode: Text.Wrap
            font.pixelSize: 16
            text: qsTr("The connection to the host is too slow for VR right now. The picture would break up and lag far behind your head.")
        }

        Label {
            id: measured
            objectName: "measured"
            visible: dialog.slow
            Layout.fillWidth: true
            Layout.maximumWidth: content.textWidth
            wrapMode: Text.Wrap
            font.pixelSize: 16
            font.bold: true
            text: qsTr("Measured round trip: %1 ms. VR needs %2 ms or less.")
                      .arg(VrLauncher.preflightMeasuredMs).arg(VrLauncher.preflightThresholdMs)
        }

        Label {
            id: cause
            objectName: "cause"
            visible: dialog.slow
            Layout.fillWidth: true
            Layout.maximumWidth: content.textWidth
            wrapMode: Text.Wrap
            opacity: 0.7
            text: qsTr("This usually means the VPN is relaying the connection instead of connecting directly.")
        }

        RowLayout {
            id: live
            Layout.fillWidth: true
            spacing: 12

            BusyIndicator {
                running: dialog.visible
                Layout.preferredWidth: content.spinnerSize
                Layout.preferredHeight: content.spinnerSize
                Layout.alignment: Qt.AlignTop
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 4

                Label {
                    id: status
                    objectName: "status"
                    Layout.fillWidth: true
                    Layout.maximumWidth: content.textWidth - content.spinnerSize - live.spacing
                    wrapMode: Text.Wrap
                    text: dialog.slow
                          ? qsTr("Checking again every %1 seconds. VR starts automatically as soon as the connection is fast enough.")
                                .arg(VrLauncher.preflightIntervalSec)
                          : qsTr("Measuring the round trip to the host…")
                }

                Label {
                    id: latest
                    objectName: "latest"
                    visible: dialog.slow
                    Layout.fillWidth: true
                    Layout.maximumWidth: content.textWidth - content.spinnerSize - live.spacing
                    wrapMode: Text.Wrap
                    text: VrLauncher.preflightRttMs >= 0
                          ? qsTr("Latest: %1 ms").arg(VrLauncher.preflightRttMs)
                          : qsTr("Latest: no answer from the host")
                }

                // 一直顯示（0／3 也顯示）：使用者看得到自動開始的條件，對話框的高度也不會在等待中跳動
                Label {
                    id: streak
                    objectName: "streak"
                    visible: dialog.slow
                    Layout.fillWidth: true
                    Layout.maximumWidth: content.textWidth - content.spinnerSize - live.spacing
                    wrapMode: Text.Wrap
                    text: qsTr("Fast enough %1 of %2 times in a row")
                              .arg(VrLauncher.preflightGoodStreak).arg(VrLauncher.preflightGoodNeeded)
                }
            }
        }
    }

    // 不用 standardButtons：標準按鈕的文字是固定的，而且按了就先關對話框（這裡要等 VrLauncher 的狀態變了才關）
    footer: Item {
        implicitHeight: 56

        RowLayout {
            anchors.right: parent.right
            anchors.rightMargin: 16
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8

            // 兩顆按鈕都不因為被雷射按到而拿焦點（Button 預設會）：焦點框是手把的游標，雷射照樣點得到按鈕，但不搬動它。
            // 不然剛出現、還不接受按鈕的那一小段裡點到「仍要開始」（那一下不算），或按住之後移開才放掉（沒有點成），
            // 焦點都會留在「仍要開始」上——預設就不再是「取消」，後一種連焦點框都沒有——接著手把按 A 開的是一場
            // 會破圖的串流。
            Button {
                id: startButton
                objectName: "startButton"
                flat: true
                focusPolicy: Qt.TabFocus
                text: qsTr("Start anyway")
                onClicked: dialog.startAnyway()
                onActiveFocusChanged: if (activeFocus) dialog.lastFocusButton = startButton
                Keys.onReturnPressed: clicked()
                Keys.onEnterPressed: clicked()
                Keys.onRightPressed: cancelButton.forceActiveFocus(Qt.TabFocus)
            }

            Button {
                id: cancelButton
                objectName: "cancelButton"
                flat: true
                focusPolicy: Qt.TabFocus
                text: qsTr("Cancel")
                onClicked: dialog.cancelFromButton()
                onActiveFocusChanged: if (activeFocus) dialog.lastFocusButton = cancelButton
                Keys.onReturnPressed: clicked()
                Keys.onEnterPressed: clicked()
                Keys.onLeftPressed: startButton.forceActiveFocus(Qt.TabFocus)
            }
        }
    }
}
