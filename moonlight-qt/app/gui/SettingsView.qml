import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.2
import QtQuick.Window 2.2

import StreamingPreferences 1.0
import ComputerManager 1.0
import SdlGamepadKeyNavigation 1.0
import SystemProperties 1.0
import VrLauncher 1.0

Flickable {
    id: settingsPage
    objectName: qsTr("Settings")

    signal languageChanged()

    boundsBehavior: Flickable.OvershootBounds

    contentWidth: settingsColumn1.width > settingsColumn2.width ? settingsColumn1.width : settingsColumn2.width
    contentHeight: settingsColumn1.height > settingsColumn2.height ? settingsColumn1.height : settingsColumn2.height

    ScrollBar.vertical: ScrollBar {
        // §UI-SCROLL：Steam Frame 上多半用雷射指標（＝滑鼠）操作，搖桿不會變成滾輪。捲軸在那裡一直顯示、
        // 做寬一點：看得出下面還有內容，也有東西可以用雷射抓著拖。其他平台維持原樣。
        policy: VrLauncher.available ? ScrollBar.AlwaysOn : ScrollBar.AsNeeded
        width: VrLauncher.available ? 18 : implicitWidth
        anchors {
            left: parent.right
            leftMargin: VrLauncher.available ? -20 : -10
        }
    }

    function isChildOfFlickable(item) {
        while (item) {
            if (item.parent === contentItem) {
                return true
            }

            item = item.parent
        }
        return false
    }

    NumberAnimation on contentY {
        id: autoScrollAnimation
        duration: 100
    }

    // §UI-SCROLL（2026-10-11）：用手把的右搖桿與 LB／RB（SdlGamepadKeyNavigation.scrollRequested）或鍵盤的
    // PageUp／PageDown 直接捲這一頁。原本手把與鍵盤只能靠焦點一項一項移動來帶動捲頁，這一頁有幾十個選項。
    //
    // 焦點的處理：有焦點的控制項被捲出畫面時，焦點先收到這一頁本身（focusParked；scrollOrigin 記住原本是哪一個）。
    // 留在原地的話使用者看不到焦點在哪：這時按 A 或左右會改到畫面外的設定，按上下則是焦點一動、頁面捲回原來的位置。
    // 捲動停下來 250 ms 之後，或使用者先按了上下，再把焦點放到看得到的控制項上（enterView）。
    property real scrollRemainderPx: 0
    property bool focusParked: false
    property Item scrollOrigin: null

    // 往下（pages > 0）或往上捲，單位是一個可見高度。fromGamepad＝這一次是手把捲的（差別見 parkFocus）
    function scrollByPages(pages, fromGamepad) {
        // 不到 1 px 的量先累著，不動動畫也不動計時器：搖桿只比死區多一點點（飄移）時，每次輪詢都會進來，
        // 每次都重跑動畫的話滑鼠滾輪會被蓋掉
        var px = pages * height + scrollRemainderPx
        if (Math.abs(px) < 1) {
            scrollRemainderPx = px
            return
        }
        scrollRemainderPx = 0

        var maxY = Math.max(contentHeight - height, 0)
        // 上一段動畫還沒跑完時從它的終點接著算，一直推著搖桿才不會少算距離
        var base = autoScrollAnimation.running ? autoScrollAnimation.to : contentY
        var target = Math.min(Math.max(base + px, 0), maxY)

        var wasParked = focusParked
        parkFocus(target, fromGamepad)
        if (target === contentY) {
            // 已經在頂或在底：沒有東西要捲，也不要讓計時器一直重來。只有這一次才把焦點收起來時（先前用滾輪或
            // 捲軸捲走、焦點留在畫面外）照樣起一次計時，等一下把它放回看得到的地方
            if (focusParked && !wasParked) {
                scrollFocusTimer.forward = px > 0
                scrollFocusTimer.restart()
            }
            return
        }

        scrollFocusTimer.forward = px > 0
        scrollFocusTimer.restart()

        autoScrollAnimation.from = contentY
        autoScrollAnimation.to = target
        autoScrollAnimation.restart()
    }

    // 焦點是不是在某個 popup（對話框、選單）裡：往上走不會經過主畫面（main.qml 的 stackView）也不會經過
    // 工具列（toolBar）的，就是掛在視窗 overlay 底下的 popup。沒有任何控制項有焦點時，焦點是視窗的根，不算。
    // （這幾個檔案匯入的是 QtQuick.Controls 2.2，沒有 Overlay 可以直接比。）
    function isFocusInPopup() {
        var start = Window.activeFocusItem
        for (var item = start; item; item = item.parent) {
            if (item === stackView || item === toolBar) {
                return false
            }
        }
        return !!start && !!start.parent
    }

    // 手把要求捲動時走這裡。這一頁不在最上層、或上面開著對話框時不捲：不然頁面會在對話框後面動
    function scrollFromGamepad(pages) {
        if (StackView.status === StackView.Active && !isFocusInPopup()) {
            scrollByPages(pages, true)
        }
    }

    // 頁面捲到 viewTop 時，item 是不是完整看得到（上下各留 margin）。已經捲到頂或到底時，
    // 貼著那一邊的項目不可能再留出邊距，這時只要求它完整看得到
    function isInViewAt(item, margin, viewTop) {
        var top = item.mapToItem(contentItem, 0, 0).y
        var bottom = top + item.height
        var viewBottom = viewTop + height
        return (top - margin >= viewTop || (viewTop <= 0 && top >= viewTop)) &&
               (bottom + margin <= viewBottom || (viewBottom >= contentHeight && bottom <= viewBottom))
    }

    // 頁面捲到 viewTop 之後，有焦點的控制項如果不再完整看得到，把焦點收到這一頁本身。
    // 文字輸入欄只有手把捲動時才收：用鍵盤捲的人可能正在裡面打字，焦點被拿走的話後面打的字會進到別的地方。
    function parkFocus(viewTop, fromGamepad) {
        var item = Window.activeFocusItem
        if (item === settingsPage) {
            // 焦點本來就在這一頁本身（沒有手把時開頁就是這樣）：之後按上下同樣從看得到的地方進去
            focusParked = true
            return
        }
        if (!item || !isChildOfFlickable(item) || isInViewAt(item, 0, viewTop)) {
            return
        }
        if (("cursorPosition" in item) && !fromGamepad) {
            return
        }
        // 正被按著的控制項不收：雷射或滑鼠按下到放開之間剛好來一次捲動的話，焦點一被拿走，那一下點擊就沒有作用。
        // 一定要寫 === true：TextField 的 pressed 是訊號，直接當條件永遠成立
        if (item.pressed === true) {
            return
        }

        settingsPage.forceActiveFocus()
        // 這兩個要在焦點移過來之後才設：下面焦點改變的處理看到別的項目拿到焦點時會把它們清掉
        scrollOrigin = item
        focusParked = true
    }

    // 把收在這一頁本身的焦點放到看得到的控制項上；有放成功回傳 true。
    // 原本那一個又完整看得到了（捲走又捲回來）就還給它；否則從它沿 Tab 順序往畫面所在的方向找第一個完整看得到的
    // 控制項（沒有原本那一個時，從這一頁的開頭往 forward 的方向找）。文字輸入欄跳過：手把的 A 在裡面會打出空白；
    // 有螢幕鍵盤的裝置上，焦點自己跑進去還可能把鍵盤叫出來。
    function enterView(forward) {
        if (!focusParked || Window.activeFocusItem !== settingsPage) {
            return false
        }

        // 看不看得到，用捲動的終點判斷（和 parkFocus 一樣）：動畫還在跑時用當下的位置挑，挑到的可能接著就被捲出去
        var viewTop = autoScrollAnimation.running ? autoScrollAnimation.to : contentY
        var target = null
        if (scrollOrigin && isInViewAt(scrollOrigin, 0, viewTop)) {
            target = scrollOrigin
        }
        else {
            // 和下面焦點改變時自動捲動留的邊距一致，焦點放上去時頁面才不會再動
            var margin = height > 100 ? 50 : 0
            var from = scrollOrigin ? scrollOrigin : settingsPage
            if (scrollOrigin) {
                // 往畫面所在的方向找（原本那一個在畫面上方就往後），不看按鍵或捲動的方向：
                // 反方向要繞完整圈 Tab 順序，會落在另一欄的另一端
                forward = scrollOrigin.mapToItem(contentItem, 0, 0).y < viewTop
            }
            var first = null
            var item = from
            // 走完一圈就停；次數上限只是防呆
            for (var i = 0; i < 500; i++) {
                item = item.nextItemInFocusChain(forward)
                if (!item || item === from || item === first) {
                    break
                }
                if (first === null) {
                    first = item
                }
                if (isChildOfFlickable(item) && !("cursorPosition" in item) && isInViewAt(item, margin, viewTop)) {
                    target = item
                    break
                }
            }
        }

        if (!target) {
            return false
        }

        focusParked = false
        scrollOrigin = null
        target.forceActiveFocus(Qt.TabFocus)
        return true
    }

    // 焦點收在這一頁本身時按了上下：馬上放到看得到的地方，不等計時器
    function enterViewFromKey(forward) {
        if (!focusParked) {
            return false
        }

        scrollFocusTimer.stop()
        // 捲動的動畫還在跑就先讓它到終點：焦點放上去時，下面焦點改變的處理是照頁面當下的位置算要不要再捲
        if (autoScrollAnimation.running) {
            autoScrollAnimation.complete()
        }
        return enterView(forward)
    }

    Timer {
        id: scrollFocusTimer
        property bool forward: true
        interval: 250
        onTriggered: enterView(forward)
    }

    Connections {
        target: SdlGamepadKeyNavigation

        function onScrollRequested(pages) {
            // Window／StackView 這類附加屬性要在這一頁自己的函式裡讀（Connections 不是 Item）
            settingsPage.scrollFromGamepad(pages)
        }
    }

    Keys.onPressed: function(event) {
        // 下拉選單開著時不接（ComboBox 自己不處理這兩個鍵，事件會傳上來）：不然整頁在選單後面捲走
        var focusItem = Window.activeFocusItem
        if (focusItem && focusItem.popup && focusItem.popup.visible) {
            return
        }

        if (event.key === Qt.Key_PageDown) {
            scrollByPages(0.8, false)
            event.accepted = true
        }
        else if (event.key === Qt.Key_PageUp) {
            scrollByPages(-0.8, false)
            event.accepted = true
        }
    }

    // 焦點收在這一頁本身時，上下是「進到看得到的地方」，不是從整個視窗的第一個控制項開始走。
    // 這兩個處理只有焦點就在這一頁本身時才會被叫到（控制項自己會處理 Tab）。手把的方向鍵上送的是帶 Shift 的 Tab，
    // 實體鍵盤的 Shift+Tab 是 Backtab，兩種都要接；沒有接手時要把 accepted 設回 false，讓原本的導覽照常進行。
    Keys.onTabPressed: function(event) {
        event.accepted = enterViewFromKey(!(event.modifiers & Qt.ShiftModifier))
    }

    Keys.onBacktabPressed: function(event) {
        event.accepted = enterViewFromKey(false)
    }

    Window.onActiveFocusItemChanged: {
        var item = Window.activeFocusItem
        if (item) {
            // §UI-SCROLL：焦點到了這一頁本身以外的地方（使用者點了某個控制項，或 enterView 放好了），
            // 「焦點收在這一頁」的狀態就結束
            if (item !== settingsPage) {
                focusParked = false
                scrollOrigin = null
            }

            // Ignore non-child elements like the toolbar buttons
            if (!isChildOfFlickable(item)) {
                return
            }

            // Map the focus item's position into our content item's coordinate space
            var pos = item.mapToItem(contentItem, 0, 0)

            // Ensure some extra space is visible around the element we're scrolling to
            var scrollMargin = height > 100 ? 50 : 0

            if (pos.y - scrollMargin < contentY) {
                autoScrollAnimation.from = contentY
                autoScrollAnimation.to = Math.max(pos.y - scrollMargin, 0)
                autoScrollAnimation.start()
            }
            else if (pos.y + item.height + scrollMargin > contentY + height) {
                autoScrollAnimation.from = contentY
                autoScrollAnimation.to = Math.min(pos.y + item.height + scrollMargin - height, contentHeight - height)
                autoScrollAnimation.start()
            }
        }
    }

    StackView.onActivated: {
        // This enables Tab and BackTab based navigation rather than arrow keys.
        // It is required to shift focus between controls on the settings page.
        SdlGamepadKeyNavigation.setUiNavMode(true)

        if (VrLauncher.available && VrLauncher.vrMode) {
            // §UI-SCROLL：Steam Frame 的 VR 模式下，這一頁一打開就捲到 VR 那一區，焦點也放在那一區的第一個選項。
            // 它排在左欄最底下，前面有幾十個選項。這裡不看有沒有偵測到手把：焦點留在頁面本身的話，
            // 之後第一次按方向鍵，焦點會走到最上面的控制項、整頁跟著捲回頂端。其他情況照舊從最上面開始。
            vrEyeComboBox.forceActiveFocus(Qt.TabFocus)
            // 焦點落下去時頁面只會捲到剛好露出那一個選項；這裡捲到整區的標題都看得到
            var groupTop = vrSettingsGroupBox.mapToItem(contentItem, 0, 0).y
            autoScrollAnimation.from = contentY
            autoScrollAnimation.to = Math.max(Math.min(groupTop - 10, contentHeight - height), 0)
            autoScrollAnimation.restart()
        }
        else if (SdlGamepadKeyNavigation.getConnectedGamepads() > 0) {
            // Highlight the first item if a gamepad is connected
            resolutionComboBox.forceActiveFocus(Qt.TabFocus)
        }
    }

    StackView.onDeactivating: {
        SdlGamepadKeyNavigation.setUiNavMode(false)

        // Save the prefs so the Session can observe the changes
        StreamingPreferences.save()
    }

    Component.onDestruction: {
        // Also save preferences on destruction, since we won't get a
        // deactivating callback if the user just closes VipleStream
        StreamingPreferences.save()
    }

    Column {
        padding: 10
        id: settingsColumn1
        width: settingsPage.width / 2
        spacing: 15

        GroupBox {
            id: basicSettingsGroupBox
            width: (parent.width - (parent.leftPadding + parent.rightPadding))
            padding: 12
            title: "<font color=\"#D4FF3A\">§ 01 · </font><font color=\"#F2F5E1\">" + qsTr("Basic Settings") + "</font>"
            font.pointSize: 12

            Column {
                anchors.fill: parent
                spacing: 5

                Label {
                    width: parent.width
                    id: resFPStitle
                    text: qsTr("Resolution and FPS")
                    font.pointSize: 12
                    wrapMode: Text.Wrap
                }

                Label {
                    width: parent.width
                    id: resFPSdesc
                    text: qsTr("Setting values too high for your PC or network connection may cause lag, stuttering, or errors.")
                    font.pointSize: 9
                    wrapMode: Text.Wrap
                }

                Row {
                    spacing: 5
                    width: parent.width

                    AutoResizingComboBox {
                        property int lastIndexValue

                        function addDetectedResolution(friendlyNamePrefix, rect) {
                            var indexToAdd = 0
                            for (var j = 0; j < resolutionComboBox.count; j++) {
                                var existing_width = parseInt(resolutionListModel.get(j).video_width);
                                var existing_height = parseInt(resolutionListModel.get(j).video_height);

                                if (rect.width === existing_width && rect.height === existing_height) {
                                    // Duplicate entry, skip
                                    indexToAdd = -1
                                    break
                                }
                                else if (rect.width * rect.height > existing_width * existing_height) {
                                    // Candidate entrypoint after this entry
                                    indexToAdd = j + 1
                                }
                            }

                            // Insert this display's resolution if it's not a duplicate
                            if (indexToAdd >= 0) {
                                resolutionListModel.insert(indexToAdd,
                                                           {
                                                               "text": friendlyNamePrefix+" ("+rect.width+"x"+rect.height+")",
                                                               "video_width": ""+rect.width,
                                                               "video_height": ""+rect.height,
                                                               "is_custom": false
                                                           })
                            }
                        }

                        // §H.4-AUTO：「自動（本機螢幕最佳）」在設定頁的估計值（實際串流值在 Session 依串流視窗
                        // 所在螢幕重算，並與 host 回報的最高模式取小）。這裡只用來顯示與算預設位元率。
                        function autoLocalResolution() {
                            var w = Math.round(Screen.width * Screen.devicePixelRatio)
                            var h = Math.round(Screen.height * Screen.devicePixelRatio)
                            var host_max = ComputerManager.getMaxHostDisplayMode()
                            if (host_max.width * host_max.height > 0 && w * h > host_max.width * host_max.height) {
                                w = host_max.width
                                h = host_max.height
                            }
                            return Qt.size(w, h)
                        }

                        // ignore setting the index at first, and actually set it when the component is loaded
                        Component.onCompleted: {
                            // Refresh display data before using it to build the list
                            SystemProperties.refreshDisplays()

                            // Add native and safe area resolutions for all attached displays
                            var done = false
                            for (var displayIndex = 0; !done; displayIndex++) {
                                var screenRect = SystemProperties.getNativeResolution(displayIndex);
                                var safeAreaRect = SystemProperties.getSafeAreaResolution(displayIndex);

                                if (screenRect.width === 0) {
                                    // Exceeded max count of displays
                                    done = true
                                    break
                                }

                                addDetectedResolution(qsTr("Native"), screenRect)
                                addDetectedResolution(qsTr("Native (Excluding Notch)"), safeAreaRect)
                            }

                            // Prune resolutions that are over the decoder's maximum
                            var max_pixels = SystemProperties.maximumResolution.width * SystemProperties.maximumResolution.height;

                            // VipleStream §H.4 — also prune resolutions that exceed every
                            // paired Sunshine host's largest physical display mode.  Returns
                            // QSize(0,0) when no host has advertised DisplayMode yet (no
                            // paired Viple host, or vanilla Sunshine without the §H.4 patch),
                            // in which case we keep the original decoder-only filter.
                            var host_max = ComputerManager.getMaxHostDisplayMode()
                            var host_max_pixels = host_max.width * host_max.height
                            if (host_max_pixels > 0) {
                                if (max_pixels === 0 || host_max_pixels < max_pixels) {
                                    max_pixels = host_max_pixels
                                }
                            }

                            if (max_pixels > 0) {
                                for (var j = 0; j < resolutionComboBox.count; j++) {
                                    var existing_width = parseInt(resolutionListModel.get(j).video_width);
                                    var existing_height = parseInt(resolutionListModel.get(j).video_height);

                                    if (existing_width * existing_height > max_pixels) {
                                        resolutionListModel.remove(j)
                                        j--
                                    }
                                }
                            }

                            // §H.4-AUTO：自動模式直接選「自動」項目（video_width 為 "0"），並把估計值同步回
                            // 偏好（位元率仍等於舊值的預設時才跟著重算，不動使用者自訂的位元率）
                            if (StreamingPreferences.autoResolution) {
                                for (var a = 0; a < resolutionListModel.count; a++) {
                                    if (parseInt(resolutionListModel.get(a).video_width) === 0) {
                                        currentIndex = a
                                        break
                                    }
                                }
                                resolutionListModel.append({
                                                               "text": qsTr("Custom"),
                                                               "video_width": "",
                                                               "video_height": "",
                                                               "is_custom": true
                                                           })
                                var est = autoLocalResolution()
                                if (est.width > 0 && est.height > 0 &&
                                        (StreamingPreferences.width !== est.width || StreamingPreferences.height !== est.height)) {
                                    var oldDefault = StreamingPreferences.getDefaultBitrate(StreamingPreferences.width,
                                                                                            StreamingPreferences.height,
                                                                                            StreamingPreferences.fps,
                                                                                            StreamingPreferences.enableYUV444,
                                                                                            StreamingPreferences.enableFrameInterpolation)
                                    var wasDefault = StreamingPreferences.bitrateKbps === oldDefault
                                    StreamingPreferences.width = est.width
                                    StreamingPreferences.height = est.height
                                    if (wasDefault) {
                                        StreamingPreferences.bitrateKbps = StreamingPreferences.getDefaultBitrate(est.width,
                                                                                                                  est.height,
                                                                                                                  StreamingPreferences.fps,
                                                                                                                  StreamingPreferences.enableYUV444,
                                                                                                                  StreamingPreferences.enableFrameInterpolation)
                                    }
                                }
                                recalculateWidth()
                                lastIndexValue = currentIndex
                                return
                            }

                            // load the saved width/height, and iterate through the ComboBox until a match is found
                            // and set it to that index.
                            var saved_width = StreamingPreferences.width
                            var saved_height = StreamingPreferences.height
                            var index_set = false
                            for (var i = 0; i < resolutionListModel.count; i++) {
                                var el_width = parseInt(resolutionListModel.get(i).video_width);
                                var el_height = parseInt(resolutionListModel.get(i).video_height);

                                if (saved_width === el_width && saved_height === el_height) {
                                    currentIndex = i
                                    index_set = true
                                    break
                                }
                            }

                            if (!index_set) {
                                // We did not find a match. This must be a custom resolution.
                                resolutionListModel.append({
                                                               "text": qsTr("Custom")+" ("+StreamingPreferences.width+"x"+StreamingPreferences.height+")",
                                                               "video_width": ""+StreamingPreferences.width,
                                                               "video_height": ""+StreamingPreferences.height,
                                                               "is_custom": true
                                                           })
                                currentIndex = resolutionListModel.count - 1
                            }
                            else {
                                resolutionListModel.append({
                                                               "text": qsTr("Custom"),
                                                               "video_width": "",
                                                               "video_height": "",
                                                               "is_custom": true
                                                           })
                            }

                            // Since we don't call activate() here, we need to trigger
                            // width calculation manually
                            recalculateWidth()

                            lastIndexValue = currentIndex
                        }

                        id: resolutionComboBox
                        maximumWidth: parent.width / 2
                        textRole: "text"
                        model: ListModel {
                            id: resolutionListModel
                            // Other elements may be added at runtime
                            // based on attached display resolution
                            ListElement {
                                text: qsTr("Auto (best for this display)")
                                video_width: "0"
                                video_height: "0"
                                is_custom: false
                            }
                            ListElement {
                                text: qsTr("720p")
                                video_width: "1280"
                                video_height: "720"
                                is_custom: false
                            }
                            ListElement {
                                text: qsTr("1080p")
                                video_width: "1920"
                                video_height: "1080"
                                is_custom: false
                            }
                            ListElement {
                                text: qsTr("1440p")
                                video_width: "2560"
                                video_height: "1440"
                                is_custom: false
                            }
                            ListElement {
                                text: qsTr("4K")
                                video_width: "3840"
                                video_height: "2160"
                                is_custom: false
                            }
                        }

                        function updateBitrateForSelection() {
                            var selectedWidth = parseInt(resolutionListModel.get(currentIndex).video_width)
                            var selectedHeight = parseInt(resolutionListModel.get(currentIndex).video_height)

                            // §H.4-AUTO：「自動」項目（寬高 0）→ 記住自動，寬高用本機螢幕估計值
                            var isAuto = (selectedWidth === 0)
                            StreamingPreferences.autoResolution = isAuto
                            if (isAuto) {
                                var est = autoLocalResolution()
                                selectedWidth = est.width
                                selectedHeight = est.height
                            }

                            // Only modify the bitrate if the values actually changed
                            if (StreamingPreferences.width !== selectedWidth || StreamingPreferences.height !== selectedHeight) {
                                StreamingPreferences.width = selectedWidth
                                StreamingPreferences.height = selectedHeight

                                if (StreamingPreferences.autoAdjustBitrate) {
                                    StreamingPreferences.bitrateKbps = StreamingPreferences.getDefaultBitrate(StreamingPreferences.width,
                                                                                                              StreamingPreferences.height,
                                                                                                              StreamingPreferences.fps,
                                                                                                              StreamingPreferences.enableYUV444,
                                                                                                              StreamingPreferences.enableFrameInterpolation);
                                    slider.value = StreamingPreferences.bitrateKbps
                                }
                            }

                            lastIndexValue = currentIndex
                        }

                        // ::onActivated must be used, as it only listens for when the index is changed by a human
                        onActivated : {
                            if (resolutionListModel.get(currentIndex).is_custom) {
                                customResolutionDialog.open()
                            }
                            else {
                                updateBitrateForSelection()
                            }
                        }

                        NavigableDialog {
                            id: customResolutionDialog
                            standardButtons: Dialog.Ok | Dialog.Cancel
                            onOpened: {
                                // Force keyboard focus on the textbox so keyboard navigation works
                                widthField.forceActiveFocus()

                                // standardButton() was added in Qt 5.10, so we must check for it first
                                if (customResolutionDialog.standardButton) {
                                    customResolutionDialog.standardButton(Dialog.Ok).enabled = customResolutionDialog.isInputValid()
                                }
                            }

                            onClosed: {
                                widthField.clear()
                                heightField.clear()
                            }

                            onRejected: {
                                resolutionComboBox.currentIndex = resolutionComboBox.lastIndexValue
                            }

                            function isInputValid() {
                                // If we have text in either textbox that isn't valid,
                                // reject the input.
                                if ((!widthField.acceptableInput && widthField.text) ||
                                        (!heightField.acceptableInput && heightField.text)) {
                                    return false
                                }

                                // The textboxes need to have text or placeholder text
                                if ((!widthField.text && !widthField.placeholderText) ||
                                        (!heightField.text && !heightField.placeholderText)) {
                                    return false
                                }

                                return true
                            }

                            onAccepted: {
                                // Reject if there's invalid input
                                if (!isInputValid()) {
                                    reject()
                                    return
                                }

                                var width = widthField.text ? widthField.text : widthField.placeholderText
                                var height = heightField.text ? heightField.text : heightField.placeholderText

                                // Find and update the custom entry
                                for (var i = 0; i < resolutionListModel.count; i++) {
                                    if (resolutionListModel.get(i).is_custom) {
                                        resolutionListModel.setProperty(i, "video_width", width)
                                        resolutionListModel.setProperty(i, "video_height", height)
                                        resolutionListModel.setProperty(i, "text", "Custom ("+width+"x"+height+")")

                                        // Now update the bitrate using the custom resolution
                                        resolutionComboBox.currentIndex = i
                                        resolutionComboBox.updateBitrateForSelection()

                                        // Update the combobox width too
                                        resolutionComboBox.recalculateWidth()
                                        break
                                    }
                                }
                            }

                            ColumnLayout {
                                Label {
                                    text: qsTr("Custom resolutions are not officially supported by GeForce Experience, so it will not set your host display resolution. You will need to set it manually while in game.") + "\n\n" +
                                          qsTr("Resolutions that are not supported by your client or host PC may cause streaming errors.") + "\n"
                                    wrapMode: Label.WordWrap
                                    Layout.maximumWidth: 300
                                }

                                Label {
                                    text: qsTr("Enter a custom resolution:")
                                    font.bold: true
                                }

                                RowLayout {
                                    TextField {
                                        id: widthField
                                        maximumLength: 5
                                        inputMethodHints: Qt.ImhDigitsOnly
                                        placeholderText: resolutionListModel.get(resolutionComboBox.currentIndex).video_width === "0" ? ""+StreamingPreferences.width : resolutionListModel.get(resolutionComboBox.currentIndex).video_width
                                        validator: IntValidator{bottom:256; top:8192}
                                        focus: true

                                        onTextChanged: {
                                            // standardButton() was added in Qt 5.10, so we must check for it first
                                            if (customResolutionDialog.standardButton) {
                                                customResolutionDialog.standardButton(Dialog.Ok).enabled = customResolutionDialog.isInputValid()
                                            }
                                        }

                                        Keys.onReturnPressed: {
                                            customResolutionDialog.accept()
                                        }

                                        Keys.onEnterPressed: {
                                            customResolutionDialog.accept()
                                        }
                                    }

                                    Label {
                                        text: "x"
                                        font.bold: true
                                    }

                                    TextField {
                                        id: heightField
                                        maximumLength: 5
                                        inputMethodHints: Qt.ImhDigitsOnly
                                        placeholderText: resolutionListModel.get(resolutionComboBox.currentIndex).video_height === "0" ? ""+StreamingPreferences.height : resolutionListModel.get(resolutionComboBox.currentIndex).video_height
                                        validator: IntValidator{bottom:256; top:8192}

                                        onTextChanged: {
                                            // standardButton() was added in Qt 5.10, so we must check for it first
                                            if (customResolutionDialog.standardButton) {
                                                customResolutionDialog.standardButton(Dialog.Ok).enabled = customResolutionDialog.isInputValid()
                                            }
                                        }

                                        Keys.onReturnPressed: {
                                            customResolutionDialog.accept()
                                        }

                                        Keys.onEnterPressed: {
                                            customResolutionDialog.accept()
                                        }
                                    }
                                }
                            }
                        }
                    }

                    AutoResizingComboBox {
                        property int lastIndexValue

                        // §H.4-AUTO：自動更新率在設定頁的估計值＝本視窗所在螢幕（找不到對應時用第 0 台）
                        // 回報的更新率；實際串流值在 Session 依串流視窗所在螢幕的目前更新率重算。
                        function autoLocalFps() {
                            var w = Math.round(Screen.width * Screen.devicePixelRatio)
                            var h = Math.round(Screen.height * Screen.devicePixelRatio)
                            var idx = 0
                            for (var d = 0; d < 16; d++) {
                                var r = SystemProperties.getNativeResolution(d)
                                if (r.width === 0) {
                                    break
                                }
                                if (r.width === w && r.height === h) {
                                    idx = d
                                    break
                                }
                            }
                            var hz = SystemProperties.getRefreshRate(idx)
                            return hz > 0 ? hz : 60
                        }

                        function updateBitrateForSelection() {
                            // Only modify the bitrate if the values actually changed
                            var selectedFps = parseInt(model.get(fpsComboBox.currentIndex).video_fps)
                            // §H.4-AUTO：「自動」項目（fps 0）
                            var isAuto = (selectedFps === 0)
                            StreamingPreferences.autoFps = isAuto
                            if (isAuto) {
                                selectedFps = autoLocalFps()
                            }
                            if (StreamingPreferences.fps !== selectedFps) {
                                StreamingPreferences.fps = selectedFps

                                if (StreamingPreferences.autoAdjustBitrate) {
                                    StreamingPreferences.bitrateKbps = StreamingPreferences.getDefaultBitrate(StreamingPreferences.width,
                                                                                                              StreamingPreferences.height,
                                                                                                              StreamingPreferences.fps,
                                                                                                              StreamingPreferences.enableYUV444,
                                                                                                              StreamingPreferences.enableFrameInterpolation);
                                    slider.value = StreamingPreferences.bitrateKbps
                                }
                            }

                            lastIndexValue = currentIndex
                        }

                        NavigableDialog {
                            function isInputValid() {
                                // If we have text that isn't valid, reject the input.
                                if (!fpsField.acceptableInput && fpsField.text) {
                                    return false
                                }

                                // The textbox needs to have text or placeholder text
                                if (!fpsField.text && !fpsField.placeholderText) {
                                    return false
                                }

                                return true
                            }

                            id: customFpsDialog
                            standardButtons: Dialog.Ok | Dialog.Cancel
                            onOpened: {
                                // Force keyboard focus on the textbox so keyboard navigation works
                                fpsField.forceActiveFocus()

                                // standardButton() was added in Qt 5.10, so we must check for it first
                                if (customFpsDialog.standardButton) {
                                    customFpsDialog.standardButton(Dialog.Ok).enabled = customFpsDialog.isInputValid()
                                }
                            }

                            onClosed: {
                                fpsField.clear()
                            }

                            onRejected: {
                                fpsComboBox.currentIndex = fpsComboBox.lastIndexValue
                            }

                            onAccepted: {
                                // Reject if there's invalid input
                                if (!isInputValid()) {
                                    reject()
                                    return
                                }

                                var fps = fpsField.text ? fpsField.text : fpsField.placeholderText

                                // Find and update the custom entry
                                for (var i = 0; i < fpsListModel.count; i++) {
                                    if (fpsListModel.get(i).is_custom) {
                                        fpsListModel.setProperty(i, "video_fps", fps)
                                        fpsListModel.setProperty(i, "text", qsTr("Custom (%1 FPS)").arg(fps))

                                        // Now update the bitrate using the custom resolution
                                        fpsComboBox.currentIndex = i
                                        fpsComboBox.updateBitrateForSelection()

                                        // Update the combobox width too
                                        fpsComboBox.recalculateWidth()
                                        break
                                    }
                                }
                            }

                            ColumnLayout {
                                Label {
                                    text: qsTr("Enter a custom frame rate:")
                                    font.bold: true
                                }

                                RowLayout {
                                    TextField {
                                        id: fpsField
                                        maximumLength: 4
                                        inputMethodHints: Qt.ImhDigitsOnly
                                        placeholderText: fpsListModel.get(fpsComboBox.currentIndex).video_fps === "0" ? ""+StreamingPreferences.fps : fpsListModel.get(fpsComboBox.currentIndex).video_fps
                                        validator: IntValidator{bottom:10; top:9999}
                                        focus: true

                                        onTextChanged: {
                                            // standardButton() was added in Qt 5.10, so we must check for it first
                                            if (customFpsDialog.standardButton) {
                                                customFpsDialog.standardButton(Dialog.Ok).enabled = customFpsDialog.isInputValid()
                                            }
                                        }

                                        Keys.onReturnPressed: {
                                            customFpsDialog.accept()
                                        }

                                        Keys.onEnterPressed: {
                                            customFpsDialog.accept()
                                        }
                                    }
                                }
                            }
                        }

                        function addRefreshRateOrdered(fpsListModel, refreshRate, description, custom) {
                            var indexToAdd = 0
                            for (var j = 0; j < fpsListModel.count; j++) {
                                var existing_fps = parseInt(fpsListModel.get(j).video_fps);

                                if (refreshRate === existing_fps || (custom && fpsListModel.get(j).is_custom)) {
                                    // Duplicate entry, skip
                                    indexToAdd = -1
                                    break
                                }
                                else if (refreshRate > existing_fps) {
                                    // Candidate entrypoint after this entry
                                    indexToAdd = j + 1
                                }
                            }

                            // Insert this frame rate if it's not a duplicate
                            if (indexToAdd >= 0) {
                                // Custom values always go at the end of the list
                                if (custom) {
                                    indexToAdd = fpsListModel.count
                                }

                                fpsListModel.insert(indexToAdd,
                                                    {
                                                        "text": description,
                                                        "video_fps": ""+refreshRate,
                                                        "is_custom": custom
                                                    })
                            }

                            return indexToAdd
                        }

                        function reinitialize() {
                            // VipleStream: populate the FPS dropdown with every
                            // refresh rate each attached display advertises at its
                            // desktop resolution (seed 30/60 are already in
                            // fpsListModel; addRefreshRateOrdered de-duplicates).
                            // Previous revision restricted the list to 30 / 60 /
                            // current-best only, which hid real modes (e.g. a VDD
                            // currently sitting at 60 Hz would not show its 90/120/
                            // 144 Hz options). We also re-enable the Custom FPS
                            // entry so users can target a rate above the panel's
                            // native refresh — useful with FRUC 2x where the
                            // decoded stream comes in at half the displayed rate.
                            var done = false
                            for (var displayIndex = 0; !done; displayIndex++) {
                                var rates = SystemProperties.getSupportedRefreshRates(displayIndex);
                                if (!rates || rates.length === 0) {
                                    // Exceeded max count of displays
                                    done = true
                                    break
                                }

                                for (var r = 0; r < rates.length; r++) {
                                    var refreshRate = rates[r];
                                    addRefreshRateOrdered(fpsListModel, refreshRate, qsTr("%1 FPS").arg(refreshRate), false)
                                }
                            }

                            // VipleStream: always append a single Custom entry at
                            // the end so the user can type any FPS (including
                            // rates above the panel's reported list, needed for
                            // FRUC 2x targeting 120/144 on a 60 Hz-reporting VDD).
                            // Seed video_fps with the highest known rate so the
                            // Custom dialog opens with a sensible placeholder
                            // instead of "0". We bypass addRefreshRateOrdered
                            // because its dedup-by-value rule would reject this
                            // entry whenever the seed happens to match an
                            // already-listed rate (e.g. 120 on a 120 Hz panel).
                            var hasCustom = false
                            var customSeed = 120
                            for (var c = 0; c < fpsListModel.count; c++) {
                                if (fpsListModel.get(c).is_custom) { hasCustom = true; break }
                                var v = parseInt(fpsListModel.get(c).video_fps)
                                if (v > customSeed) customSeed = v
                            }
                            if (!hasCustom) {
                                fpsListModel.append({
                                    "text": qsTr("Custom"),
                                    "video_fps": ""+customSeed,
                                    "is_custom": true
                                })
                            }

                            // §H.4-AUTO：自動模式直接選「自動」項目，估計值同步回偏好（位元率是預設值時才重算）
                            if (StreamingPreferences.autoFps) {
                                for (var a = 0; a < model.count; a++) {
                                    if (parseInt(model.get(a).video_fps) === 0) {
                                        currentIndex = a
                                        break
                                    }
                                }
                                var estFps = autoLocalFps()
                                if (StreamingPreferences.fps !== estFps) {
                                    var oldDefault = StreamingPreferences.getDefaultBitrate(StreamingPreferences.width,
                                                                                            StreamingPreferences.height,
                                                                                            StreamingPreferences.fps,
                                                                                            StreamingPreferences.enableYUV444,
                                                                                            StreamingPreferences.enableFrameInterpolation)
                                    var wasDefault = StreamingPreferences.bitrateKbps === oldDefault
                                    StreamingPreferences.fps = estFps
                                    if (wasDefault) {
                                        StreamingPreferences.bitrateKbps = StreamingPreferences.getDefaultBitrate(StreamingPreferences.width,
                                                                                                                  StreamingPreferences.height,
                                                                                                                  estFps,
                                                                                                                  StreamingPreferences.enableYUV444,
                                                                                                                  StreamingPreferences.enableFrameInterpolation)
                                    }
                                }
                                recalculateWidth()
                                lastIndexValue = currentIndex
                                return
                            }

                            var saved_fps = StreamingPreferences.fps
                            var found = false
                            for (var i = 0; i < model.count; i++) {
                                var el_fps = parseInt(model.get(i).video_fps);

                                // Look for a matching frame rate
                                if (saved_fps === el_fps) {
                                    currentIndex = i
                                    found = true
                                    break
                                }
                            }

                            // VipleStream: if saved_fps isn't in the
                            // display-reported list (e.g. user set
                            // Custom 180 FPS on a 60 Hz panel), show
                            // it inside the Custom entry instead of
                            // clamping down.  The previous code
                            // overwrote StreamingPreferences.fps with
                            // the nearest lower rate, silently
                            // destroying the user's choice every time
                            // Settings was opened (§K.15 bug).
                            if (!found) {
                                // Update the Custom entry to reflect the
                                // user's saved value so the dropdown
                                // displays "Custom (180 FPS)" correctly.
                                var customFound = false
                                for (var k = 0; k < model.count; k++) {
                                    if (fpsListModel.get(k).is_custom) {
                                        fpsListModel.setProperty(k, "video_fps", ""+saved_fps)
                                        fpsListModel.setProperty(k, "text", qsTr("Custom (%1 FPS)").arg(saved_fps))
                                        currentIndex = k
                                        customFound = true
                                        break
                                    }
                                }
                                // Fallback: if no Custom entry exists
                                // (shouldn't happen), pick the closest
                                // without overwriting the preference.
                                if (!customFound) {
                                    var bestIdx = 0
                                    var bestFps = parseInt(model.get(0).video_fps)
                                    for (var j = 0; j < model.count; j++) {
                                        var opt_fps = parseInt(model.get(j).video_fps)
                                        if (opt_fps <= saved_fps && opt_fps >= bestFps) {
                                            bestFps = opt_fps
                                            bestIdx = j
                                        }
                                    }
                                    currentIndex = bestIdx
                                    // DO NOT overwrite StreamingPreferences.fps —
                                    // the user's saved value is intentional.
                                }
                            }

                            recalculateWidth()

                            lastIndexValue = currentIndex
                        }

                        // ignore setting the index at first, and actually set it when the component is loaded
                        Component.onCompleted: {
                            reinitialize()
                            languageChanged.connect(reinitialize)
                        }

                        model: ListModel {
                            id: fpsListModel
                            // Other elements may be added at runtime
                            ListElement {
                                text: qsTr("Auto (this display)")
                                video_fps: "0"
                                is_custom: false
                            }
                            ListElement {
                                text: qsTr("30 FPS")
                                video_fps: "30"
                                is_custom: false
                            }
                            ListElement {
                                text: qsTr("60 FPS")
                                video_fps: "60"
                                is_custom: false
                            }
                        }

                        id: fpsComboBox
                        maximumWidth: parent.width / 2
                        textRole: "text"
                        // ::onActivated must be used, as it only listens for when the index is changed by a human
                        onActivated : {
                            if (model.get(currentIndex).is_custom) {
                                customFpsDialog.open()
                            }
                            else {
                                updateBitrateForSelection()
                            }
                        }
                    }
                }

                // §J.3.e.2.i (v1.4.137) — VipleStream renderer backend dropdown.
                //
                // Enum 值穩定：RS_VULKAN=0 / RS_D3D11=1 / RS_AUTO=2 (v1.4.137).
                // Model 順序跟 enum 不再 1:1，currentIndex 用 lookup 對應.
                // v1.4.137 加 RS_AUTO 並改成預設，行為 = 走標準 hwaccel cascade.
                Label {
                    width: parent.width
                    text: qsTr("Renderer")
                    font.pointSize: 12
                    wrapMode: Text.Wrap
                }

                // §UI-SCROLL：用 AutoResizingComboBox 而不是一般的 ComboBox——選單打開時它會把手把導覽切回上下鍵。
                // 一般的 ComboBox 在這一頁用手把打開之後，按上下送出的是 Tab，選單直接關掉、焦點跳到下一個控制項，
                // 選不了項目。
                AutoResizingComboBox {
                    id: rendererSelectionCombo
                    width: parent.width
                    font.pointSize: 12
                    textRole: "text"
                    model: ListModel {
                        id: rendererSelectionModel
                    }
                    Component.onCompleted: {
                        rendererSelectionModel.append({text: qsTr("Auto (recommended) — let the system pick the most stable hardware decoding path"), val: 2})
                        rendererSelectionModel.append({text: qsTr("Direct3D 11 — always use D3D11VA; works with the NV-OF / DirectML / NCNN interpolation engines"), val: 1})
                        rendererSelectionModel.append({text: qsTr("Vulkan [experimental] — built-in frame interpolation; compatibility issues on some systems (AMD APU / older NVIDIA drivers)"), val: 0})
                        // Lookup currentIndex by val (enum value) — model
                        // order is no longer 1:1 with enum integer.
                        for (var i = 0; i < rendererSelectionModel.count; i++) {
                            if (rendererSelectionModel.get(i).val === StreamingPreferences.rendererSelection) {
                                currentIndex = i
                                break
                            }
                        }
                    }
                    onActivated: {
                        StreamingPreferences.rendererSelection = rendererSelectionModel.get(currentIndex).val
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Auto (recommended): same as upstream Moonlight's hwaccel cascade. ffmpeg picks the first hardware decoder that initializes (usually D3D11VA on Windows). Choose this unless you have a specific need.\n\nDirect3D 11: always use D3D11VA; the interpolation engine can be Generic / NV-OF / DirectML / NCNN. It currently behaves the same as Auto, but is kept as a hard pin for a future option that stops the helper from switching to Vulkan.\n\nVulkan [experimental]: forces the Vulkan-first cascade + native Vulkan rendering pipeline + dual-present, with the built-in interpolation engine (same as Generic). Known issues: native vkCmdDecodeVideoKHR in ONLY mode causes an NVDEC device-lost on NVIDIA driver 596.36; the driver crashes during the 226 MB video session memory bind on AMD APUs; the PARALLEL+SW upload path is slow (80+ ms/frame). It will return to the recommended options once it is stable.")
                }

                // §F6 — Linux 視訊 frontend（畫面呈現端）。只在 Linux 顯示。
                //
                // 只決定 frontend；decoder cascade 仍由上面的「Renderer 渲染器」
                // 決定。Enum 值穩定：LVF_AUTO=0 / LVF_VULKAN=1 / LVF_EGL=2，
                // currentIndex 用 val lookup 對應（同 rendererSelectionCombo）。
                Label {
                    width: parent.width
                    visible: Qt.platform.os === "linux"
                    text: qsTr("Linux video output (frontend)")
                    font.pointSize: 12
                    wrapMode: Text.Wrap
                }

                AutoResizingComboBox {
                    id: linuxVideoFrontendCombo
                    visible: Qt.platform.os === "linux"
                    width: parent.width
                    font.pointSize: 12
                    textRole: "text"
                    model: ListModel {
                        id: linuxVideoFrontendModel
                    }
                    Component.onCompleted: {
                        linuxVideoFrontendModel.append({text: qsTr("Auto (recommended) — Vulkan on aarch64 or Zink, otherwise the original order"), val: StreamingPreferences.LVF_AUTO})
                        linuxVideoFrontendModel.append({text: qsTr("Vulkan (libplacebo) — prefer Vulkan output"), val: StreamingPreferences.LVF_VULKAN})
                        linuxVideoFrontendModel.append({text: qsTr("EGL — keep the original order (EGL / DRM / SDL)"), val: StreamingPreferences.LVF_EGL})
                        currentIndex = 0
                        for (var i = 0; i < linuxVideoFrontendModel.count; i++) {
                            if (linuxVideoFrontendModel.get(i).val === StreamingPreferences.linuxVideoFrontend) {
                                currentIndex = i
                                break
                            }
                        }
                    }
                    onActivated: {
                        StreamingPreferences.linuxVideoFrontend = linuxVideoFrontendModel.get(currentIndex).val
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Only chooses the frontend that presents the picture; the decoding path is still set by Renderer above.\n\nAuto (recommended): prefers Vulkan (libplacebo) on aarch64 (e.g. Steam Frame) or when GL runs on Zink; other platforms keep the original EGL / DRM / SDL order.\n\nVulkan: always prefer Vulkan (libplacebo) output, falling back to the original order if it fails.\n\nEGL: keep the original order without preferring Vulkan.")
                }

                CheckBox {
                    id: frameInterpolationCheck
                    width: parent.width
                    text: qsTr("Frame interpolation (FRUC)")
                    font.pointSize: 12
                    checked: StreamingPreferences.enableFrameInterpolation
                    // VipleStream: 180 fps cap removed in v1.2.57.  No
                    // hard upper limit any more — backend selection
                    // (especially Generic) handles 240 fps + on the
                    // benchmark machine.  Settings UI follows suit.
                    onCheckedChanged: {
                        console.log("[VIPLE-PREFS] FRUC checkbox onCheckedChanged:"
                            + " checked=" + checked
                            + " model=" + StreamingPreferences.enableFrameInterpolation)
                        // VipleStream: FRUC on/off changes the host-side
                        // encode fps (ON → fps/2) AND adds a source-
                        // quality headroom multiplier to the default
                        // bitrate (see streamingpreferences.cpp). If the
                        // user hasn't manually pinned a bitrate (i.e.
                        // autoAdjustBitrate is still on), re-derive the
                        // default so the slider follows the FRUC state.
                        // Otherwise respect their pinned value — matches
                        // how YUV444 / resolution / fps toggles behave.
                        if (StreamingPreferences.enableFrameInterpolation != checked) {
                            console.log("[VIPLE-PREFS] FRUC model updated: "
                                + StreamingPreferences.enableFrameInterpolation
                                + " → " + checked)
                            StreamingPreferences.enableFrameInterpolation = checked
                            if (StreamingPreferences.autoAdjustBitrate) {
                                StreamingPreferences.bitrateKbps = StreamingPreferences.getDefaultBitrate(
                                    StreamingPreferences.width,
                                    StreamingPreferences.height,
                                    StreamingPreferences.fps,
                                    StreamingPreferences.enableYUV444,
                                    StreamingPreferences.enableFrameInterpolation)
                                slider.value = StreamingPreferences.bitrateKbps
                            }
                        }
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Inserts an interpolated frame between each real frame, doubling the effective output frame rate. The actual server frame rate is halved (e.g. 120 FPS setting → request 60 FPS → output 120 FPS).")
                }

                // §J.3.e.2.i — Vulkan renderer 用內建 compute 補幀引擎，不
                //吃 frucBackend 選擇 (還沒接 IFRUCBackend 抽象)；D3D11 才需要
                // 選 backend.  顯示提示讓使用者知道 Vulkan 等同 Generic.
                Label {
                    width: parent.width
                    visible: frameInterpolationCheck.checked
                             && StreamingPreferences.rendererSelection === StreamingPreferences.RS_VULKAN
                    text: qsTr("The Vulkan renderer uses the built-in interpolation engine (same as Generic). Switch to Direct3D 11 to choose a different engine.")
                    font.pointSize: 10
                    font.italic: true
                    opacity: 0.75
                    wrapMode: Text.Wrap
                }

                // v1.4.153 §R2-γ — Vulkan 補幀模式: 主動 / 被動.
                // 主動 (default): server 推 UI/ratio fps + always-on dual present.
                //                  低 server 負載, 需要 client GPU 夠強.
                // 被動: server 推 UI fps 全速, client 看 recv% 動態升 ratio.
                //       適合 server 跟不上時, 從 1x pass-through → 2x → 3x 階梯升.
                Row {
                    width: parent.width
                    spacing: 8
                    visible: frameInterpolationCheck.checked
                             && StreamingPreferences.rendererSelection === StreamingPreferences.RS_VULKAN
                    Label {
                        text: qsTr("Interpolation mode:")
                        font.pointSize: 12
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    RadioButton {
                        id: frucActiveRadio
                        text: qsTr("Active (recommended)")
                        font.pointSize: 12
                        checked: !StreamingPreferences.vkfrucPassiveMode
                        onCheckedChanged: {
                            if (checked && StreamingPreferences.vkfrucPassiveMode) {
                                StreamingPreferences.vkfrucPassiveMode = false
                            }
                        }
                        ToolTip.delay: 1000
                        ToolTip.timeout: 5000
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("The host streams at the UI frame rate / 2 (or / 3 on high-refresh displays) and the client always interpolates 2x/3x. Low host load; needs a strong client GPU.")
                    }
                    RadioButton {
                        id: frucPassiveRadio
                        text: qsTr("Passive")
                        font.pointSize: 12
                        checked: StreamingPreferences.vkfrucPassiveMode
                        onCheckedChanged: {
                            if (checked && !StreamingPreferences.vkfrucPassiveMode) {
                                StreamingPreferences.vkfrucPassiveMode = true
                            }
                        }
                        ToolTip.delay: 1000
                        ToolTip.timeout: 5000
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("The host streams at the full UI frame rate. The client raises the ratio based on how many frames arrive: ≥ 70% pass-through, 40-70% 2x, < 40% 3x. Good when the host cannot always keep up.")
                    }
                }

                AutoResizingComboBox {
                    id: frucBackendCombo
                    width: parent.width
                    // Vulkan renderer 的 compute 補幀內建,不需 frucBackend.
                    // RS_AUTO 走標準 cascade（實際也是 D3D11VA on Windows），
                    // 所以跟 RS_D3D11 一樣需要選 backend.
                    visible: frameInterpolationCheck.checked
                             && StreamingPreferences.rendererSelection !== StreamingPreferences.RS_VULKAN
                    font.pointSize: 12
                    textRole: "text"
                    model: ListModel {
                        id: frucBackendModel
                    }
                    Component.onCompleted: {
                        // VipleStream v1.2.92: backend labels honestly
                        // reflect the v1.2.86+ reality. Generic remains
                        // the default & all-rounder. NvOF is good for
                        // up to ~120 fps on consumer NVIDIA cards (slower
                        // on workstation Ampere). DirectML now runs
                        // *real* RIFE ML inference and needs Tensor-Core
                        // class GPUs to keep up at higher resolutions —
                        // weak GPUs auto-cascade down to 360p or fall
                        // back to Generic, surprising users who picked
                        // it expecting "best quality". The GPU note
                        // below is the simplest way to set expectations
                        // up-front.
                        frucBackendModel.append({text: qsTr("Generic Compute (recommended — lowest latency, any GPU)"), val: 0})
                        frucBackendModel.append({text: qsTr("NVIDIA Optical Flow (HW flow + warp; NVIDIA RTX 20+)"), val: 1})
                        frucBackendModel.append({text: qsTr("DirectML — RIFE ML model (needs strong GPU; Tensor Core RTX 30/40+)"), val: 2})
                        frucBackendModel.append({text: qsTr("NCNN-Vulkan RIFE (experimental; cross-vendor: NV/AMD/Intel)"), val: 3})
                        currentIndex = StreamingPreferences.frucBackend
                    }
                    onActivated: {
                        StreamingPreferences.frucBackend = frucBackendModel.get(currentIndex).val
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Generic Compute: D3D11/GLES motion-estimation + warp shader. ~1-2ms latency, scales to 240 fps, works on every D3D11 GPU including iGPU. The default and best fit for most users.\n\nNVIDIA Optical Flow: dedicated NVOFA hardware on Turing+ (RTX 20/30/40 series). ~5ms on consumer cards, ~13ms on workstation Ampere (A1000-class). HW-accelerated optical flow gives slightly different artefact profile from Generic on disocclusion edges.\n\nDirectML: runs the bundled RIFE 4.25 lite ONNX model on the D3D12 DirectML EP — *real* motion-compensated ML interpolation, best visual quality on dynamic content. Per-op kernel-launch overhead (~25 ms baseline regardless of resolution) means it needs a Tensor-Core-class GPU (RTX 30/40 mainstream or better) to keep up at 720p+ in real time. On weaker GPUs the client auto-cascades the inference resolution down (1080p → 720p → 540p → 480p → 360p) and falls back to Generic if even 360p can't fit the frame budget.")
                }

                AutoResizingComboBox {
                    id: frucQualityCombo
                    width: parent.width
                    // Quality preset only applies to D3D11 + Generic
                    // Compute backend (shader-based). NVIDIA Optical
                    // Flow and DirectML both run their own internal
                    // pipelines that don't take a quality hint.
                    // Vulkan renderer 走自己內建 compute，不吃 quality hint.
                    // RS_AUTO 跟 RS_D3D11 一樣會吃 Generic backend，顯示 quality.
                    visible: frameInterpolationCheck.checked
                             && StreamingPreferences.rendererSelection !== StreamingPreferences.RS_VULKAN
                             && frucBackendCombo.currentIndex === 0
                    font.pointSize: 12
                    textRole: "text"
                    model: ListModel {
                        id: frucQualityModel
                    }
                    Component.onCompleted: {
                        frucQualityModel.append({text: qsTr("Quality — Best visual quality, higher GPU load"), val: 0})
                        frucQualityModel.append({text: qsTr("Balanced — Recommended (default)"), val: 1})
                        frucQualityModel.append({text: qsTr("Performance — Lowest latency, suitable for iGPU"), val: 2})
                        currentIndex = StreamingPreferences.frucQuality
                    }
                    onActivated: {
                        StreamingPreferences.frucQuality = frucQualityModel.get(currentIndex).val
                    }
                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Quality: 8-neighbor search + sub-pixel + adaptive blend (~12ms on iGPU)\nBalanced: 8-neighbor search + temporal smoothing (~8ms on iGPU)\nPerformance: 4-neighbor search + minimal processing (~6ms on iGPU)")
                }

                // §J.3.e.2.i.60 (v1.4.127) — Vulkan FRUC 子選項統一收進 autotier.
                // 舊 5 個 CheckBox / ComboBox (NV-OF / TRIPLE / Auto-tier / Manual
                // RIFE / inferDim) 全部移除. 啟用 FRUC = 直接走 unified autotier:
                // GPU 偵測 + benchmark 決定 cap tier, runtime chain_mean / chainBusy /
                // NVOF fails 沿 7 階梯 (T-1..T5) 動態升降. 各 GPU 在能力內最高品質.
                // Env vars 保留作 dev override (VIPLE_VKFRUC_TIER_CAP / TIER / NV_OF /
                // TRIPLE / NATIVE_RIFE / RIFE_INFER_DIM) — UI 不暴露.
                //
                // 顯示 read-only 偵測結果 + 當前 tier (autotier 自動更新 detectedTier
                // 給 UI 看; m_CurrentTier 是 runtime, 跑時才有意義).
                Label {
                    width: parent.width
                    visible: frameInterpolationCheck.checked
                             && StreamingPreferences.rendererSelection === StreamingPreferences.RS_VULKAN
                    font.pointSize: 11
                    wrapMode: Text.Wrap
                    text: {
                        var tier = StreamingPreferences.vkfrucDetectedTier
                        var tierName = tier === StreamingPreferences.VGT_ENTRY       ? qsTr("ENTRY (integrated / older GPUs)") :
                                       tier === StreamingPreferences.VGT_PERFORMANCE ? "PERFORMANCE (GTX 16xx, RX 5xxx)" :
                                       tier === StreamingPreferences.VGT_BALANCED    ? "BALANCED (RTX 20/30, RX 6700+, Arc A7)" :
                                       tier === StreamingPreferences.VGT_QUALITY     ? "QUALITY (RTX 40+, RX 7800+)" :
                                                                                       qsTr("UNKNOWN (not detected; restart the client to detect)")
                        var gpu = StreamingPreferences.vkfrucDetectedGpuName === ""
                                  ? qsTr("(not detected yet)")
                                  : StreamingPreferences.vkfrucDetectedGpuName
                        var benchMs = StreamingPreferences.vkfrucBenchmarkNs > 0
                                      ? (StreamingPreferences.vkfrucBenchmarkNs / 1e6).toFixed(2) + " ms"
                                      : qsTr("(not measured)")
                        return qsTr("FRUC auto tiering (Vulkan unified autotier v1.4.127):") + "\n"
                             + qsTr("   GPU: ")       + gpu      + "\n"
                             + qsTr("   Detected tier: ") + tierName + "\n"
                             + qsTr("   Benchmark: ") + benchMs  + "\n\n"
                             + qsTr("At runtime it moves up and down 7 tiers (T-1..T5):") + "\n"
                             + qsTr("  T5 = NVOF + TRIPLE + chain_lv3 + RIFE 256 (QUALITY GPU)") + "\n"
                             + qsTr("  T4 = NVOF + chain_lv3 + RIFE 128 (BALANCED GPU)") + "\n"
                             + qsTr("  T3 = NVOF + chain_lv2 (NVOF GPU, chain-heavy)") + "\n"
                             + qsTr("  T2 = chain_lv2 (AMD/Intel, no NVOF)") + "\n"
                             + qsTr("  T1 = chain_lv1 (PERFORMANCE GPU)") + "\n"
                             + qsTr("  T0 = SYNC chain (minimum FRUC)") + "\n"
                             + qsTr("  T-1 = FRUC paused (GPU too slow; retry after 5 min)")
                    }
                }
                // (NV-OF / TRIPLE / Auto-tier / Manual RIFE / inferDim) 舊選項移除.

                // §β.11.b — edge-aware MV 閾值 slider（Vulkan 補幀 warp shader 參數）。
                Row {
                    width: parent.width
                    spacing: 8
                    visible: frameInterpolationCheck.checked
                             && StreamingPreferences.rendererSelection === StreamingPreferences.RS_VULKAN
                    Label {
                        text: qsTr("Edge MV threshold:")
                        font.pointSize: 11
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    SpinBox {
                        id: edgeMvThresholdSpinBox
                        from: 2; to: 20
                        value: Math.round(StreamingPreferences.vkfrucEdgeMvThreshold)
                        font.pointSize: 11
                        onValueChanged: {
                            if (Math.abs(StreamingPreferences.vkfrucEdgeMvThreshold - value) > 0.01)
                                StreamingPreferences.vkfrucEdgeMvThreshold = value
                        }
                        ToolTip.delay: 1000
                        ToolTip.timeout: 7000
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Edge-detection threshold for motion vectors in the warp shader (in L² diff units).\n"
                            + "Default 8 (= 2.83 px L2 diff).\n"
                            + "Lower (4): more edge protection and less blocky mosaic, but edges may look softer.\n"
                            + "Higher (12-16): sharper edges, but high-contrast motion boundaries may keep some mosaic.\n"
                            + "Start with the default; if interpolated edges look blocky, lower it to 4-6.")
                    }
                }

                // VipleStream v1.2.92: 180 fps cap warning removed
                // along with the cap itself.  Generic FRUC happily
                // does 240 fps on the benchmark machine; the old red
                // ⚠ label was a leftover from the v1.2.56-era cap.

                Label {
                    width: parent.width
                    id: bitrateTitle
                    text: qsTr("Video bitrate:")
                    font.pointSize: 12
                    wrapMode: Text.Wrap
                }

                Label {
                    width: parent.width
                    id: bitrateDesc
                    text: qsTr("Lower the bitrate on slower connections. Raise the bitrate to increase image quality.")
                    font.pointSize: 9
                    wrapMode: Text.Wrap
                }

                Row {
                    width: parent.width
                    spacing: 5

                    Slider {
                        id: slider

                        value: StreamingPreferences.bitrateKbps

                        stepSize: 500
                        from : 500
                        to: StreamingPreferences.unlockBitrate ? 500000 : 150000

                        snapMode: "SnapOnRelease"
                        width: Math.min(bitrateDesc.implicitWidth, parent.width - (resetBitrateButton.visible ? resetBitrateButton.width + parent.spacing : 0))

                        onValueChanged: {
                            bitrateTitle.text = qsTr("Video bitrate: %1 Mbps").arg(value / 1000.0)
                            StreamingPreferences.bitrateKbps = value
                        }

                        onMoved: {
                            StreamingPreferences.autoAdjustBitrate = false
                        }

                        Component.onCompleted: {
                            // Refresh the text after translations change
                            languageChanged.connect(valueChanged)
                        }
                    }

                    Button {
                        id: resetBitrateButton
                        text: qsTr("Use Default (%1 Mbps)").arg(StreamingPreferences.getDefaultBitrate(StreamingPreferences.width, StreamingPreferences.height, StreamingPreferences.fps, StreamingPreferences.enableYUV444, StreamingPreferences.enableFrameInterpolation) / 1000.0)
                        visible: StreamingPreferences.bitrateKbps !== StreamingPreferences.getDefaultBitrate(StreamingPreferences.width, StreamingPreferences.height, StreamingPreferences.fps, StreamingPreferences.enableYUV444, StreamingPreferences.enableFrameInterpolation)
                        onClicked: {
                            var defaultBitrate = StreamingPreferences.getDefaultBitrate(StreamingPreferences.width, StreamingPreferences.height, StreamingPreferences.fps, StreamingPreferences.enableYUV444, StreamingPreferences.enableFrameInterpolation)
                            StreamingPreferences.bitrateKbps = defaultBitrate
                            StreamingPreferences.autoAdjustBitrate = true
                            slider.value = defaultBitrate
                        }
                    }
                }

                Label {
                    width: parent.width
                    id: windowModeTitle
                    text: qsTr("Display mode")
                    font.pointSize: 12
                    wrapMode: Text.Wrap
                    visible: SystemProperties.hasDesktopEnvironment
                }

                AutoResizingComboBox {
                    function createModel() {
                        var model = Qt.createQmlObject('import QtQuick 2.0; ListModel {}', parent, '')

                        model.append({
                                         text: qsTr("Fullscreen"),
                                         val: StreamingPreferences.WM_FULLSCREEN
                                     })

                        model.append({
                                         text: qsTr("Borderless windowed"),
                                         val: StreamingPreferences.WM_FULLSCREEN_DESKTOP
                                     })

                        model.append({
                                         text: qsTr("Windowed"),
                                         val: StreamingPreferences.WM_WINDOWED
                                     })


                        // Set the recommended option based on the OS
                        for (var i = 0; i < model.count; i++) {
                            var thisWm = model.get(i).val;
                            if (thisWm === StreamingPreferences.recommendedFullScreenMode) {
                                model.get(i).text += " " + qsTr("(Recommended)")
                                model.move(i, 0, 1)
                                break
                            }
                        }

                        return model
                    }


                    // This is used on initialization and upon retranslation
                    function reinitialize() {
                        if (!visible) {
                            // Do nothing if the control won't even be visible
                            return
                        }

                        model = createModel()
                        currentIndex = 0

                        // Set the current value based on the saved preferences
                        var savedWm = StreamingPreferences.windowMode
                        for (var i = 0; i < model.count; i++) {
                             var thisWm = model.get(i).val;
                             if (savedWm === thisWm) {
                                 currentIndex = i
                                 break
                             }
                        }

                        activated(currentIndex)
                    }

                    Component.onCompleted: {
                        reinitialize()
                        languageChanged.connect(reinitialize)
                    }

                    id: windowModeComboBox
                    visible: SystemProperties.hasDesktopEnvironment
                    enabled: !SystemProperties.rendererAlwaysFullScreen
                    hoverEnabled: true
                    textRole: "text"
                    onActivated: {
                        StreamingPreferences.windowMode = model.get(currentIndex).val
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Fullscreen generally provides the best performance, but borderless windowed may work better with features like macOS Spaces, Alt+Tab, screenshot tools, on-screen overlays, etc.")
                }

                CheckBox {
                    id: vsyncCheck
                    width: parent.width
                    hoverEnabled: true
                    text: qsTr("V-Sync")
                    font.pointSize:  12
                    checked: StreamingPreferences.enableVsync
                    onCheckedChanged: {
                        StreamingPreferences.enableVsync = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Disabling V-Sync allows sub-frame rendering latency, but it can display visible tearing")
                }

                CheckBox {
                    id: framePacingCheck
                    width: parent.width
                    hoverEnabled: true
                    text: qsTr("Frame pacing")
                    font.pointSize:  12
                    // §P1-PACE: 節拍不再綁 V-Sync。Pacer 是等真實 vblank
                    // 事件來決定何時送幀，跟 Present 要不要擋是兩回事；
                    // 綁在一起會讓「關 V-Sync 求低延遲」的使用者同時失去
                    // 唯一能吸收網路到達抖動的機制。
                    checked: StreamingPreferences.framePacing
                    onCheckedChanged: {
                        StreamingPreferences.framePacing = checked
                    }
                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Frame pacing reduces micro-stutter by delaying frames that come in too early. It works with V-Sync off too, so you keep low latency while smoothing out uneven frame arrival.")
                }
            }
        }

        GroupBox {

            id: audioSettingsGroupBox
            width: (parent.width - (parent.leftPadding + parent.rightPadding))
            padding: 12
            title: "<font color=\"#D4FF3A\">§ 02 · </font><font color=\"#F2F5E1\">" + qsTr("Audio Settings") + "</font>"
            font.pointSize: 12

            Column {
                anchors.fill: parent
                spacing: 5

                Label {
                    width: parent.width
                    id: resAudioTitle
                    text: qsTr("Audio configuration")
                    font.pointSize: 12
                    wrapMode: Text.Wrap
                }

                AutoResizingComboBox {
                    // ignore setting the index at first, and actually set it when the component is loaded
                    Component.onCompleted: {
                        var saved_audio = StreamingPreferences.audioConfig
                        currentIndex = 0
                        for (var i = 0; i < audioListModel.count; i++) {
                            var el_audio = audioListModel.get(i).val;
                            if (saved_audio === el_audio) {
                                currentIndex = i
                                break
                            }
                        }
                        activated(currentIndex)
                    }

                    id: audioComboBox
                    textRole: "text"
                    model: ListModel {
                        id: audioListModel
                        ListElement {
                            text: qsTr("Stereo")
                            val: StreamingPreferences.AC_STEREO
                        }
                        ListElement {
                            text: qsTr("5.1 surround sound")
                            val: StreamingPreferences.AC_51_SURROUND
                        }
                        ListElement {
                            text: qsTr("7.1 surround sound")
                            val: StreamingPreferences.AC_71_SURROUND
                        }
                    }
                    // ::onActivated must be used, as it only listens for when the index is changed by a human
                    onActivated : {
                        StreamingPreferences.audioConfig = audioListModel.get(currentIndex).val
                    }
                }


                CheckBox {
                    id: audioPcCheck
                    width: parent.width
                    text: qsTr("Mute host PC speakers while streaming")
                    font.pointSize: 12
                    checked: !StreamingPreferences.playAudioOnHost
                    onCheckedChanged: {
                        StreamingPreferences.playAudioOnHost = !checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("You must restart any game currently in progress for this setting to take effect")
                }

                CheckBox {
                    id: muteOnFocusLossCheck
                    width: parent.width
                    text: qsTr("Mute audio stream when VipleStream is not the active window")
                    font.pointSize: 12
                    visible: SystemProperties.hasDesktopEnvironment
                    checked: StreamingPreferences.muteOnFocusLoss
                    onCheckedChanged: {
                        StreamingPreferences.muteOnFocusLoss = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Mutes VipleStream's audio when you Alt+Tab out of the stream or click on a different window.")
                }
            }
        }

        GroupBox {
            id: hostSettingsGroupBox
            width: (parent.width - (parent.leftPadding + parent.rightPadding))
            padding: 12
            title: "<font color=\"#D4FF3A\">§ 03 · </font><font color=\"#F2F5E1\">" + qsTr("Host Settings") + "</font>"
            font.pointSize: 12

            Column {
                anchors.fill: parent
                spacing: 5

                CheckBox {
                    id: optimizeGameSettingsCheck
                    width: parent.width
                    text: qsTr("Optimize game settings for streaming")
                    font.pointSize:  12
                    checked: StreamingPreferences.gameOptimizations
                    onCheckedChanged: {
                        StreamingPreferences.gameOptimizations = checked
                    }
                }

                CheckBox {
                    id: quitAppAfter
                    width: parent.width
                    text: qsTr("Quit app on host PC after ending stream")
                    font.pointSize: 12
                    checked: StreamingPreferences.quitAppAfter
                    onCheckedChanged: {
                        StreamingPreferences.quitAppAfter = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("This will close the app or game you are streaming when you end your stream. You will lose any unsaved progress!")
                }

                Label {
                    width: parent.width
                    text: qsTr("NAT Traversal (Signaling Relay)")
                    font.pointSize: 12
                    font.bold: true
                    topPadding: 10
                }

                TextField {
                    id: relayUrlField
                    width: parent.width
                    font.pointSize: 11
                    placeholderText: qsTr("Relay URL (e.g. ws://relay.example.com:9999)")
                    text: StreamingPreferences.relayUrl
                    onEditingFinished: {
                        StreamingPreferences.relayUrl = text
                    }
                }

                TextField {
                    id: relayPskField
                    width: parent.width
                    font.pointSize: 11
                    placeholderText: qsTr("Relay PSK (pre-shared key, leave empty if none)")
                    text: StreamingPreferences.relayPsk
                    echoMode: TextInput.Password
                    onEditingFinished: {
                        StreamingPreferences.relayPsk = text
                    }
                }

                Label {
                    width: parent.width
                    text: qsTr("Optional: helps connect when both sides are behind NAT. Run viplestream-relay on a server with a public IP.")
                    font.pointSize: 9
                    color: "gray"
                    wrapMode: Text.Wrap
                }

                CheckBox {
                    id: forceRelayStreamCheck
                    width: parent.width
                    text: qsTr("Force streaming via relay (for testing UDP tunnel)")
                    font.pointSize: 11
                    checked: StreamingPreferences.forceRelayStream
                    onCheckedChanged: {
                        StreamingPreferences.forceRelayStream = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 6000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Always stream via the relay even when the host is directly reachable. Useful for verifying the relay UDP tunnel when a VPN (e.g. Cloudflare WARP) would otherwise keep direct /launch working.")
                }
            }
        }

        GroupBox {
            id: uiSettingsGroupBox
            width: (parent.width - (parent.leftPadding + parent.rightPadding))
            padding: 12
            title: "<font color=\"#D4FF3A\">§ 04 · </font><font color=\"#F2F5E1\">" + qsTr("UI Settings") + "</font>"
            font.pointSize: 12

            Column {
                anchors.fill: parent
                spacing: 5

                // VipleStream editorial design variant — Safe (quiet
                // grid) vs Bold (magazine-cover energy). Applies to the
                // app masthead height + title typography; other screens
                // inherit the global theme and are unaffected here.
                Label {
                    width: parent.width
                    id: designVariantTitle
                    text: qsTr("Design")
                    font.pointSize: 12
                    wrapMode: Text.Wrap
                }
                Label {
                    width: parent.width
                    id: designVariantLabel
                    text: qsTr("Safe: editorial grid, quieter masthead.\nBold: magazine-cover energy, oversized display titles.")
                    font.pointSize: 9
                    color: window.theme.mute
                    wrapMode: Text.Wrap
                }
                AutoResizingComboBox {
                    id: designVariantComboBox
                    textRole: "text"
                    Component.onCompleted: {
                        currentIndex = StreamingPreferences.designVariant
                    }
                    onActivated: {
                        StreamingPreferences.designVariant = currentIndex
                    }
                    model: ListModel {
                        id: designVariantListModel
                        ListElement { text: qsTr("Safe"); val: 0 }
                        ListElement { text: qsTr("Bold"); val: 1 }
                    }
                }

                Label {
                    width: parent.width
                    id: languageTitle
                    text: qsTr("Language")
                    font.pointSize: 12
                    wrapMode: Text.Wrap
                }

                AutoResizingComboBox {
                    // ignore setting the index at first, and actually set it when the component is loaded
                    Component.onCompleted: {
                        var saved_language = StreamingPreferences.language
                        currentIndex = 0
                        for (var i = 0; i < languageListModel.count; i++) {
                            var el_language = languageListModel.get(i).val;
                            if (saved_language === el_language) {
                                currentIndex = i
                                break
                            }
                        }

                        activated(currentIndex)
                    }

                    id: languageComboBox
                    textRole: "text"
                    model: ListModel {
                        id: languageListModel
                        ListElement {
                            text: qsTr("Automatic")
                            val: StreamingPreferences.LANG_AUTO
                        }
                        ListElement {
                            text: "Deutsch" // German
                            val: StreamingPreferences.LANG_DE
                        }
                        ListElement {
                            text: "English"
                            val: StreamingPreferences.LANG_EN
                        }
                        ListElement {
                            text: "Français" // French
                            val: StreamingPreferences.LANG_FR
                        }
                        ListElement {
                            text: "简体中文" // Simplified Chinese
                            val: StreamingPreferences.LANG_ZH_CN
                        }
                        ListElement {
                            text: "Norwegian Bokmål"
                            val: StreamingPreferences.LANG_NB_NO
                        }
                        ListElement {
                            text: "русский" // Russian
                            val: StreamingPreferences.LANG_RU
                        }
                        ListElement {
                            text: "Español" // Spanish
                            val: StreamingPreferences.LANG_ES
                        }
                        ListElement {
                            text: "日本語" // Japanese
                            val: StreamingPreferences.LANG_JA
                        }
                        ListElement {
                            text: "Tiếng Việt" // Vietnamese
                            val: StreamingPreferences.LANG_VI
                        }
                        ListElement {
                            text: "ภาษาไทย" // Thai
                            val: StreamingPreferences.LANG_TH
                        }
                        ListElement {
                            text: "한국어" // Korean
                            val: StreamingPreferences.LANG_KO
                        }
                        ListElement {
                            text: "Magyar" // Hungarian
                            val: StreamingPreferences.LANG_HU
                        }
                        ListElement {
                            text: "Nederlands" // Dutch
                            val: StreamingPreferences.LANG_NL
                        }
                        ListElement {
                            text: "Svenska" // Swedish
                            val: StreamingPreferences.LANG_SV
                        }
                        ListElement {
                            text: "Türkçe" // Turkish
                            val: StreamingPreferences.LANG_TR
                        }
                        /* ListElement {
                            text: "Українська" // Ukrainian
                            val: StreamingPreferences.LANG_UK
                        } */
                        ListElement {
                            text: "繁體中文" // Traditional Chinese
                            val: StreamingPreferences.LANG_ZH_TW
                        }
                        ListElement {
                            text: "Português" // Portuguese
                            val: StreamingPreferences.LANG_PT
                        }
                        /* ListElement {
                            text: "Português do Brasil" // Brazilian Portuguese
                            val: StreamingPreferences.LANG_PT_BR
                        } */
                        ListElement {
                            text: "Ελληνικά" // Greek
                            val: StreamingPreferences.LANG_EL
                        }
                        ListElement {
                            text: "Italiano" // Italian
                            val: StreamingPreferences.LANG_IT
                        }
                        /* ListElement {
                            text: "हिन्दी, हिंदी" // Hindi
                            val: StreamingPreferences.LANG_HI
                        } */
                        ListElement {
                            text: "Język polski" // Polish
                            val: StreamingPreferences.LANG_PL
                        }
                        ListElement {
                            text: "Čeština" // Czech
                            val: StreamingPreferences.LANG_CS
                        }
                        /* ListElement {
                            text: "עִבְרִית" // Hebrew
                            val: StreamingPreferences.LANG_HE
                        } */
                        /* ListElement {
                            text: "کرمانجیی خواروو" // Central Kurdish
                            val: StreamingPreferences.LANG_CKB
                        } */
                        /* ListElement {
                            text: "Lietuvių kalba" // Lithuanian
                            val: StreamingPreferences.LANG_LT
                        } */
                        /* ListElement {
                            text: "Eesti" // Estonian
                            val: StreamingPreferences.LANG_ET
                        } */
                        ListElement {
                            text: "Български" // Bulgarian
                            val: StreamingPreferences.LANG_BG
                        }
                        /* ListElement {
                            text: "Esperanto"
                            val: StreamingPreferences.LANG_EO
                        } */
                        ListElement {
                            text: "தமிழ்" // Tamil
                            val: StreamingPreferences.LANG_TA
                        }
                    }
                    // ::onActivated must be used, as it only listens for when the index is changed by a human
                    onActivated : {
                        // Retranslating is expensive, so only do it if the language actually changed
                        var new_language = languageListModel.get(currentIndex).val
                        if (StreamingPreferences.language !== new_language) {
                            StreamingPreferences.language = languageListModel.get(currentIndex).val
                            if (!StreamingPreferences.retranslate()) {
                                ToolTip.show(qsTr("You must restart VipleStream for this change to take effect"), 5000)
                            }
                            else {
                                // Force the back operation to pop any AppView pages that exist.
                                // The AppView stops working after retranslate() for some reason.
                                window.clearOnBack = true

                                // Signal other controls to adjust their text
                                languageChanged()
                            }
                        }
                    }
                }

                Label {
                    width: parent.width
                    id: uiDisplayModeTitle
                    text: qsTr("GUI display mode")
                    font.pointSize: 12
                    wrapMode: Text.Wrap
                    visible: SystemProperties.hasDesktopEnvironment
                }

                AutoResizingComboBox {
                    // ignore setting the index at first, and actually set it when the component is loaded
                    Component.onCompleted: {
                        if (!visible) {
                            // Do nothing if the control won't even be visible
                            return
                        }

                        var saved_uidisplaymode = StreamingPreferences.uiDisplayMode
                        currentIndex = 0
                        for (var i = 0; i < uiDisplayModeListModel.count; i++) {
                            var el_uidisplaymode = uiDisplayModeListModel.get(i).val;
                            if (saved_uidisplaymode === el_uidisplaymode) {
                                currentIndex = i
                                break
                            }
                        }

                        activated(currentIndex)
                    }

                    id: uiDisplayModeComboBox
                    visible: SystemProperties.hasDesktopEnvironment
                    textRole: "text"
                    model: ListModel {
                        id: uiDisplayModeListModel
                        ListElement {
                            text: qsTr("Windowed")
                            val: StreamingPreferences.UI_WINDOWED
                        }
                        ListElement {
                            text: qsTr("Maximized")
                            val: StreamingPreferences.UI_MAXIMIZED
                        }   
                        ListElement {
                            text: qsTr("Fullscreen")
                            val: StreamingPreferences.UI_FULLSCREEN
                        }
                    }
                    // ::onActivated must be used, as it only listens for when the index is changed by a human
                    onActivated : {
                        StreamingPreferences.uiDisplayMode = uiDisplayModeListModel.get(currentIndex).val
                    }
                }

                CheckBox {
                    id: connectionWarningsCheck
                    width: parent.width
                    text: qsTr("Show connection quality warnings")
                    font.pointSize: 12
                    checked: StreamingPreferences.connectionWarnings
                    onCheckedChanged: {
                        StreamingPreferences.connectionWarnings = checked
                    }
                }

                CheckBox {
                    id: configurationWarningsCheck
                    width: parent.width
                    text: qsTr("Show configuration warnings")
                    font.pointSize: 12
                    checked: StreamingPreferences.configurationWarnings
                    onCheckedChanged: {
                        StreamingPreferences.configurationWarnings = checked
                    }
                }

                CheckBox {
                    visible: SystemProperties.hasDiscordIntegration
                    id: discordPresenceCheck
                    width: parent.width
                    text: qsTr("Discord Rich Presence integration")
                    font.pointSize: 12
                    checked: StreamingPreferences.richPresence
                    onCheckedChanged: {
                        StreamingPreferences.richPresence = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Updates your Discord status to display the name of the game you're streaming. The status reads \"Playing <game> · via VipleStream\" with the VipleStream icon.")
                }

                CheckBox {
                    id: keepAwakeCheck
                    width: parent.width
                    text: qsTr("Keep the display awake while streaming")
                    font.pointSize: 12
                    checked: StreamingPreferences.keepAwake
                    onCheckedChanged: {
                        StreamingPreferences.keepAwake = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Prevents the screensaver from starting or the display from going to sleep while streaming.")
                }
            }
        }

        // VR（Steam Frame PCVR）：從 GUI 啟動的 VR 串流是另一個行程，讀的是這裡存下來的值
        GroupBox {
            id: vrSettingsGroupBox
            width: (parent.width - (parent.leftPadding + parent.rightPadding))
            padding: 12
            title: "<font color=\"#D4FF3A\">§ VR · </font><font color=\"#F2F5E1\">" + qsTr("VR Streaming (Steam Frame)") + "</font>"
            font.pointSize: 12

            Column {
                anchors.fill: parent
                spacing: 5

                Label {
                    width: parent.width
                    text: qsTr("Resolution per eye")
                    font.pointSize: 12
                    wrapMode: Text.Wrap
                }

                // §UI-SCROLL：這一區的四個下拉選單同樣用 AutoResizingComboBox（手把才能在選單裡上下選），
                // 寬度跟著最長的選項，初始化時要自己算一次
                AutoResizingComboBox {
                    id: vrEyeComboBox
                    property var values: [1152, 1440, 1728, 2160]
                    model: ["1152 x 1152", "1440 x 1440", "1728 x 1728", "2160 x 2160"]
                    Component.onCompleted: {
                        var i = values.indexOf(StreamingPreferences.vrEyeWidth)
                        currentIndex = i >= 0 ? i : values.length - 1
                        recalculateWidth()
                    }
                    onActivated: {
                        StreamingPreferences.vrEyeWidth = values[currentIndex]
                        StreamingPreferences.vrEyeHeight = values[currentIndex]
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 8000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Fewer pixels to encode, send and decode means lower latency. The host can still render at a higher resolution and scale it down.")
                }

                Label {
                    width: parent.width
                    text: qsTr("Extra field of view (overscan)")
                    font.pointSize: 12
                    wrapMode: Text.Wrap
                }

                AutoResizingComboBox {
                    id: vrOverscanComboBox
                    property var values: [0, 2, 3, 4, 5]
                    model: [qsTr("Off"), "2\u00b0", "3\u00b0", "4\u00b0", "5\u00b0"]
                    Component.onCompleted: {
                        var i = values.indexOf(Math.round(StreamingPreferences.vrOverscanDeg))
                        currentIndex = i >= 0 ? i : 0
                        recalculateWidth()
                    }
                    onActivated: {
                        StreamingPreferences.vrOverscanDeg = values[currentIndex]
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 8000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Renders a little beyond the edge of each eye so that turning your head does not reveal empty borders.")
                }

                Label {
                    width: parent.width
                    text: qsTr("Pick the newest frame this late")
                    font.pointSize: 12
                    wrapMode: Text.Wrap
                }

                AutoResizingComboBox {
                    id: vrLatchDelayComboBox
                    property var values: [0, 3, 5, 6]
                    model: [qsTr("Off"), "3 ms", "5 ms", "6 ms"]
                    Component.onCompleted: {
                        var i = values.indexOf(Math.round(StreamingPreferences.vrLatchDelayMs))
                        currentIndex = i >= 0 ? i : 0
                        recalculateWidth()
                    }
                    onActivated: {
                        StreamingPreferences.vrLatchDelayMs = values[currentIndex]
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 8000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Waits a few milliseconds before choosing which frame to show, so the frame is fresher. Too long makes the headset skip frames.")
                }

                Label {
                    width: parent.width
                    text: qsTr("Sharper centre (foveated encoding)")
                    font.pointSize: 12
                    wrapMode: Text.Wrap
                }

                AutoResizingComboBox {
                    id: vrFoveationComboBox
                    property var values: [100, 125, 150, 200]
                    model: [qsTr("Off"), "1.25x", "1.5x", "2x"]
                    Component.onCompleted: {
                        var i = values.indexOf(StreamingPreferences.vrFoveation)
                        currentIndex = i >= 0 ? i : 0
                        recalculateWidth()
                    }
                    onActivated: {
                        StreamingPreferences.vrFoveation = values[currentIndex]
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 8000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Spends more of the stream's pixels straight ahead and fewer at the edges of each eye. The stream stays the same size.")
                }

                CheckBox {
                    id: vrHalfRateCheck
                    width: parent.width
                    text: qsTr("Stream at half the headset refresh rate")
                    font.pointSize: 12
                    checked: StreamingPreferences.vrHalfRate
                    onCheckedChanged: {
                        StreamingPreferences.vrHalfRate = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 8000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("The game renders and streams at half rate while the headset still displays at full rate. Halves the load on the host and the network.")
                }

                CheckBox {
                    id: vrSynthCheck
                    width: parent.width
                    text: qsTr("Synthesize the in-between frames")
                    font.pointSize: 12
                    enabled: vrHalfRateCheck.checked
                    checked: StreamingPreferences.vrSynth
                    onCheckedChanged: {
                        StreamingPreferences.vrSynth = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 8000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("With half-rate streaming, moving objects are pushed forward on the refreshes that have no new frame instead of showing the previous frame again.")
                }
            }
        }
    }

    Column {
        padding: 10
        rightPadding: 20
        anchors.left: settingsColumn1.right
        id: settingsColumn2
        width: settingsPage.width / 2
        spacing: 15

        GroupBox {
            id: inputSettingsGroupBox
            width: (parent.width - (parent.leftPadding + parent.rightPadding))
            padding: 12
            title: "<font color=\"#D4FF3A\">§ 05 · </font><font color=\"#F2F5E1\">" + qsTr("Input Settings") + "</font>"
            font.pointSize: 12

            Column {
                anchors.fill: parent
                spacing: 5

                CheckBox {
                    id: absoluteMouseCheck
                    hoverEnabled: true
                    width: parent.width
                    text: qsTr("Optimize mouse for remote desktop instead of games")
                    font.pointSize:  12
                    checked: StreamingPreferences.absoluteMouseMode
                    onCheckedChanged: {
                        StreamingPreferences.absoluteMouseMode = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 10000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("This enables seamless mouse control without capturing the client's mouse cursor. It is ideal for remote desktop usage but will not work in most games.") + " " +
                                  qsTr("You can toggle this while streaming using Ctrl+Alt+Shift+M.") + "\n\n" +
                                  qsTr("NOTE: Due to a bug in GeForce Experience, this option may not work properly if your host PC has multiple monitors.")
                }

                Row {
                    spacing: 5
                    width: parent.width

                    CheckBox {
                        id: captureSysKeysCheck
                        hoverEnabled: true
                        text: qsTr("Capture system keyboard shortcuts")
                        font.pointSize: 12
                        enabled: SystemProperties.hasDesktopEnvironment
                        checked: StreamingPreferences.captureSysKeysMode !== StreamingPreferences.CSK_OFF || !SystemProperties.hasDesktopEnvironment

                        ToolTip.delay: 1000
                        ToolTip.timeout: 10000
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("This enables the capture of system-wide keyboard shortcuts like Alt+Tab that would normally be handled by the client OS while streaming.") + "\n\n" +
                                      qsTr("NOTE: Certain keyboard shortcuts like Ctrl+Alt+Del on Windows cannot be intercepted by any application, including VipleStream.")
                    }

                    AutoResizingComboBox {
                        // ignore setting the index at first, and actually set it when the component is loaded
                        Component.onCompleted: {
                            if (!visible) {
                                // Do nothing if the control won't even be visible
                                return
                            }

                            var saved_syskeysmode = StreamingPreferences.captureSysKeysMode
                            currentIndex = 0
                            for (var i = 0; i < captureSysKeysModeListModel.count; i++) {
                                var el_syskeysmode = captureSysKeysModeListModel.get(i).val;
                                if (saved_syskeysmode === el_syskeysmode) {
                                    currentIndex = i
                                    break
                                }
                            }

                            activated(currentIndex)
                        }

                        enabled: captureSysKeysCheck.checked && captureSysKeysCheck.enabled
                        textRole: "text"
                        model: ListModel {
                            id: captureSysKeysModeListModel
                            ListElement {
                                text: qsTr("in fullscreen")
                                val: StreamingPreferences.CSK_FULLSCREEN
                            }
                            ListElement {
                                text: qsTr("always")
                                val: StreamingPreferences.CSK_ALWAYS
                            }
                        }

                        function updatePref() {
                            if (!enabled) {
                                StreamingPreferences.captureSysKeysMode = StreamingPreferences.CSK_OFF
                            }
                            else {
                                StreamingPreferences.captureSysKeysMode = captureSysKeysModeListModel.get(currentIndex).val
                            }
                        }

                        // ::onActivated must be used, as it only listens for when the index is changed by a human
                        onActivated: {
                            updatePref()
                        }

                        // This handles transition of the checkbox state
                        onEnabledChanged: {
                            updatePref()
                        }
                    }
                }

                CheckBox {
                    id: absoluteTouchCheck
                    hoverEnabled: true
                    width: parent.width
                    text: qsTr("Use touchscreen as a virtual trackpad")
                    font.pointSize:  12
                    checked: !StreamingPreferences.absoluteTouchMode
                    onCheckedChanged: {
                        StreamingPreferences.absoluteTouchMode = !checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("When checked, the touchscreen acts like a trackpad. When unchecked, the touchscreen will directly control the mouse pointer.")
                }

                CheckBox {
                    id: swapMouseButtonsCheck
                    hoverEnabled: true
                    width: parent.width
                    text: qsTr("Swap left and right mouse buttons")
                    font.pointSize:  12
                    checked: StreamingPreferences.swapMouseButtons
                    onCheckedChanged: {
                        StreamingPreferences.swapMouseButtons = checked
                    }
                }

                CheckBox {
                    id: reverseScrollButtonsCheck
                    hoverEnabled: true
                    width: parent.width
                    text: qsTr("Reverse mouse scrolling direction")
                    font.pointSize: 12
                    checked: StreamingPreferences.reverseScrollDirection
                    onCheckedChanged: {
                        StreamingPreferences.reverseScrollDirection = checked
                    }
                }
            }
        }

        GroupBox {
            id: gamepadSettingsGroupBox
            width: (parent.width - (parent.leftPadding + parent.rightPadding))
            padding: 12
            title: "<font color=\"#D4FF3A\">§ 06 · </font><font color=\"#F2F5E1\">" + qsTr("Gamepad Settings") + "</font>"
            font.pointSize: 12

            Column {
                anchors.fill: parent
                spacing: 5

                CheckBox {
                    id: swapFaceButtonsCheck
                    width: parent.width
                    text: qsTr("Swap A/B and X/Y gamepad buttons")
                    font.pointSize: 12
                    checked: StreamingPreferences.swapFaceButtons
                    onCheckedChanged: {
                        StreamingPreferences.swapFaceButtons = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("This switches gamepads into a Nintendo-style button layout")
                }

                CheckBox {
                    id: singleControllerCheck
                    width: parent.width
                    text: qsTr("Force gamepad #1 always connected")
                    font.pointSize:  12
                    checked: !StreamingPreferences.multiController
                    onCheckedChanged: {
                        StreamingPreferences.multiController = !checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Forces a single gamepad to always stay connected to the host, even if no gamepads are actually connected to this PC.") + " " +
                                  qsTr("Only enable this option when streaming a game that doesn't support gamepads being connected after startup.")
                }

                CheckBox {
                    id: gamepadMouseCheck
                    hoverEnabled: true
                    width: parent.width
                    text: qsTr("Enable mouse control with gamepads by holding the 'Start' button")
                    font.pointSize: 12
                    checked: StreamingPreferences.gamepadMouse
                    onCheckedChanged: {
                        StreamingPreferences.gamepadMouse = checked
                    }
                }

                CheckBox {
                    id: backgroundGamepadCheck
                    width: parent.width
                    text: qsTr("Process gamepad input when VipleStream is in the background")
                    font.pointSize: 12
                    visible: SystemProperties.hasDesktopEnvironment
                    checked: StreamingPreferences.backgroundGamepad
                    onCheckedChanged: {
                        StreamingPreferences.backgroundGamepad = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Allows VipleStream to capture gamepad inputs even if it's not the current window in focus")
                }
            }
        }

        GroupBox {
            id: mpquicSettingsGroupBox
            width: (parent.width - (parent.leftPadding + parent.rightPadding))
            padding: 12
            title: "<font color=\"#D4FF3A\">§ 08 · </font><font color=\"#F2F5E1\">" + qsTr("Network (MP-QUIC)") + "</font>"
            font.pointSize: 12

            Column {
                anchors.fill: parent
                spacing: 5

                CheckBox {
                    id: enableMpQuicCheck
                    width: parent.width
                    text: qsTr("Enable MP-QUIC multipath transport")
                    font.pointSize: 12
                    checked: StreamingPreferences.enableMpQuic
                    onCheckedChanged: {
                        StreamingPreferences.enableMpQuic = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Uses QUIC multipath (RFC 9443) for streaming. Enables WiFi + cellular failover and dual-NIC bandwidth aggregation. Requires server support.")
                }

                Label {
                    width: parent.width
                    text: qsTr("Path scheduler")
                    font.pointSize: 12
                    wrapMode: Text.Wrap
                    visible: enableMpQuicCheck.checked
                }

                AutoResizingComboBox {
                    visible: enableMpQuicCheck.checked
                    Component.onCompleted: {
                        currentIndex = StreamingPreferences.mpQuicScheduler
                        // 跟 resolution / fps dropdown 一樣顯式量寬, 不然
                        // textWidth=0 → dropdown 只剩 padding+indicator,
                        // 文字被截掉看不到內容.
                        recalculateWidth()
                    }
                    model: [
                        qsTr("Auto (per stream type)"),
                        qsTr("Lowest latency"),
                        qsTr("Bandwidth aggregation"),
                        qsTr("Redundant (all paths)"),
                        qsTr("Earliest completion")
                    ]
                    onActivated: {
                        StreamingPreferences.mpQuicScheduler = currentIndex
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Auto: video=ECF, audio=redundant, control=min-RTT. Aggregation: round-robin across all NICs for maximum bandwidth.")
                }
            }
        }

        GroupBox {
            id: advancedSettingsGroupBox
            width: (parent.width - (parent.leftPadding + parent.rightPadding))
            padding: 12
            title: "<font color=\"#D4FF3A\">§ 07 · </font><font color=\"#F2F5E1\">" + qsTr("Advanced Settings") + "</font>"
            font.pointSize: 12

            Column {
                anchors.fill: parent
                spacing: 5

                Label {
                    width: parent.width
                    id: resVDSTitle
                    text: qsTr("Video decoder")
                    font.pointSize: 12
                    wrapMode: Text.Wrap
                }

                AutoResizingComboBox {
                    // ignore setting the index at first, and actually set it when the component is loaded
                    Component.onCompleted: {
                        var saved_vds = StreamingPreferences.videoDecoderSelection
                        currentIndex = 0
                        for (var i = 0; i < decoderListModel.count; i++) {
                            var el_vds = decoderListModel.get(i).val;
                            if (saved_vds === el_vds) {
                                currentIndex = i
                                break
                            }
                        }
                        activated(currentIndex)
                    }

                    id: decoderComboBox
                    textRole: "text"
                    model: ListModel {
                        id: decoderListModel
                        ListElement {
                            text: qsTr("Automatic (Recommended)")
                            val: StreamingPreferences.VDS_AUTO
                        }
                        ListElement {
                            text: qsTr("Force software decoding")
                            val: StreamingPreferences.VDS_FORCE_SOFTWARE
                        }
                        ListElement {
                            text: qsTr("Force hardware decoding")
                            val: StreamingPreferences.VDS_FORCE_HARDWARE
                        }
                    }
                    // ::onActivated must be used, as it only listens for when the index is changed by a human
                    onActivated: {
                        if (enabled) {
                            StreamingPreferences.videoDecoderSelection = decoderListModel.get(currentIndex).val
                        }
                    }
                }

                Label {
                    width: parent.width
                    id: resVCCTitle
                    text: qsTr("Video codec")
                    font.pointSize: 12
                    wrapMode: Text.Wrap
                }

                AutoResizingComboBox {
                    // ignore setting the index at first, and actually set it when the component is loaded
                    Component.onCompleted: {
                        var saved_vcc = StreamingPreferences.videoCodecConfig

                        // Default to Automatic (relevant if HDR is enabled,
                        // where we will match none of the codecs in the list)
                        currentIndex = 0

                        for(var i = 0; i < codecListModel.count; i++) {
                            var el_vcc = codecListModel.get(i).val;
                            if (saved_vcc === el_vcc) {
                                currentIndex = i
                                break
                            }
                        }

                        activated(currentIndex)
                    }

                    id: codecComboBox
                    textRole: "text"
                    model: ListModel {
                        id: codecListModel
                        ListElement {
                            text: qsTr("Automatic (Recommended)")
                            val: StreamingPreferences.VCC_AUTO
                        }
                        ListElement {
                            text: qsTr("H.264")
                            val: StreamingPreferences.VCC_FORCE_H264
                        }
                        ListElement {
                            text: qsTr("HEVC (H.265)")
                            val: StreamingPreferences.VCC_FORCE_HEVC
                        }
                        ListElement {
                            text: qsTr("AV1 (Experimental)")
                            val: StreamingPreferences.VCC_FORCE_AV1
                        }
                    }
                    // ::onActivated must be used, as it only listens for when the index is changed by a human
                    onActivated : {
                        if (enabled) {
                            StreamingPreferences.videoCodecConfig = codecListModel.get(currentIndex).val
                        }
                    }
                }

                CheckBox {
                    id: enableHdr
                    width: parent.width
                    text: qsTr("Enable HDR (Experimental)")
                    font.pointSize: 12

                    enabled: SystemProperties.supportsHdr
                    checked: enabled && StreamingPreferences.enableHdr
                    onCheckedChanged: {
                        StreamingPreferences.enableHdr = checked
                    }

                    // Updating StreamingPreferences.videoCodecConfig is handled above

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: enabled ?
                                      qsTr("The stream will be HDR-capable, but some games may require an HDR monitor on your host PC to enable HDR mode.")
                                    :
                                      qsTr("HDR streaming is not supported on this PC.")
                }

                CheckBox {
                    id: enableYUV444
                    width: parent.width
                    text: qsTr("Enable YUV 4:4:4 (Experimental)")
                    font.pointSize: 12

                    checked: StreamingPreferences.enableYUV444
                    onCheckedChanged: {
                        // This is called on init, so only reset to default bitrate when checked state changes.
                        if (StreamingPreferences.enableYUV444 != checked) {
                            StreamingPreferences.enableYUV444 = checked
                            if (StreamingPreferences.autoAdjustBitrate) {
                                StreamingPreferences.bitrateKbps = StreamingPreferences.getDefaultBitrate(StreamingPreferences.width,
                                                                                                          StreamingPreferences.height,
                                                                                                          StreamingPreferences.fps,
                                                                                                          StreamingPreferences.enableYUV444,
                                                                                                          StreamingPreferences.enableFrameInterpolation);
                                slider.value = StreamingPreferences.bitrateKbps
                            }
                        }
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: enabled ?
                                      qsTr("Good for streaming desktop and text-heavy games, but not recommended for fast-paced games.")
                                    :
                                      qsTr("YUV 4:4:4 is not supported on this PC.")
                }

                CheckBox {
                    id: unlockBitrate
                    width: parent.width
                    text: qsTr("Unlock bitrate limit (Experimental)")
                    font.pointSize: 12

                    checked: StreamingPreferences.unlockBitrate
                    onCheckedChanged: {
                        StreamingPreferences.unlockBitrate = checked
                        StreamingPreferences.bitrateKbps = Math.min(StreamingPreferences.bitrateKbps, slider.to)
                        slider.value = StreamingPreferences.bitrateKbps
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("This unlocks extremely high video bitrates for use with VipleStream-Server hosts. It should only be used when streaming over an Ethernet LAN connection.")
                }

                CheckBox {
                    id: enableMdns
                    width: parent.width
                    text: qsTr("Automatically find PCs on the local network (Recommended)")
                    font.pointSize: 12
                    checked: StreamingPreferences.enableMdns
                    onCheckedChanged: {
                        // This is called on init, so only do the work if we've
                        // actually changed the value.
                        if (StreamingPreferences.enableMdns != checked) {
                            StreamingPreferences.enableMdns = checked

                            // Restart polling so the mDNS change takes effect
                            if (window.pollingActive) {
                                ComputerManager.stopPollingAsync()
                                ComputerManager.startPolling()
                            }
                        }
                    }
                }

                CheckBox {
                    id: detectNetworkBlocking
                    width: parent.width
                    text: qsTr("Automatically detect blocked connections (Recommended)")
                    font.pointSize: 12
                    checked: StreamingPreferences.detectNetworkBlocking
                    onCheckedChanged: {
                        StreamingPreferences.detectNetworkBlocking = checked
                    }
                }

                CheckBox {
                    id: showPerformanceOverlay
                    width: parent.width
                    text: qsTr("Show performance stats while streaming")
                    font.pointSize: 12
                    checked: StreamingPreferences.showPerformanceOverlay
                    onCheckedChanged: {
                        StreamingPreferences.showPerformanceOverlay = checked
                    }

                    ToolTip.delay: 1000
                    ToolTip.timeout: 5000
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Display real-time stream performance information while streaming.") + "\n\n" +
                                  qsTr("You can toggle it at any time while streaming using Ctrl+Alt+Shift+S or Select+L1+R1+X.") + "\n\n" +
                                  qsTr("The performance overlay is not supported on Steam Link or Raspberry Pi.")
                }
            }
        }
    }
}
