import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.3
import QtCore

import ComputerManager 1.0
import MediaLibrary 1.0
import SdlGamepadKeyNavigation 1.0
import VrLauncher 1.0

// VipleStream 2.0 §VR-PLAYER：影片模式的選片頁。選好之後 VrLauncher.launchPlayer() 另開子行程在頭盔裡播
// （這一頁隱藏，播完再回來）。分頁：這台頭盔上的檔案、電腦（VipleStream Server 分享的資料夾）、
// YouTube、網址。YouTube 分頁在有 Qt WebEngine 的建置就是內建瀏覽器開的 youtube.com（跟網頁版一樣、可登入；
// VideoBrowser.qml），沒有的建置退回 yt-dlp 搜尋清單；網址解析都用 yt-dlp，在播放器裡做。
// 「最近播過」分頁 2026-11-02 依使用者要求拿掉（續播位置仍由 MediaLibrary 記著）。畫面格式預設「自動」（播放器會看
// 影片的中繼資料與檔名），也可以在這裡或 VR 裡的控制列改。
Item {
    id: videoView
    objectName: qsTr("Videos")

    // §VR-PLAYER-THUMB（2026-10-23 使用者要求）：格狀檢視（縮圖＋片名），下方的按鈕可切回清單，選擇記在設定裡。
    // 四個清單都是 GridView：清單模式時一格＝整列寬（看起來跟以前的 ListView 一樣），格狀模式一列 gridColumns 格。
    // 縮圖來自 image://vthumb/（backend/videothumbs.h）：本機檔頭盔自己抽、主機的檔案主機產生、YouTube 用搜尋結果的縮圖
    Settings {
        id: viewSettings
        category: "VideoView"
        property bool grid: true
        // 上次停在哪個分頁（下次打開回到那裡）
        property int tab: 0
        // §VIDEO-LASTPLAYED（2026-11-18）：最後播放的那一項（本機路徑／「主機 uuid|根|路徑」／網址）。播完回到清單時
        // 那一項有外框、而且清單會捲到它——清單項目多的時候，使用者分不出剛才播的是哪一個。存在設定裡，介面重建後還在
        property string lastPlayed: ""
    }
    // 分頁順序（TabBar 與 StackLayout 一致；--dev-video 的分頁號碼也照這個）
    readonly property int tabHeadset: 0
    readonly property int tabPc: 1
    readonly property int tabYouTube: 2
    readonly property int tabLink: 3
    // YouTube 分頁：B（＝Esc）／返回鍵是網頁的「上一頁」，不離開影片模式（2026-11-02 使用者要求：按 B 跑回主機清單跳太多層）。
    // 已經在第一頁、沒有上一頁時什麼都不做；要離開影片模式用左上角的返回或切到別的分頁再按 B
    function youTubeBack(event) {
        if (tabs.currentIndex === tabYouTube && browserLoader.item) {
            browserLoader.item.goBackIfPossible()
            event.accepted = true
        }
        else {
            event.accepted = false
        }
    }
    Keys.onEscapePressed: function(event) { youTubeBack(event) }
    Keys.onBackPressed: function(event) { youTubeBack(event) }
    readonly property bool gridMode: viewSettings.grid
    readonly property int gridColumns: 4
    // 同 VideoThumbProvider::encodeId()
    function thumbKey(s) { return encodeURIComponent(s).replace(/~/g, "%7E").replace(/%/g, "~") }
    function localThumb(path) { return "image://vthumb/local/" + thumbKey(path) }
    function sourceThumb(source) { return "image://vthumb/src/" + thumbKey(source) }
    function hostThumb(root, path, stamp) {
        // §REVIEW-1104：hostIndex 已改存主機 uuid，直接傳入 uuid 產生縮圖 id
        var id = MediaLibrary.hostThumbId(ComputerManager, videoView.hostIndex, root, path, stamp || "")
        return id !== "" ? "image://vthumb/" + id : ""
    }
    function cellW(view) { return gridMode ? Math.floor(view.width / gridColumns) : view.width }
    function cellH(view, rowHeight) { return gridMode ? Math.round((Math.floor(view.width / gridColumns) - 12) * 9 / 16) + 72 : rowHeight }

    // 格狀模式的一格：16:9 縮圖（拿不到時是色塊＋標籤）、片名（最多兩行）、副標
    // §VIDEO-LASTPLAYED：最後播放那一項的外框（蓋在清單項目上，不擋點擊）
    component PlayedFrame: Rectangle {
        anchors.fill: parent
        anchors.margins: 2
        z: 3
        radius: 6
        color: "#224da3ff"
        border.color: "#4da3ff"
        border.width: 3
    }
    function hostKey(root, path) { return hostIndex + "|" + root + "|" + path }
    // 清單載入後：游標移到最後播放的那一項並捲到看得見；沒有就停在第一項
    function focusPlayed(view, listModel, keyOf) {
        var at = 0
        for (var i = 0; viewSettings.lastPlayed !== "" && i < listModel.count; i++) {
            var e = listModel.get(i)
            if (!e.isDir && keyOf(e) === viewSettings.lastPlayed) {
                at = i
                break
            }
        }
        view.currentIndex = at
        if (at > 0) {
            view.positionViewAtIndex(at, GridView.Contain)
        }
    }

    component ThumbCard: ColumnLayout {
        property string thumb: ""
        property string title: ""
        property string subtitle: ""
        property bool folder: false
        property string badge: ""
        spacing: 4
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.round(width * 9 / 16)
            radius: 6
            clip: true
            color: folder ? "#c9a227" : "#2a3446"
            Image {
                id: thumbImage
                anchors.fill: parent
                source: parent.parent.thumb
                asynchronous: true
                fillMode: Image.PreserveAspectCrop
                sourceSize.width: 320
                visible: status === Image.Ready
            }
            Label {
                anchors.centerIn: parent
                visible: thumbImage.status !== Image.Ready && parent.parent.badge !== ""
                text: parent.parent.badge
                font.pixelSize: 18
                color: "white"
                opacity: 0.8
            }
        }
        Label {
            text: parent.title
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            maximumLineCount: 2
            elide: Text.ElideRight
            font.pointSize: 11
            font.bold: parent.folder
        }
        Label {
            text: parent.subtitle
            visible: text !== ""
            Layout.fillWidth: true
            elide: Text.ElideRight
            font.pointSize: 9
            opacity: 0.65
        }
        Item { Layout.fillHeight: true }
    }

    property string currentDir: ""
    // 電腦分頁：先選主機（hostIndex＝-1），再像檔案總管一樣瀏覽：hostRoot＝-1 是那台主機的分享資料夾清單，
    // 否則是某個分享資料夾裡的 hostPath（相對路徑，用 / 分隔）。讀取是非同步的（hostRequest 對得上才採用結果）
    // §REVIEW-1104：改存主機 uuid（空字串代表未選主機），避免主機排序變動後指到別台電腦
    property string hostIndex: ""
    property string hostName: ""
    property int hostRoot: -1
    property string hostRootName: ""
    property string hostPath: ""
    property bool hostLoading: false
    // 瀏覽器第一次切到那一頁才載入，之後留著（不重新整理網頁）
    property bool browserWanted: false
    property int hostRequest: 0
    // §MEDIA-FILEOPS：剪貼簿。null，或 {side, host, root, path, name, isDir, mode:"copy"|"move"}。
    // 只在同一邊貼上（本機對本機；同一台 host 對同一台）。移動成功才清掉。
    property var fileClip: null
    property var fileOpTarget: null
    property string opKind: ""
    property string opSide: ""
    property real opDone: 0
    property real opTotal: 0
    // 「⋯」跟項目本體疊在同一格。按鈕先吃掉點擊，這面旗標擋住隨後冒上來的 onClicked。
    property bool suppressItemClick: false
    // 目前這台 host、這個分享根可不可寫（根清單那一層沒有「目前的根」）
    property bool hostRootWritable: false
    readonly property bool localCanWrite: currentDir !== "" && MediaLibrary.localWritable(currentDir)

    function canPaste(side) {
        var c = fileClip
        if (!c || c.side !== side || MediaLibrary.opBusy) {
            return false
        }
        if (side === "host") {
            return c.host === hostIndex && hostRoot >= 0 && hostRootWritable
        }
        return localCanWrite
    }

    // 名稱裡的 % 不要被 String.arg 再吃掉，否則確認框會少掉檔名的一段。
    function clipHint() {
        var c = fileClip
        if (!c) {
            return ""
        }
        var shown = (c.name || "").replace(/%/g, "%%")
        if (c.mode === "move") {
            return qsTr("Selected to move: %1").arg(shown)
        }
        return qsTr("Copied: %1").arg(shown)
    }

    function setFileClip(mode, target) {
        fileClip = {
            "side": target.side,
            "host": target.host,
            "root": target.root,
            "path": target.path,
            "name": target.name,
            "isDir": target.isDir,
            "mode": mode
        }
    }

    function openFileMenu(button, target) {
        fileOpTarget = target
        fileMenu.initiator = target.side === "local" ? fileList : hostEntryList
        if (fileMenu.popup) {
            fileMenu.popup(button, 0, button.height)
        }
        else {
            fileMenu.open()
        }
    }

    // 忙的時候不要把進行中那次的進度清成 0。C++ 也會擋，這裡先擋掉介面閃一下。
    function startOp(kind, side, go) {
        if (MediaLibrary.opBusy) {
            errorLabel.text = opErrorText("busy")
            return
        }
        opKind = kind
        opSide = side
        opDone = 0
        opTotal = 0
        errorLabel.text = ""
        go()
    }

    function commitMkdir(forHost, folderName) {
        startOp("mkdir", forHost ? "host" : "local", function() {
            if (forHost) {
                MediaLibrary.hostOp(ComputerManager, hostIndex, "mkdir", hostRoot, hostPath, 0, "", folderName)
            }
            else {
                MediaLibrary.localOp("mkdir", currentDir, "", folderName)
            }
        })
    }

    function commitDelete(target) {
        if (!target) {
            return
        }
        startOp("delete", target.side, function() {
            if (target.side === "local") {
                MediaLibrary.localOp("delete", target.path, "", "")
            }
            else {
                MediaLibrary.hostOp(ComputerManager, target.host, "delete", target.root, target.path, 0, "", "")
            }
        })
    }

    function commitPaste(side) {
        var c = fileClip
        if (!c || !canPaste(side)) {
            return
        }
        startOp(c.mode, side, function() {
            if (side === "local") {
                MediaLibrary.localOp(c.mode, c.path, currentDir, "")
            }
            else {
                MediaLibrary.hostOp(ComputerManager, hostIndex, c.mode, c.root, c.path, hostRoot, hostPath, "")
            }
        })
    }

    function refreshAfterOp() {
        if (opSide === "local") {
            openDir(currentDir)
        }
        else if (opSide === "host" && hostIndex !== "") {
            openHostDir(hostRoot, hostPath, "")
        }
    }

    function handleOpFinished(error) {
        if (error === "busy") {
            errorLabel.text = opErrorText(error)
            return
        }
        var kind = opKind
        if (error === "") {
            if (kind === "move") {
                fileClip = null
            }
            errorLabel.text = ""
        }
        else {
            errorLabel.text = opErrorText(error)
        }
        // 成功、取消、刪到一半的刪除／移動，清單要重讀。複製失敗若清不掉目的，也要重讀才看得到留下的檔。
        // 純失敗的複製（目的已刪掉）不重讀，避免清單閃一下。busy 在上面就返回，不重讀。
        if (error === "" || error === "cancelled" || kind === "delete" || kind === "move"
                || error === "failed: cleanup failed") {
            refreshAfterOp()
        }
        opKind = ""
        opSide = ""
    }

    function opErrorText(error) {
        if (error === "read-only") return qsTr("This folder is read-only.")
        if (error === "not found") return qsTr("That item was not found.")
        if (error === "bad name") return qsTr("That name cannot be used.")
        if (error === "bad request") return qsTr("That file operation is not valid.")
        if (error === "exists") return qsTr("An item with that name already exists.")
        if (error === "into itself") return qsTr("A folder cannot be copied or moved into itself.")
        if (error === "busy") return qsTr("A file operation is already in progress.")
        if (error === "no space") return qsTr("Not enough free space.")
        if (error === "cancelled") return qsTr("Cancelled.")
        if (error.indexOf("failed:") === 0) {
            var reason = error.substring(7).trim()
            return reason.length > 0 ? qsTr("The file operation failed.") + " " + reason
                                     : qsTr("The file operation failed.")
        }
        return qsTr("The file operation failed.") + " " + error
    }
    // 畫面格式選單：顯示文字與播放器的格式名稱（playerformat.h）
    readonly property var formatNames: ["auto", "flat", "flat-sbs", "flat-tb", "180-sbs", "180-mono", "360-mono", "360-tb", "360-sbs", "360eac-mono"]

    function play(source, title) {
        var fmt = formatNames[formatBox.currentIndex]
        if (!VrLauncher.launchPlayer(source, fmt, title || "", "")) {
            errorLabel.text = qsTr("Could not start the video player.")
        }
        else {
            errorLabel.text = ""
        }
    }

    function openDir(path) {
        currentDir = path
        dirModel.clear()
        var entries = path === "" ? MediaLibrary.roots() : MediaLibrary.listDir(path)
        for (var i = 0; i < entries.length; i++) {
            var e = entries[i]
            dirModel.append({ "name": e.name, "path": e.path,
                              "isDir": path === "" ? true : e.isDir,
                              "detail": path === "" ? e.path : e.detail,
                              "format": path === "" ? "" : e.format })
        }
        focusPlayed(fileList, dirModel, function(e) { return e.path })
    }

    function refreshHosts() {
        var hs = MediaLibrary.hosts(ComputerManager)
        // 同一批主機只有上下線變了：就地更新（不重建清單，游標與捲動位置不跳）
        var same = hs.length === hostsModel.count
        for (var j = 0; same && j < hs.length; j++) {
            // §REVIEW-1104：hostIdx 改存主機 uuid
            same = hostsModel.get(j).hostIdx === hs[j].uuid && hostsModel.get(j).name === hs[j].name
        }
        if (same) {
            for (var k = 0; k < hs.length; k++) {
                hostsModel.setProperty(k, "online", hs[k].online)
            }
            return
        }
        var cur = hostListView.currentIndex
        hostsModel.clear()
        for (var i = 0; i < hs.length; i++) {
            // §REVIEW-1104：hostIdx 改存主機 uuid
            hostsModel.append({ "name": hs[i].name, "online": hs[i].online, "hostIdx": hs[i].uuid })
        }
        hostListView.currentIndex = Math.min(Math.max(cur, 0), hostsModel.count - 1)
        devOpenHost()
    }

    // dev（--dev-video host=<名>,dir=<子資料夾>）：主機上線後自動點進去
    property int devHostStep: VrLauncher.devVideoHost !== "" ? 0 : -1
    function devOpenHost() {
        if (devHostStep !== 0) {
            return
        }
        for (var i = 0; i < hostsModel.count; i++) {
            var h = hostsModel.get(i)
            if (h.name === VrLauncher.devVideoHost && h.online) {
                devHostStep = VrLauncher.devVideoDir !== "" ? 1 : 2
                console.warn("[VIPLE-PLAYER] dev: opening PC " + h.name)
                openHost(h.hostIdx, h.name)
                return
            }
        }
    }

    // 主機上線、下線、配對狀態變了（啟動後頭幾秒才陸續查到）：主機清單跟著更新
    Connections {
        target: ComputerManager
        function onComputerStateChanged() {
            videoView.refreshHosts()
        }
    }

    // 選了一台主機：列它的分享資料夾
    // §REVIEW-1104：openHost 改收並保存主機 uuid
    function openHost(uuid, name) {
        hostIndex = uuid
        hostName = name
        openHostDir(-1, "", "")
    }

    // 回到主機清單
    function closeHost() {
        // §REVIEW-1104：hostIndex 改為空字串代表回到主機清單
        hostIndex = ""
        hostModel.clear()
        hostMessage.text = ""
        // §REVIEW-1104：讓在途的 hostBrowseFinished 對不上 requestId，避免重填已關掉的清單並搶焦點
        hostRequest++
        refreshHosts()
        hostListView.forceActiveFocus(Qt.TabFocus)
    }

    // root＝-1：分享資料夾清單；rootName 只給路徑列顯示
    function openHostDir(root, path, rootName) {
        // §MEDIA-FILEOPS：回到根清單就沒有「目前這個根」。重新整理或走進子資料夾時不要把可寫旗標清掉，
        // 否則「⋯」會在同一層裡忽然消失。旗標是進來的時候從 <Writable> 抄的。
        if (root < 0) {
            hostRootWritable = false
        }
        hostRoot = root
        hostPath = path
        if (rootName !== undefined && rootName !== "") {
            hostRootName = rootName
        }
        hostModel.clear()
        hostMessage.text = ""
        hostLoading = true
        hostRequest++
        // §REVIEW-1104：改傳主機 uuid 瀏覽分享資料夾
        MediaLibrary.hostBrowse(ComputerManager, hostIndex, root, path, hostRequest)
    }

    // 上一層：資料夾裡往上一層；在分享資料夾的最上層就回到分享資料夾清單；在清單就回到主機清單
    function hostUp() {
        if (hostRoot < 0) {
            closeHost()
        }
        else if (hostPath === "") {
            openHostDir(-1, "", "")
        }
        else {
            var slash = hostPath.lastIndexOf("/")
            openHostDir(hostRoot, slash < 0 ? "" : hostPath.substring(0, slash), "")
        }
    }

    // 路徑列的第 i 段（0＝分享資料夾本身）
    function hostCrumbs() {
        if (hostRoot < 0) {
            return []
        }
        var parts = [hostRootName]
        if (hostPath !== "") {
            parts = parts.concat(hostPath.split("/"))
        }
        return parts
    }

    function openCrumb(i) {
        var parts = hostPath === "" ? [] : hostPath.split("/")
        openHostDir(hostRoot, parts.slice(0, i).join("/"), "")
    }

    StackView.onActivated: {
        if (VrLauncher.devVideoTab >= 0) {
            console.warn("[VIPLE-PLAYER] dev video page activated: tab=" + VrLauncher.devVideoTab)
        }
        openDir("")
        refreshHosts()
        var t = VrLauncher.devVideoTab >= 0 ? VrLauncher.devVideoTab : viewSettings.tab
        tabs.currentIndex = t >= 0 && t < tabs.count ? t : tabHeadset
        tabs.forceActiveFocus(Qt.TabFocus)
        SdlGamepadKeyNavigation.setScrollMode(true)
    }

    StackView.onDeactivating: {
        SdlGamepadKeyNavigation.setScrollMode(false)
    }

    // 手把的搖桿上下／LB／RB（SdlGamepadKeyNavigation 的捲動模式）：捲目前這個分頁看得到的清單（瀏覽器分頁＝捲網頁）。
    // 使用者 2026-10-19 的配戴回饋：清單一頁放不下時要拖右邊的捲軸很麻煩
    function scrollList(lv, pages) {
        if (!lv || !lv.visible || lv.contentHeight <= lv.height) {
            return
        }
        var top = lv.originY
        var bottom = lv.originY + lv.contentHeight - lv.height
        lv.contentY = Math.max(top, Math.min(bottom, lv.contentY + pages * lv.height))
    }

    // 捲目前這個分頁（手把的搖桿／LB／RB，或右側的上一頁／下一頁按鈕）
    function scrollCurrent(pages) {
        switch (tabs.currentIndex) {
        case tabHeadset: videoView.scrollList(fileList, pages); break
        // §REVIEW-1104：hostIndex 改存 uuid 字串，以是否為空字串判斷是否已選主機
        case tabPc: videoView.scrollList(videoView.hostIndex === "" ? hostListView : hostEntryList, pages); break
        case tabYouTube:
            if (browserLoader.item) {
                browserLoader.item.scrollPages(pages)
            }
            else {
                videoView.scrollList(webList, pages)
            }
            break
        }
    }

    Connections {
        target: SdlGamepadKeyNavigation
        function onScrollRequested(pages) {
            if (videoView.StackView.status === StackView.Active) {
                videoView.scrollCurrent(pages)
            }
        }
    }

    // §VR-PLAYER（dev，--dev-video play=<秒>,url=<檔案或網址>）：瀏覽器以外的分頁，幾秒後直接播 url（無人驗測啟動流程用）
    Timer {
        running: VrLauncher.devVideoPlaySec > 0 && VrLauncher.devVideoUrl !== ""
                 && !(VrLauncher.devVideoTab === tabYouTube && VrLauncher.browserAvailable)
        interval: VrLauncher.devVideoPlaySec * 1000
        onTriggered: {
            console.warn("[VIPLE-PLAYER] dev: playing " + VrLauncher.devVideoUrl)
            videoView.play(VrLauncher.devVideoUrl, "")
        }
    }

    // §VR-PLAYER（dev，--dev-video）：幾秒後把這一頁存成 PNG（無人驗測看版面與瀏覽器有沒有畫出來）
    Timer {
        running: VrLauncher.devVideoGrabSec > 0
        interval: VrLauncher.devVideoGrabSec * 1000
        onTriggered: {
            var started = videoView.grabToImage(function(r) {
                var ok = r.saveToFile(VrLauncher.devVideoGrabPath)
                console.warn("[VIPLE-PLAYER] dev grab " + VrLauncher.devVideoGrabPath + " " + ok)
            })
            console.warn("[VIPLE-PLAYER] dev grab requested: " + started + " visible=" + videoView.visible
                         + " size=" + videoView.width + "x" + videoView.height)
        }
    }

    ListModel { id: dirModel }
    ListModel { id: hostsModel }  // name, online, hostIdx
    ListModel { id: hostModel }
    ListModel { id: webModel }

    Connections {
        target: MediaLibrary
        function onWebSearchFinished(results, error) {
            webModel.clear()
            for (var i = 0; i < results.length; i++) {
                webModel.append(results[i])
            }
            webStatus.text = error !== "" ? error : (results.length === 0 ? qsTr("Nothing found.") : "")
            webSearchButton.enabled = true
        }
        function onHostBrowseFinished(requestId, entries, error) {
            if (requestId !== videoView.hostRequest) {
                return  // 已經換到別的資料夾了
            }
            videoView.hostLoading = false
            hostModel.clear()
            var root = videoView.hostRoot
            for (var i = 0; i < entries.length; i++) {
                var e = entries[i]
                hostModel.append({ "name": e.name, "path": root < 0 ? "" : e.path, "root": root < 0 ? e.index : root,
                                   "isDir": root < 0 ? true : e.isDir,
                                   "detail": root < 0 ? (e.drive ? qsTr("Drive") : qsTr("Shared folder")) : e.detail,
                                   "format": root < 0 ? "" : e.format, "drive": root < 0 && e.drive === true,
                                   "stamp": root < 0 ? "" : (e.stamp || ""),
                                   // 只有根清單帶 Writable。沒有這個欄位（舊 server）當成不可寫。
                                   "writable": root < 0 ? e.writable === true : false })
            }
            hostMessage.text = error !== "" ? error : (entries.length === 0 ? qsTr("This folder has no videos.") : "")
            videoView.focusPlayed(hostEntryList, hostModel, function(e) { return videoView.hostKey(e.root, e.path) })
            hostEntryList.forceActiveFocus(Qt.TabFocus)
            if (videoView.devHostStep === 1 && root < 0 && entries.length > 0) {
                videoView.devHostStep = 2
                // 自動走進第一個根之前先抄可寫，openHostDir 只在回到根清單時才清掉。
                videoView.hostRootWritable = entries[0].writable === true
                videoView.openHostDir(entries[0].index, VrLauncher.devVideoDir === "." ? "" : VrLauncher.devVideoDir, entries[0].name)
            }
        }
        function onWebUpdateFinished(ok, message) {
            webStatus.text = message
            webUpdateButton.enabled = true
            webVersionLabel.text = qsTr("Helper: %1").arg(MediaLibrary.webVersion() || qsTr("not installed"))
        }
        // §MEDIA-FILEOPS：進度與結束都在 GUI 執行緒（C++ 已排回來）。
        function onOpProgress(done, total) {
            videoView.opDone = done
            videoView.opTotal = total
        }
        function onOpFinished(error) {
            videoView.handleOpFinished(error)
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        TabBar {
            id: tabs
            Layout.fillWidth: true
            TabButton { text: qsTr("This headset") }
            TabButton { text: qsTr("PC") }
            TabButton { text: "YouTube" }
            TabButton { text: qsTr("Link") }
            onCurrentIndexChanged: {
                if (currentIndex === videoView.tabYouTube) {
                    videoView.browserWanted = true
                }
                if (VrLauncher.devVideoTab < 0) {
                    viewSettings.tab = currentIndex
                }
            }
            Keys.onDownPressed: {
                if (tabs.currentIndex === videoView.tabYouTube && browserLoader.item) {
                    browserLoader.item.focusPage()
                    return
                }
                // §REVIEW-1104：hostIndex 改存 uuid 字串，以是否為空字串判斷是否已選主機
                var t = [fileList, videoView.hostIndex === "" ? hostListView : hostEntryList, webQuery, urlField][tabs.currentIndex]
                t.forceActiveFocus(Qt.TabFocus)
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 8

            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: tabs.currentIndex

                // ── 這台頭盔上的檔案 ──
                ColumnLayout {
                    spacing: 6
                    RowLayout {
                        Layout.fillWidth: true
                        Button {
                            text: qsTr("Up")
                            enabled: videoView.currentDir !== ""
                            onClicked: {
                                var up = MediaLibrary.parentDir(videoView.currentDir)
                                // 回到起點清單（不往檔案系統的根目錄走）
                                var roots = MediaLibrary.roots()
                                var isRoot = false
                                for (var i = 0; i < roots.length; i++) {
                                    if (roots[i].path === videoView.currentDir) {
                                        isRoot = true
                                    }
                                }
                                openDir(isRoot || up === "" ? "" : up)
                            }
                        }
                        Label {
                            text: videoView.currentDir === "" ? qsTr("Choose a folder") : videoView.currentDir
                            elide: Text.ElideLeft
                            Layout.fillWidth: true
                            opacity: 0.8
                        }
                        // §MEDIA-FILEOPS：只在目前資料夾可寫時出現。起點清單（currentDir 空）沒有這些按鈕。
                        Button {
                            text: qsTr("New folder")
                            visible: videoView.localCanWrite
                            enabled: visible && !MediaLibrary.opBusy
                            onClicked: {
                                newFolderDialog.forHost = false
                                folderNameField.text = ""
                                newFolderDialog.open()
                            }
                        }
                        Button {
                            text: qsTr("Paste")
                            visible: videoView.localCanWrite
                            enabled: visible && videoView.canPaste("local")
                            onClicked: videoView.commitPaste("local")
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        visible: videoView.fileClip && videoView.fileClip.side === "local"
                        Label {
                            text: videoView.clipHint()
                            elide: Text.ElideMiddle
                            Layout.fillWidth: true
                            opacity: 0.8
                        }
                        Button {
                            text: qsTr("Clear")
                            enabled: !MediaLibrary.opBusy
                            onClicked: videoView.fileClip = null
                        }
                    }
                    GridView {
                        id: fileList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: dirModel
                        keyNavigationEnabled: true
                        cellWidth: videoView.cellW(fileList)
                        // 清單列加高，右側 48px 的「⋯」才放得下，片名仍走省略。
                        cellHeight: videoView.cellH(fileList, 72)
                        ScrollBar.vertical: ScrollBar {}
                        delegate: ItemDelegate {
                            width: fileList.cellWidth
                            height: fileList.cellHeight
                            padding: videoView.gridMode ? 6 : 12
                            highlighted: fileList.activeFocus && fileList.currentIndex === index
                            contentItem: Item {
                                RowLayout {
                                    anchors.fill: parent
                                    // 清單模式把片名讓出「⋯」的寬度；格狀模式片名維持整格寬，按鈕蓋在縮圖角上。
                                    anchors.rightMargin: !videoView.gridMode && videoView.localCanWrite ? 44 : 0
                                    visible: !videoView.gridMode
                                    spacing: 12
                                    Label {
                                        text: model.name + (model.isDir ? "/" : "")
                                        textFormat: Text.PlainText  // §REVIEW-1127 qt_ui #2：檔名來自 host
                                        font.pointSize: 14
                                        font.bold: model.isDir
                                        elide: Text.ElideMiddle
                                        Layout.fillWidth: true
                                    }
                                    Label { text: model.format; opacity: 0.7; visible: model.format !== "" }
                                    Label { text: model.detail; opacity: 0.6; elide: Text.ElideLeft; Layout.maximumWidth: 360 }
                                }
                                ThumbCard {
                                    anchors.fill: parent
                                    visible: videoView.gridMode
                                    thumb: visible && !model.isDir ? videoView.localThumb(model.path) : ""
                                    title: model.name
                                    subtitle: model.isDir ? "" : (model.format !== "" ? model.format + " · " : "") + model.detail
                                    folder: model.isDir
                                    badge: model.isDir ? "" : qsTr("VID")
                                }
                            }
                            PlayedFrame { visible: !model.isDir && model.path === viewSettings.lastPlayed }
                            // 不進手把焦點鏈（跟右側捲動鈕一樣）。按下時先擋住項目本體的 onClicked，避免點「⋯」就開播。
                            Button {
                                id: localMoreButton
                                z: 6
                                width: 48
                                height: 48
                                padding: 0
                                focusPolicy: Qt.NoFocus
                                visible: videoView.localCanWrite
                                enabled: visible && !MediaLibrary.opBusy
                                text: "⋯"
                                font.pixelSize: 20
                                anchors.right: parent.right
                                anchors.top: parent.top
                                anchors.rightMargin: 4
                                anchors.topMargin: videoView.gridMode ? 4 : Math.max(0, (parent.height - height) / 2)
                                onPressed: videoView.suppressItemClick = true
                                onReleased: suppressClickTimer.restart()
                                onCanceled: videoView.suppressItemClick = false
                                onClicked: videoView.openFileMenu(localMoreButton, {
                                               "side": "local", "host": "", "root": -1,
                                               "path": model.path, "name": model.name, "isDir": model.isDir
                                           })
                            }
                            onClicked: {
                                if (videoView.suppressItemClick) {
                                    return
                                }
                                if (model.isDir) {
                                    openDir(model.path)
                                }
                                else {
                                    fileList.currentIndex = index
                                    viewSettings.lastPlayed = model.path
                                    play(model.path, "")
                                }
                            }
                            Keys.onReturnPressed: clicked()
                            Keys.onEnterPressed: clicked()
                        }
                        Label {
                            anchors.centerIn: parent
                            visible: dirModel.count === 0
                            text: qsTr("No videos here.")
                            opacity: 0.7
                        }
                    }
                }

                // ── 電腦（VipleStream Server 分享的資料夾）──
                Item {
                    // 第一層：可以連的主機（配對過的）
                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 8
                        // §REVIEW-1104：hostIndex 改存 uuid 字串，空字串時顯示主機清單
                        visible: videoView.hostIndex === ""
                        Label {
                            text: qsTr("Choose a PC to browse its shared video folders")
                            opacity: 0.8
                        }
                        ListView {
                            id: hostListView
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            model: hostsModel
                            keyNavigationEnabled: true
                            spacing: 6
                            delegate: ItemDelegate {
                                width: hostListView.width
                                height: 72
                                highlighted: hostListView.activeFocus && hostListView.currentIndex === index
                                enabled: model.online
                                contentItem: RowLayout {
                                    spacing: 16
                                    Rectangle {
                                        width: 14; height: 14; radius: 7
                                        color: model.online ? "#4caf50" : "#777777"
                                    }
                                    // §REVIEW-1127 qt_ui #2：主機名稱來自網路，不當成 HTML
                                    Label { text: model.name; textFormat: Text.PlainText; font.pointSize: 16; font.bold: true; Layout.fillWidth: true; elide: Text.ElideRight }
                                    Label { text: model.online ? qsTr("Online") : qsTr("Offline"); opacity: 0.7 }
                                }
                                onClicked: openHost(model.hostIdx, model.name)
                                Keys.onReturnPressed: clicked()
                                Keys.onEnterPressed: clicked()
                            }
                            Label {
                                anchors.centerIn: parent
                                width: parent.width * 0.8
                                horizontalAlignment: Text.AlignHCenter
                                wrapMode: Text.Wrap
                                visible: hostsModel.count === 0
                                text: qsTr("No paired PCs. Pair a PC in Desktop or VR mode first.")
                                opacity: 0.7
                            }
                        }
                    }

                    // 第二層：像檔案總管
                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 6
                        // §REVIEW-1104：hostIndex 改存 uuid 字串，非空時顯示已選主機的分享資料夾
                        visible: videoView.hostIndex !== ""
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 6
                            Button {
                                text: qsTr("All PCs")
                                onClicked: closeHost()
                            }
                            Button {
                                text: qsTr("Up one level")
                                enabled: !videoView.hostLoading
                                onClicked: hostUp()
                            }
                            Button {
                                text: qsTr("Refresh")
                                enabled: !videoView.hostLoading
                                onClicked: openHostDir(videoView.hostRoot, videoView.hostPath, "")
                            }
                            // §MEDIA-FILEOPS：分享根清單那一層沒有「目前的根」，按鈕先不出現。磁碟機的 Writable 是 0。
                            Button {
                                text: qsTr("New folder")
                                visible: videoView.hostRoot >= 0 && videoView.hostRootWritable
                                enabled: visible && !MediaLibrary.opBusy && !videoView.hostLoading
                                onClicked: {
                                    newFolderDialog.forHost = true
                                    folderNameField.text = ""
                                    newFolderDialog.open()
                                }
                            }
                            Button {
                                text: qsTr("Paste")
                                visible: videoView.hostRoot >= 0 && videoView.hostRootWritable
                                enabled: visible && videoView.canPaste("host") && !videoView.hostLoading
                                onClicked: videoView.commitPaste("host")
                            }
                            BusyIndicator {
                                running: videoView.hostLoading
                                visible: running
                                Layout.preferredHeight: 36
                                Layout.preferredWidth: 36
                            }
                        }
                        // 路徑列：主機 › 分享資料夾 › 子資料夾…（每一段都可以點）
                        Flow {
                            Layout.fillWidth: true
                            spacing: 2
                            Button {
                                flat: true
                                text: videoView.hostName
                                font.bold: videoView.hostRoot < 0
                                onClicked: openHostDir(-1, "", "")
                            }
                            Repeater {
                                model: videoView.hostCrumbs()
                                delegate: Row {
                                    Label { text: " › "; anchors.verticalCenter: parent.verticalCenter; opacity: 0.6 }
                                    Button {
                                        flat: true
                                        text: modelData
                                        font.bold: index === videoView.hostCrumbs().length - 1
                                        onClicked: openCrumb(index)
                                    }
                                }
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            visible: videoView.fileClip && videoView.fileClip.side === "host"
                            Label {
                                text: videoView.clipHint()
                                elide: Text.ElideMiddle
                                Layout.fillWidth: true
                                opacity: 0.8
                            }
                            Button {
                                text: qsTr("Clear")
                                enabled: !MediaLibrary.opBusy
                                onClicked: videoView.fileClip = null
                            }
                        }
                        GridView {
                            id: hostEntryList
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            model: hostModel
                            keyNavigationEnabled: true
                            cellWidth: videoView.cellW(hostEntryList)
                            cellHeight: videoView.cellH(hostEntryList, 72)
                            ScrollBar.vertical: ScrollBar {}
                            delegate: ItemDelegate {
                                width: hostEntryList.cellWidth
                                height: hostEntryList.cellHeight
                                padding: videoView.gridMode ? 6 : 12
                                highlighted: hostEntryList.activeFocus && hostEntryList.currentIndex === index
                                PlayedFrame { visible: !model.isDir && videoView.hostKey(model.root, model.path) === viewSettings.lastPlayed }
                                contentItem: Item {
                                ThumbCard {
                                    anchors.fill: parent
                                    visible: videoView.gridMode
                                    thumb: visible && !model.isDir && !model.drive ? videoView.hostThumb(model.root, model.path, model.stamp) : ""
                                    title: model.name
                                    subtitle: model.isDir ? "" : (model.format !== "" ? model.format + " · " : "") + model.detail
                                    folder: model.isDir && !model.drive
                                    badge: model.drive ? qsTr("DISK") : (model.isDir ? "" : qsTr("VID"))
                                }
                                RowLayout {
                                    anchors.fill: parent
                                    anchors.rightMargin: !videoView.gridMode && videoView.hostRoot >= 0 && videoView.hostRootWritable ? 44 : 0
                                    visible: !videoView.gridMode
                                    spacing: 12
                                    Rectangle {
                                        // 資料夾／影片的小圖示（字型不一定有 emoji，用色塊）
                                        width: 28; height: 22; radius: 4
                                        color: model.drive ? "#7a8a99" : (model.isDir ? "#c9a227" : "#3d7be0")
                                        Label {
                                            anchors.centerIn: parent
                                            text: model.drive ? qsTr("DISK") : (model.isDir ? "" : qsTr("VID"))
                                            font.pixelSize: 9
                                            color: "white"
                                        }
                                    }
                                    Label {
                                        text: model.name
                                        textFormat: Text.PlainText  // §REVIEW-1127 qt_ui #2：檔名來自 host
                                        font.pointSize: 14
                                        font.bold: model.isDir
                                        elide: Text.ElideMiddle
                                        Layout.fillWidth: true
                                    }
                                    Label { text: model.format; opacity: 0.7; visible: model.format !== "" }
                                    Label { text: model.detail; opacity: 0.6 }
                                }
                                }
                                // 根清單（分享資料夾本身、磁碟機）不放「⋯」。只在已進入、而且這個根可寫時給檔案與資料夾。
                                Button {
                                    id: hostMoreButton
                                    z: 6
                                    width: 48
                                    height: 48
                                    padding: 0
                                    focusPolicy: Qt.NoFocus
                                    visible: videoView.hostRoot >= 0 && videoView.hostRootWritable
                                    enabled: visible && !MediaLibrary.opBusy
                                    text: "⋯"
                                    font.pixelSize: 20
                                    anchors.right: parent.right
                                    anchors.top: parent.top
                                    anchors.rightMargin: 4
                                    anchors.topMargin: videoView.gridMode ? 4 : Math.max(0, (parent.height - height) / 2)
                                    onPressed: videoView.suppressItemClick = true
                                    onReleased: suppressClickTimer.restart()
                                    onCanceled: videoView.suppressItemClick = false
                                    onClicked: videoView.openFileMenu(hostMoreButton, {
                                                   "side": "host", "host": videoView.hostIndex, "root": model.root,
                                                   "path": model.path, "name": model.name, "isDir": model.isDir
                                               })
                                }
                                onClicked: {
                                    if (videoView.suppressItemClick) {
                                        return
                                    }
                                    if (model.isDir) {
                                        // 從根清單點進去才更新可寫。子資料夾沿用進來時抄下的旗標。
                                        if (videoView.hostRoot < 0) {
                                            videoView.hostRootWritable = model.writable === true
                                        }
                                        openHostDir(model.root, model.path, videoView.hostRoot < 0 ? model.name : "")
                                    }
                                    else {
                                        hostEntryList.currentIndex = index
                                        viewSettings.lastPlayed = videoView.hostKey(model.root, model.path)
                                        // §REVIEW-1104：改傳主機 uuid 組出播放網址
                                        play(MediaLibrary.hostFileUrl(ComputerManager, videoView.hostIndex, model.root, model.path), model.name)
                                    }
                                }
                                Keys.onReturnPressed: clicked()
                                Keys.onEnterPressed: clicked()
                            }
                            Keys.onBackPressed: hostUp()
                            Keys.onEscapePressed: hostUp()
                            Label {
                                id: hostMessage
                                anchors.centerIn: parent
                                width: parent.width * 0.8
                                horizontalAlignment: Text.AlignHCenter
                                wrapMode: Text.Wrap
                                visible: !videoView.hostLoading && text !== ""
                                opacity: 0.8
                            }
                        }
                    }
                }

                // ── YouTube：內建瀏覽器開 youtube.com（有 Qt WebEngine 的建置；VideoBrowser.qml），沒有的建置退回 yt-dlp 搜尋 ──
                Item {
                    Loader {
                        id: browserLoader
                        anchors.fill: parent
                        visible: VrLauncher.browserAvailable
                        // 第一次切到這一頁才載入（Chromium 很吃記憶體），之後留著（不重新整理網頁）
                        active: VrLauncher.browserAvailable && videoView.browserWanted
                        source: "qrc:/gui/VideoBrowser.qml"
                        onStatusChanged: console.warn("[VIPLE-PLAYER] browser loader status=" + status
                                                      + (status === Loader.Error ? " (load error)" : ""))
                        onLoaded: item.formatName = Qt.binding(function() { return videoView.formatNames[formatBox.currentIndex] })
                        Connections {
                            target: browserLoader.item
                            function onLaunchFailed(message) { errorLabel.text = message }
                        }
                    }
                    ColumnLayout {
                        anchors.fill: parent
                        visible: !VrLauncher.browserAvailable
                        spacing: 6
                        // 版本要跑一次 python（約 1 秒），第一次切到這一頁才查
                        onVisibleChanged: {
                            if (visible && webVersionLabel.text === "") {
                                webVersionLabel.text = qsTr("Helper: %1").arg(MediaLibrary.webVersion() || qsTr("not installed"))
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            TextField {
                                id: webQuery
                                Layout.fillWidth: true
                                placeholderText: qsTr("Search YouTube (try \"360 video\" or \"VR180\")")
                                Keys.onReturnPressed: webSearchButton.clicked()
                                Keys.onEnterPressed: webSearchButton.clicked()
                            }
                            Button {
                                id: webSearchButton
                                text: qsTr("Search")
                                enabled: webQuery.text.trim() !== ""
                                onClicked: {
                                    enabled = false
                                    webStatus.text = qsTr("Searching…")
                                    MediaLibrary.webSearch(webQuery.text)
                                }
                            }
                        }
                        GridView {
                            id: webList
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            model: webModel
                            keyNavigationEnabled: true
                            cellWidth: videoView.cellW(webList)
                            cellHeight: videoView.cellH(webList, 64)
                            ScrollBar.vertical: ScrollBar {}
                            delegate: ItemDelegate {
                                width: webList.cellWidth
                                height: webList.cellHeight
                                padding: videoView.gridMode ? 6 : 12
                                highlighted: webList.activeFocus && webList.currentIndex === index
                                contentItem: Item {
                                    ColumnLayout {
                                        anchors.fill: parent
                                        visible: !videoView.gridMode
                                        spacing: 2
                                        Label { text: model.title; font.pointSize: 14; elide: Text.ElideRight; Layout.fillWidth: true }
                                        Label { text: model.channel + (model.duration !== "" ? "   ·   " + model.duration : ""); opacity: 0.7 }
                                    }
                                    ThumbCard {
                                        anchors.fill: parent
                                        visible: videoView.gridMode
                                        thumb: visible ? videoView.sourceThumb(model.url) : ""
                                        title: model.title
                                        subtitle: model.channel + (model.duration !== "" ? " · " + model.duration : "")
                                        badge: qsTr("VID")
                                    }
                                }
                                PlayedFrame { visible: model.url === viewSettings.lastPlayed }
                                onClicked: {
                                    webList.currentIndex = index
                                    viewSettings.lastPlayed = model.url
                                    play(model.url, model.title)
                                }
                                Keys.onReturnPressed: clicked()
                                Keys.onEnterPressed: clicked()
                            }
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            Label {
                                id: webStatus
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                                opacity: 0.8
                            }
                            Label {
                                id: webVersionLabel
                                opacity: 0.6
                            }
                            Button {
                                id: webUpdateButton
                                text: qsTr("Update")
                                onClicked: {
                                    enabled = false
                                    webStatus.text = qsTr("Updating the YouTube helper…")
                                    MediaLibrary.webUpdate()
                                }
                            }
                        }
                    }
                }

                // ── 貼網址 ──
                ColumnLayout {
                    spacing: 12
                    Label {
                        text: qsTr("Paste a link: a video page (YouTube and many other sites) or a video file or stream (MP4, MKV, WebM, HLS .m3u8).")
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }
                    TextField {
                        id: urlField
                        Layout.fillWidth: true
                        placeholderText: "https://"
                        inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoAutoUppercase
                        Keys.onReturnPressed: urlPlay.clicked()
                        Keys.onEnterPressed: urlPlay.clicked()
                    }
                    Button {
                        id: urlPlay
                        text: qsTr("Play")
                        enabled: MediaLibrary.isUrl(urlField.text)
                        onClicked: play(urlField.text.trim(), "")
                    }
                    Item { Layout.fillHeight: true }
                }
            }

            // 上一頁／下一頁：頭盔上用雷射指標時搖桿不會送進來（Steam 自己留著，2026-10-19 量過：滾輪、按鍵、手把輸入都是 0），
            // 清單一頁放不下時用雷射點這兩顆大按鈕捲
            ColumnLayout {
                Layout.fillHeight: true
                Layout.preferredWidth: 72
                Layout.maximumWidth: 72
                spacing: 8
                Button {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    text: "▲"
                    Layout.maximumWidth: 72
                    font.pixelSize: 28
                    focusPolicy: Qt.NoFocus
                    onClicked: videoView.scrollCurrent(-0.8)
                }
                Button {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    text: "▼"
                    Layout.maximumWidth: 72
                    font.pixelSize: 28
                    focusPolicy: Qt.NoFocus
                    onClicked: videoView.scrollCurrent(0.8)
                }
            }
        }

        // §MEDIA-FILEOPS：進行中放在分頁外面，切到別頁也看得到、也能取消。total 還沒算好（0）用不確定模式。
        RowLayout {
            visible: MediaLibrary.opBusy
            Layout.fillWidth: true
            spacing: 8
            ProgressBar {
                Layout.fillWidth: true
                from: 0
                to: videoView.opTotal > 0 ? videoView.opTotal : 1
                value: videoView.opDone
                // 這一列藏起來時 ProgressBar 自己的 visible 仍是 true，不確定模式會在背後空轉。
                indeterminate: MediaLibrary.opBusy && videoView.opTotal <= 0
            }
            Button {
                text: qsTr("Cancel")
                focusPolicy: Qt.NoFocus
                onClicked: MediaLibrary.cancelOp()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 12
            Label { text: qsTr("Picture format") }
            ComboBox {
                id: formatBox
                Layout.preferredWidth: 320
                model: [qsTr("Auto"), qsTr("2D"), qsTr("3D side-by-side"), qsTr("3D top-bottom"), qsTr("180° 3D (side-by-side)"),
                        qsTr("180° 2D"), qsTr("360° 2D"), qsTr("360° 3D (top-bottom)"), qsTr("360° 3D (side-by-side)"),
                        qsTr("360° cubemap (YouTube EAC)")]
            }
            Label {
                id: errorLabel
                color: "#ff8080"
                Layout.fillWidth: true
                // 失敗原因接在通用句子後面，一行省略會把原因切掉。
                wrapMode: Text.Wrap
                maximumLineCount: 2
                elide: Text.ElideRight
            }
            Button {
                text: videoView.gridMode ? qsTr("List view") : qsTr("Grid view")
                focusPolicy: Qt.NoFocus
                onClicked: viewSettings.grid = !viewSettings.grid
            }
        }
    }

    // 「⋯」的 onPressed 先把旗標立起來。項目的 onClicked 若在同一次點擊裡跟著來，會被擋下。
    // 0 ms 的計時器排到這次事件處理完才清，避免旗標卡住下一次真正要開的項目。
    Timer {
        id: suppressClickTimer
        interval: 0
        onTriggered: videoView.suppressItemClick = false
    }

    NavigableMenu {
        id: fileMenu
        NavigableMenuItem {
            text: qsTr("Copy")
            enabled: visible && !MediaLibrary.opBusy
            onTriggered: {
                if (videoView.fileOpTarget) {
                    videoView.setFileClip("copy", videoView.fileOpTarget)
                }
            }
        }
        NavigableMenuItem {
            text: qsTr("Move")
            enabled: visible && !MediaLibrary.opBusy
            onTriggered: {
                if (videoView.fileOpTarget) {
                    videoView.setFileClip("move", videoView.fileOpTarget)
                }
            }
        }
        NavigableMenuItem {
            text: qsTr("Delete")
            enabled: visible && !MediaLibrary.opBusy
            onTriggered: deleteFileDialog.open()
        }
    }

    NavigableMessageDialog {
        id: deleteFileDialog
        standardButtons: Dialog.Yes | Dialog.No
        text: {
            var t = videoView.fileOpTarget
            if (!t) {
                return ""
            }
            var shown = (t.name || "").replace(/%/g, "%%")
            var line = qsTr("Delete \"%1\" permanently?").arg(shown)
            if (t.isDir) {
                line += "\n" + qsTr("This folder and everything inside it, including files that are not videos, will be permanently deleted.")
            }
            return line
        }
        onAccepted: videoView.commitDelete(videoView.fileOpTarget)
    }

    // 不設 inputMethodHints：資料夾名稱要能打各種字，跟這一頁的搜尋框一樣（網址框的 ImhUrlCharactersOnly 會擋掉）。
    // 一打開就把焦點放在欄位，頭盔的虛擬鍵盤才有目標（跟重新命名電腦的對話框一樣）。
    NavigableDialog {
        id: newFolderDialog
        property bool forHost: false
        title: qsTr("New folder")
        standardButtons: Dialog.Ok | Dialog.Cancel
        onOpened: folderNameField.forceActiveFocus()
        onClosed: folderNameField.text = ""
        onAccepted: videoView.commitMkdir(forHost, folderNameField.text)
        ColumnLayout {
            TextField {
                id: folderNameField
                Layout.fillWidth: true
                Layout.preferredWidth: 360
                placeholderText: qsTr("Folder name")
                Keys.onReturnPressed: newFolderDialog.accept()
                Keys.onEnterPressed: newFolderDialog.accept()
            }
        }
    }
}
