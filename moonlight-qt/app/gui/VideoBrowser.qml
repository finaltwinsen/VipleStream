import QtQuick 2.9
import QtQuick.Controls 2.2
import QtQuick.Layouts 1.3
import QtWebEngine

import VrLauncher 1.0

// VipleStream 2.0 §VR-PLAYER：影片模式的內建瀏覽器（Qt WebEngine＝Chromium 核心）。在網頁上找到影片之後按
// 「Play in VR」：先暫停網頁裡的影片，再交給 VR 播放器（VrLauncher.launchPlayer）——
//   - 網頁裡的 <video> 直接用 http(s) 網址播的（不是 blob:／MediaSource）：用那個網址直接播；
//   - 其他（YouTube 等用 MediaSource 的網站）：用這一頁的網址，播放器經 yt-dlp 解析（180°／360°／EAC 的判斷照舊）。
// 只在有 Qt WebEngine 的建置（Flatpak 用 io.qt.qtwebengine.BaseApp）由 VideoView 的 Loader 載入；沒有的建置不會碰到這個檔。
Item {
    id: browser

    // VideoView 給：目前選的畫面格式（playerformat 名稱）
    property string formatName: "auto"
    property string homeUrl: VrLauncher.devVideoUrl !== "" ? VrLauncher.devVideoUrl : "https://www.youtube.com/"
    property bool pageHasVideo: false
    signal launchFailed(string message)

    function go(text) {
        var t = text.trim()
        if (t === "") {
            return
        }
        if (!/^[a-zA-Z][a-zA-Z0-9+.-]*:\/\//.test(t)) {
            // 沒有 scheme：像網址的就補 https://，否則當成搜尋
            t = (t.indexOf(".") > 0 && t.indexOf(" ") < 0) ? "https://" + t
                                                         : "https://www.youtube.com/results?search_query=" + encodeURIComponent(t)
        }
        view.url = t
    }

    // 網頁裡正在播（或最大的）那一個 <video>：回傳 {src, title}
    readonly property string probeScript: "(function(){var vs=document.querySelectorAll('video');var best=null,area=0;" +
        "for(var i=0;i<vs.length;i++){var v=vs[i];var a=v.clientWidth*v.clientHeight;if(!v.paused)a+=1e9;if(a>area){area=a;best=v;}}" +
        "for(var j=0;j<vs.length;j++){try{vs[j].pause();}catch(e){}}" +
        "return {src: best?(best.currentSrc||best.src||''):'', title: document.title};})()"

    function playInVr() {
        view.runJavaScript(probeScript, function(r) {
            var src = r && r.src ? r.src : ""
            console.warn("[VIPLE-PLAYER] browser play in VR: page=" + view.url + " videoSrc=" + src.substring(0, 80))
            var direct = /^https?:\/\//.test(src)
            var source = direct ? src : view.url.toString()
            if (!VrLauncher.launchPlayer(source, browser.formatName, r && r.title ? r.title : view.title, "")) {
                browser.launchFailed(qsTr("Could not start the video player."))
            }
        })
    }

    Timer {
        // dev（--dev-video play=<秒>）：自動按「Play in VR」
        running: VrLauncher.devVideoPlaySec > 0
        interval: VrLauncher.devVideoPlaySec * 1000
        onTriggered: browser.playInVr()
    }

    Timer {
        // 有沒有影片：讓按鈕顯眼一點（每兩秒看一次）
        interval: 2000
        repeat: true
        running: browser.visible
        onTriggered: view.runJavaScript("document.querySelectorAll('video').length > 0", function(r) { browser.pageHasVideo = r === true })
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 6

        RowLayout {
            Layout.fillWidth: true
            spacing: 4
            Button { text: qsTr("Back"); enabled: view.canGoBack; onClicked: view.goBack() }
            Button { text: qsTr("Forward"); enabled: view.canGoForward; onClicked: view.goForward() }
            Button { text: view.loading ? qsTr("Stop") : qsTr("Reload"); onClicked: view.loading ? view.stop() : view.reload() }
            Button { text: qsTr("Home"); onClicked: view.url = browser.homeUrl }
            TextField {
                id: address
                Layout.fillWidth: true
                selectByMouse: true
                inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoAutoUppercase
                placeholderText: qsTr("Search or type a web address")
                onAccepted: browser.go(text)
            }
            Button {
                id: playButton
                text: qsTr("Play in VR")
                highlighted: browser.pageHasVideo
                enabled: /^https?:\/\//.test(view.url.toString())
                onClicked: browser.playInVr()
            }
        }
        ProgressBar {
            Layout.fillWidth: true
            visible: view.loading
            from: 0
            to: 100
            value: view.loadProgress
        }

        WebEngineView {
            id: view
            Layout.fillWidth: true
            Layout.fillHeight: true
            url: browser.homeUrl
            // 帳號登入（YouTube 等）與 cookie 留著：用有名字的設定檔（存在 app 的資料夾），不用無痕
            profile: WebEngineProfile {
                storageName: "VipleStream"
                offTheRecord: false
                httpCacheType: WebEngineProfile.DiskHttpCache
                persistentCookiesPolicy: WebEngineProfile.AllowPersistentCookies
            }
            onUrlChanged: {
                if (!address.activeFocus) {
                    address.text = url.toString()
                }
            }
            // 新視窗（target=_blank）在同一頁打開
            onNewWindowRequested: function(request) {
                view.url = request.requestedUrl
            }
            onLoadingChanged: function(info) {
                console.warn("[VIPLE-PLAYER] browser load status=" + info.status + " " + info.url
                             + (info.errorString ? " error=" + info.errorString : ""))
            }
            onRenderProcessTerminated: function(status, code) {
                console.warn("[VIPLE-PLAYER] browser render process terminated: status=" + status + " code=" + code)
            }
            onFullScreenRequested: function(request) {
                request.accept()
            }
        }
    }
}
