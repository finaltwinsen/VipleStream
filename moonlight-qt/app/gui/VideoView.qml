import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.3

import ComputerManager 1.0
import MediaLibrary 1.0
import SdlGamepadKeyNavigation 1.0
import VrLauncher 1.0

// VipleStream 2.0 §VR-PLAYER：影片模式的選片頁。選好之後 VrLauncher.launchPlayer() 另開子行程在頭盔裡播
// （這一頁隱藏，播完再回來）。分頁：最近播過、這台頭盔上的檔案、電腦（VipleStream Server 分享的資料夾）、
// YouTube（搜尋；網址解析用 yt-dlp，在播放器裡做）、網址。畫面格式預設「自動」（播放器會看
// 影片的中繼資料與檔名），也可以在這裡或 VR 裡的控制列改。
Item {
    id: videoView
    objectName: qsTr("Videos")

    property string currentDir: ""
    // 電腦分頁：先選主機（hostIndex＝-1），再像檔案總管一樣瀏覽：hostRoot＝-1 是那台主機的分享資料夾清單，
    // 否則是某個分享資料夾裡的 hostPath（相對路徑，用 / 分隔）。讀取是非同步的（hostRequest 對得上才採用結果）
    property int hostIndex: -1
    property string hostName: ""
    property int hostRoot: -1
    property string hostRootName: ""
    property string hostPath: ""
    property bool hostLoading: false
    // 瀏覽器第一次切到那一頁才載入，之後留著（不重新整理網頁）
    property bool browserWanted: false
    property int hostRequest: 0
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
        fileList.currentIndex = 0
    }

    function refreshHosts() {
        var hs = MediaLibrary.hosts(ComputerManager)
        // 同一批主機只有上下線變了：就地更新（不重建清單，游標與捲動位置不跳）
        var same = hs.length === hostsModel.count
        for (var j = 0; same && j < hs.length; j++) {
            same = hostsModel.get(j).hostIdx === hs[j].index && hostsModel.get(j).name === hs[j].name
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
            hostsModel.append({ "name": hs[i].name, "online": hs[i].online, "hostIdx": hs[i].index })
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
    function openHost(index, name) {
        hostIndex = index
        hostName = name
        openHostDir(-1, "", "")
    }

    // 回到主機清單
    function closeHost() {
        hostIndex = -1
        hostModel.clear()
        hostMessage.text = ""
        refreshHosts()
        hostListView.forceActiveFocus(Qt.TabFocus)
    }

    // root＝-1：分享資料夾清單；rootName 只給路徑列顯示
    function openHostDir(root, path, rootName) {
        hostRoot = root
        hostPath = path
        if (rootName !== undefined && rootName !== "") {
            hostRootName = rootName
        }
        hostModel.clear()
        hostMessage.text = ""
        hostLoading = true
        hostRequest++
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

    function refreshRecent() {
        recentModel.clear()
        var items = MediaLibrary.recent()
        for (var i = 0; i < items.length; i++) {
            recentModel.append(items[i])
        }
    }

    StackView.onActivated: {
        if (VrLauncher.devVideoTab >= 0) {
            console.warn("[VIPLE-PLAYER] dev video page activated: tab=" + VrLauncher.devVideoTab)
        }
        refreshRecent()
        openDir("")
        refreshHosts()
        tabs.currentIndex = VrLauncher.devVideoTab >= 0 ? VrLauncher.devVideoTab : (recentModel.count > 0 ? 0 : 1)
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
        case 0: videoView.scrollList(recentList, pages); break
        case 1: videoView.scrollList(fileList, pages); break
        case 2: videoView.scrollList(videoView.hostIndex < 0 ? hostListView : hostEntryList, pages); break
        case 3: videoView.scrollList(webList, pages); break
        case 4:
            if (browserLoader.item) {
                browserLoader.item.scrollPages(pages)
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

    // 子行程結束、回到這一頁時更新「最近播過」（停在哪裡）
    Connections {
        target: VrLauncher
        function onRunningChanged() {
            if (!VrLauncher.running) {
                refreshRecent()
            }
        }
    }

    // §VR-PLAYER（dev，--dev-video play=<秒>,url=<檔案或網址>）：瀏覽器以外的分頁，幾秒後直接播 url（無人驗測啟動流程用）
    Timer {
        running: VrLauncher.devVideoPlaySec > 0 && VrLauncher.devVideoTab !== 4 && VrLauncher.devVideoUrl !== ""
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

    ListModel { id: recentModel }
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
                                   "format": root < 0 ? "" : e.format, "drive": root < 0 && e.drive === true })
            }
            hostMessage.text = error !== "" ? error : (entries.length === 0 ? qsTr("This folder has no videos.") : "")
            hostEntryList.currentIndex = 0
            hostEntryList.forceActiveFocus(Qt.TabFocus)
            if (videoView.devHostStep === 1 && root < 0 && entries.length > 0) {
                videoView.devHostStep = 2
                videoView.openHostDir(entries[0].index, VrLauncher.devVideoDir === "." ? "" : VrLauncher.devVideoDir, entries[0].name)
            }
        }
        function onWebUpdateFinished(ok, message) {
            webStatus.text = message
            webUpdateButton.enabled = true
            webVersionLabel.text = qsTr("Helper: %1").arg(MediaLibrary.webVersion() || qsTr("not installed"))
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12

        TabBar {
            id: tabs
            Layout.fillWidth: true
            TabButton { text: qsTr("Recent") }
            TabButton { text: qsTr("This headset") }
            TabButton { text: qsTr("PC") }
            TabButton { text: "YouTube" }
            TabButton { text: VrLauncher.browserAvailable ? qsTr("Browser") : qsTr("Link") }
            onCurrentIndexChanged: {
                if (currentIndex === 4) {
                    videoView.browserWanted = true
                }
            }
            Keys.onDownPressed: {
                [recentList, fileList, videoView.hostIndex < 0 ? hostListView : hostEntryList, webQuery, urlField][tabs.currentIndex].forceActiveFocus(Qt.TabFocus)
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

                // ── 最近播過 ──
                ListView {
                    id: recentList
                    clip: true
                    model: recentModel
                    keyNavigationEnabled: true
                    ScrollBar.vertical: ScrollBar {}
                    delegate: ItemDelegate {
                        width: recentList.width
                        highlighted: recentList.activeFocus && recentList.currentIndex === index
                        contentItem: ColumnLayout {
                            spacing: 2
                            Label { text: model.title; font.pointSize: 14; elide: Text.ElideMiddle; Layout.fillWidth: true }
                            Label {
                                text: model.progress + (model.format !== "" ? "   ·   " + model.format : "")
                                opacity: 0.7
                                Layout.fillWidth: true
                                elide: Text.ElideMiddle
                            }
                        }
                        onClicked: play(model.source, model.title)
                        Keys.onReturnPressed: clicked()
                        Keys.onEnterPressed: clicked()
                    }
                    Label {
                        anchors.centerIn: parent
                        visible: recentModel.count === 0
                        text: qsTr("Videos you play will show up here.")
                        opacity: 0.7
                    }
                }

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
                    }
                    ListView {
                        id: fileList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: dirModel
                        keyNavigationEnabled: true
                        ScrollBar.vertical: ScrollBar {}
                        delegate: ItemDelegate {
                            width: fileList.width
                            highlighted: fileList.activeFocus && fileList.currentIndex === index
                            contentItem: RowLayout {
                                spacing: 12
                                Label {
                                    text: model.name + (model.isDir ? "/" : "")
                                    font.pointSize: 14
                                    font.bold: model.isDir
                                    elide: Text.ElideMiddle
                                    Layout.fillWidth: true
                                }
                                Label { text: model.format; opacity: 0.7; visible: model.format !== "" }
                                Label { text: model.detail; opacity: 0.6; elide: Text.ElideLeft; Layout.maximumWidth: 360 }
                            }
                            onClicked: model.isDir ? openDir(model.path) : play(model.path, "")
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
                        visible: videoView.hostIndex < 0
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
                                    Label { text: model.name; font.pointSize: 16; font.bold: true; Layout.fillWidth: true; elide: Text.ElideRight }
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
                        visible: videoView.hostIndex >= 0
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
                        ListView {
                            id: hostEntryList
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            clip: true
                            model: hostModel
                            keyNavigationEnabled: true
                            ScrollBar.vertical: ScrollBar {}
                            delegate: ItemDelegate {
                                width: hostEntryList.width
                                height: 56
                                highlighted: hostEntryList.activeFocus && hostEntryList.currentIndex === index
                                contentItem: RowLayout {
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
                                        font.pointSize: 14
                                        font.bold: model.isDir
                                        elide: Text.ElideMiddle
                                        Layout.fillWidth: true
                                    }
                                    Label { text: model.format; opacity: 0.7; visible: model.format !== "" }
                                    Label { text: model.detail; opacity: 0.6 }
                                }
                                onClicked: {
                                    if (model.isDir) {
                                        openHostDir(model.root, model.path, videoView.hostRoot < 0 ? model.name : "")
                                    }
                                    else {
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

                // ── YouTube ──
                ColumnLayout {
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
                    ListView {
                        id: webList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        model: webModel
                        keyNavigationEnabled: true
                        ScrollBar.vertical: ScrollBar {}
                        delegate: ItemDelegate {
                            width: webList.width
                            highlighted: webList.activeFocus && webList.currentIndex === index
                            contentItem: ColumnLayout {
                                spacing: 2
                                Label { text: model.title; font.pointSize: 14; elide: Text.ElideRight; Layout.fillWidth: true }
                                Label { text: model.channel + (model.duration !== "" ? "   ·   " + model.duration : ""); opacity: 0.7 }
                            }
                            onClicked: play(model.url, model.title)
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

                // ── 瀏覽器（有 Qt WebEngine 的建置；VideoBrowser.qml）或貼網址 ──
                ColumnLayout {
                    spacing: 12
                    Loader {
                        id: browserLoader
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        visible: VrLauncher.browserAvailable
                        // 第一次切到這一頁才載入（Chromium 很吃記憶體）
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
                    Label {
                        visible: !VrLauncher.browserAvailable
                        text: qsTr("Paste a link: a video page (YouTube and many other sites) or a video file or stream (MP4, MKV, WebM, HLS .m3u8).")
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                    }
                    TextField {
                        id: urlField
                        visible: !VrLauncher.browserAvailable
                        Layout.fillWidth: true
                        placeholderText: "https://"
                        inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoAutoUppercase
                        Keys.onReturnPressed: urlPlay.clicked()
                        Keys.onEnterPressed: urlPlay.clicked()
                    }
                    Button {
                        id: urlPlay
                        visible: !VrLauncher.browserAvailable
                        text: qsTr("Play")
                        enabled: MediaLibrary.isUrl(urlField.text)
                        onClicked: play(urlField.text.trim(), "")
                    }
                    Item { Layout.fillHeight: true; visible: !VrLauncher.browserAvailable }
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
                elide: Text.ElideRight
            }
        }
    }
}
